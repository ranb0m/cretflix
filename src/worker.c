#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sched.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <immintrin.h>

#include "protocol.h"
#include "work_queue.h"
#include "logger.h"
#include "common.h"
#include "rcu.h"

#include <libavformat/avformat.h>
#include <libavutil/log.h>

/* ingestion daemon bookkeeping */
#define INOTIFY_EVENT_SIZE  (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN     (1024 * (INOTIFY_EVENT_SIZE + 256))

#define MAX_PENDING_MOVES   16
#define MAX_SEARCH_RESULTS  32
#define SEARCH_RESPONSE_BUF (8 * 1024)

#define MEDIA_ROOT_DEFAULT  "/mnt/media_test"
#define MAX_PATH_LEN        1024
#define MAX_WATCHES         1024

/*
 * Runtime-configurable media root. Set once at ingestion daemon startup
 * from the MEDIA_ROOT env var, falling back to /mnt/media_test. Both pointer
 * and length are read by hot-path code (relative_path); we initialize before
 * any such reader can run, then never mutate.
 */
static const char *media_root = NULL;
static size_t      media_root_len = 0;

struct WorkQueue global_work_queue = {0};

/*
 * Tracks an unmatched IN_MOVED_FROM event awaiting its IN_MOVED_TO partner.
 * Stores the full path so cross-watch (cross-directory) renames work too.
 */
struct PendingMove {
    uint32_t cookie;
    char full_path[MAX_PATH_LEN];
    int valid;
};

/*
 * Watch table: inotify gives us back a wd (watch descriptor) per directory.
 * When events fire, we get the wd in the event and need to know which
 * directory it refers to in order to construct full paths for files inside.
 * Linear scan is fine — MAX_WATCHES is small relative to event-handling cost.
 */
struct WatchEntry {
    int  wd;
    char path[MAX_PATH_LEN];
    int  valid;
};
static struct WatchEntry watch_table[MAX_WATCHES];

static int register_watch(int wd, const char* path) {
    for (int i = 0; i < MAX_WATCHES; i++) {
        if (!watch_table[i].valid) {
            watch_table[i].wd = wd;
            strncpy(watch_table[i].path, path, MAX_PATH_LEN - 1);
            watch_table[i].path[MAX_PATH_LEN - 1] = '\0';
            watch_table[i].valid = 1;
            return 0;
        }
    }
    return -1; /* table full */
}

static const char* path_for_wd(int wd) {
    for (int i = 0; i < MAX_WATCHES; i++) {
        if (watch_table[i].valid && watch_table[i].wd == wd) {
            return watch_table[i].path;
        }
    }
    return NULL;
}

static void unregister_watch(int wd) {
    for (int i = 0; i < MAX_WATCHES; i++) {
        if (watch_table[i].valid && watch_table[i].wd == wd) {
            watch_table[i].valid = 0;
            return;
        }
    }
}

/*
 * djb2 over the full path. Path is the unit of identity now: two files with
 * the same basename in different subdirs hash distinctly.
 */
static uint64_t hash_path(const char *path) {
    uint64_t hash = 5381;
    int c;
    while ((c = *path++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/*
 * Strip media_root prefix (and the following slash) from a full path.
 * "/mnt/media_test/movies/inception.mkv" → "movies/inception.mkv"
 * The relative form is what we store in FileEntry::filename for display +
 * substring search. Hashing always uses the full path so identity is
 * unambiguous regardless of how the root is configured.
 */
static const char* relative_path(const char* full_path) {
    if (strncmp(full_path, media_root, media_root_len) != 0) return full_path;
    if (full_path[media_root_len] == '/') return full_path + media_root_len + 1;
    return full_path + media_root_len;
}

static int is_valid_media_file(const char* filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext || ext == filename) return 0;

    if (strcasecmp(ext, ".mkv") == 0) return 1;
    if (strcasecmp(ext, ".mp4") == 0) return 1;
    if (strcasecmp(ext, ".webm") == 0) return 1;
    if (strcasecmp(ext, ".mov") == 0) return 1;
    if (strcasecmp(ext, ".avi") == 0) return 1;

    return 0;
}

/* *****************************************************************************
 * Bitrate Probe
 * -----------------------------------------------------------------------------
 * Reads the container's duration via libavformat and returns bytes/sec
 * averaged over the file. Falls back to 0 (= no pacing) on any failure;
 * the dispatcher's token bucket treats bitrate==0 as "send as fast as the
 * network allows," which is graceful degradation for files we couldn't probe.
 *
 * For mkv/mp4/webm/mov/avi the duration lives in the container header, so
 * the probe is fast (~10ms typical). probesize/max_analyze_duration are
 * clamped low so libavformat doesn't read megabytes per file just to find
 * out something already in the header.
 * ****************************************************************************/
static pthread_once_t avformat_init_once = PTHREAD_ONCE_INIT;

static void avformat_init(void) {
    /* Silence libavformat's stderr chatter; keep errors so genuine probe
     * failures are visible during boot if anything looks wrong. */
    av_log_set_level(AV_LOG_ERROR);
}

static uint32_t probe_bitrate(const char* filepath, off_t file_size) {
    pthread_once(&avformat_init_once, avformat_init);

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) return 0;

    /* Cheap probe: container header normally has the duration */
    fmt_ctx->probesize             = 64 * 1024;
    fmt_ctx->max_analyze_duration  = AV_TIME_BASE; /* 1 second */

    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) < 0) {
        /* On failure libavformat frees fmt_ctx and NULLs it; nothing to clean up */
        return 0;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return 0;
    }

    int64_t duration_us = fmt_ctx->duration; /* AV_TIME_BASE units = microseconds */
    avformat_close_input(&fmt_ctx);

    if (duration_us <= 0) return 0;

    /* bytes/sec = file_size * 1_000_000 / duration_us
     * Compute in uint64_t — file_size * 10^6 overflows int32 trivially. */
    uint64_t bps = ((uint64_t)file_size * 1000000ULL) / (uint64_t)duration_us;

    if (bps > UINT32_MAX) return UINT32_MAX;
    return (uint32_t)bps;
}

/* *****************************************************************************
 * Boot-Phase Helper: Recursive Substrate Traversal
 * ****************************************************************************/
/*
 * Watch flags. IN_CREATE picks up new directories (so we can add watches to
 * them as they appear); IN_CLOSE_WRITE picks up newly-finished files. We
 * intentionally don't request IN_IGNORED — the kernel sends it unconditionally
 * when a watch becomes invalid (deleted dir, etc.) and we use it to keep
 * watch_table tidy.
 */
#define INOTIFY_WATCH_FLAGS \
    (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE)

/*
 * Recursive boot scan: for each directory we descend into, register an
 * inotify watch BEFORE walking its contents. That way, any file added to
 * that subdir while we're still walking will trigger a queued event the
 * daemon picks up after publishing the boot dict — at worst we re-ingest a
 * file we already saw (idempotent via internal_insert's match case).
 */
static void boot_scan(const char* current_path, struct FileEntry* batch_dict, int* files_ingested, int inotify_fd) {
    int wd = inotify_add_watch(inotify_fd, current_path, INOTIFY_WATCH_FLAGS);
    if (wd < 0) {
        WORKER_LOG("Failed to add inotify watch on dir (errno=%d). Skipping subtree.",
                   errno, 0, 0, 0);
        return;
    }
    if (register_watch(wd, current_path) < 0) {
        WORKER_LOG("Watch table full at %d entries. Bump MAX_WATCHES.", MAX_WATCHES, 0, 0, 0);
        inotify_rm_watch(inotify_fd, wd);
        return;
    }

    DIR* dir = opendir(current_path);
    if (!dir) {
        WORKER_LOG("Warning: Failed to opendir (errno=%d).", errno, 0, 0, 0);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char filepath[MAX_PATH_LEN];
        int n = snprintf(filepath, sizeof(filepath), "%s/%s", current_path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(filepath)) continue; /* path too long; skip */

        unsigned char d_type = entry->d_type;

        if (d_type == DT_UNKNOWN) {
            struct stat probe;
            if (stat(filepath, &probe) == 0) {
                if (S_ISDIR(probe.st_mode)) d_type = DT_DIR;
                else if (S_ISREG(probe.st_mode)) d_type = DT_REG;
            }
        }

        /* Skip symlinks — avoids infinite recursion on cycles, and avoids
         * watching directories we don't actually own. */
        if (d_type == DT_LNK) continue;

        if (d_type == DT_DIR) {
            boot_scan(filepath, batch_dict, files_ingested, inotify_fd);
        }
        else if (d_type == DT_REG) {
            if (!is_valid_media_file(entry->d_name)) continue;

            int fd = open(filepath, O_RDONLY);
            if (fd < 0) continue;

            struct stat st;
            if (fstat(fd, &st) < 0) {
                close(fd);
                continue;
            }

            uint32_t average_bitrate = probe_bitrate(filepath, st.st_size);
            uint64_t file_id = hash_path(filepath);

            if (*files_ingested >= DICT_CAPACITY) {
                WORKER_LOG("CRITICAL: Dictionary capacity exceeded! Skipping ID: %lu", file_id, 0, 0, 0);
                close(fd);
                break;
            }

            const char* rel = relative_path(filepath);
            int displaced_fd = internal_insert(batch_dict, file_id, fd, st.st_size, average_bitrate, rel);
            if (displaced_fd != EMPTY_FD) {
                /* Boot-time hash collision on the FULL path — astronomically
                 * unlikely with djb2 over distinct strings, but if it happens,
                 * the displaced fd is unreachable so close it. */
                close(displaced_fd);
                WORKER_LOG("Hash collision during boot ingestion for file_id %lu",
                           file_id, 0, 0, 0);
            }

            WORKER_LOG("Ingested ID: %lu | Size: %lu | Bitrate: %u",
                       file_id, (uint64_t)st.st_size, average_bitrate, 0);

            (*files_ingested)++;
        }
    }
    closedir(dir);
}

/* *****************************************************************************
 * Boot Phase: Mass Directory Ingestion (one-shot atomic publish)
 * ****************************************************************************/
static void batch_ingest_directory(const char* root_path, int inotify_fd) {
    struct FileEntry* batch_dict = calloc(DICT_CAPACITY, sizeof(struct FileEntry));
    if (!batch_dict) {
        WORKER_LOG("Fatal OOM during batch dictionary allocation.", 0, 0, 0, 0);
        exit(1);
    }

    int files_ingested = 0;
    boot_scan(root_path, batch_dict, &files_ingested, inotify_fd);

    atomic_store_explicit(&global_dictionary, batch_dict, memory_order_release);
    WORKER_LOG("Batch Ingestion Complete. Published %d files to Dispatcher.", files_ingested, 0, 0, 0);
}

/* *****************************************************************************
 * Inotify helpers: add / remove / rename / new-directory
 *
 * All take FULL paths now. Hashing uses the full path (so subdir collisions
 * vanish); display/search uses the relative path (relative to MEDIA_ROOT).
 * ****************************************************************************/
static void handle_new_file(const char* full_path) {
    /* Brief wait for the writer to flush; IN_CLOSE_WRITE means close() returned
     * but page cache writeback may still be queued. 10ms is empirically OK. */
    usleep(10000);

    int file_fd = open(full_path, O_RDONLY);
    if (file_fd < 0) return;

    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        return;
    }

    uint32_t average_bitrate = probe_bitrate(full_path, st.st_size);
    uint64_t file_id = hash_path(full_path);
    const char* rel = relative_path(full_path);

    rcu_insert_file(file_id, file_fd, st.st_size, average_bitrate, rel);
    WORKER_LOG("[INOTIFY] Indexed new file. ID: %lu", file_id, 0, 0, 0);
}

static void handle_removed_file(const char* full_path) {
    uint64_t file_id = hash_path(full_path);
    if (rcu_remove_file(file_id)) {
        /* The removed entry's fd was enqueued for GC inside rcu_remove_file;
         * logger will close it after the grace period. */
        WORKER_LOG("[INOTIFY] Removed file. ID: %lu", file_id, 0, 0, 0);
    }
}

static void handle_renamed_file(const char* old_full_path, const char* new_full_path) {
    uint64_t old_id = hash_path(old_full_path);
    uint64_t new_id = hash_path(new_full_path);
    const char* new_rel = relative_path(new_full_path);
    rcu_rename_file(old_id, new_id, new_rel);
    WORKER_LOG("[INOTIFY] Renamed. Old ID: %lu, New ID: %lu", old_id, new_id, 0, 0);
}

/*
 * New subdirectory appeared (IN_CREATE|IN_ISDIR or IN_MOVED_TO|IN_ISDIR).
 * Add a watch + recursively scan for any files already inside (the kernel
 * doesn't replay events for files that arrived between the dir existing
 * and our watch being registered, so we have to scan).
 */
static void handle_new_directory(const char* full_path, int inotify_fd) {
    int wd = inotify_add_watch(inotify_fd, full_path, INOTIFY_WATCH_FLAGS);
    if (wd < 0) {
        WORKER_LOG("[INOTIFY] Failed to add watch on new dir (errno=%d)", errno, 0, 0, 0);
        return;
    }
    if (register_watch(wd, full_path) < 0) {
        WORKER_LOG("[INOTIFY] Watch table full; new dir not tracked.", 0, 0, 0, 0);
        inotify_rm_watch(inotify_fd, wd);
        return;
    }
    WORKER_LOG("[INOTIFY] Added watch on new subdir wd=%d", wd, 0, 0, 0);

    /* Scan for any files/dirs already present (race window between dir
     * creation and our watch being installed). */
    DIR* dir = opendir(full_path);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char child_path[MAX_PATH_LEN];
        int n = snprintf(child_path, sizeof(child_path), "%s/%s", full_path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child_path)) continue;

        unsigned char d_type = entry->d_type;
        if (d_type == DT_UNKNOWN) {
            struct stat probe;
            if (stat(child_path, &probe) == 0) {
                if (S_ISDIR(probe.st_mode)) d_type = DT_DIR;
                else if (S_ISREG(probe.st_mode)) d_type = DT_REG;
            }
        }
        if (d_type == DT_LNK) continue;

        if (d_type == DT_DIR) {
            handle_new_directory(child_path, inotify_fd); /* recurse */
        } else if (d_type == DT_REG && is_valid_media_file(entry->d_name)) {
            handle_new_file(child_path);
        }
    }
    closedir(dir);
}

/* *****************************************************************************
 * The Payload Parser (now SQLite-free)
 * ****************************************************************************/
static void process_tlv_payload(uint8_t* payload_buffer, uint32_t total_length, int client_fd, int worker_id) {
    uint32_t current_offset = 0;
    char response[SEARCH_RESPONSE_BUF];

    while (current_offset < total_length) {
        if (total_length - current_offset < sizeof(struct TLV_Item)) {
            WORKER_LOG("Malformed payload: insufficient space for TLV header.", 0, 0, 0, 0);
            break;
        }

        struct TLV_Item* item = (struct TLV_Item*)(payload_buffer + current_offset);

        if (current_offset + sizeof(struct TLV_Item) + item->length > total_length) {
            WORKER_LOG("Malformed payload: TLV length exceeds buffer boundary.", 0, 0, 0, 0);
            break;
        }

        uint8_t* data_ptr = payload_buffer + current_offset + sizeof(struct TLV_Item);

        switch (item->type) {
            case 0x0001: { /* Substring filename search */
                /* Build null-terminated query string, bounded by MAX_FILENAME_LEN */
                char query[MAX_FILENAME_LEN];
                size_t copy_len = item->length < MAX_FILENAME_LEN - 1 ? item->length : MAX_FILENAME_LEN - 1;
                memcpy(query, data_ptr, copy_len);
                query[copy_len] = '\0';

                int written = 0;
                int match_count = 0;
                int reader_id = READER_WORKER_BASE + worker_id;

                /* Real RCU read-side critical section: the dict (and the
                 * filename strings inside each entry) must remain valid for
                 * the duration of this scan. After unlock, GC may free the
                 * dict — by that point we've copied everything we need into
                 * `response`. */
                rcu_read_lock(reader_id);

                struct FileEntry* dict = atomic_load_explicit(&global_dictionary, memory_order_acquire);

                if (!dict) {
                    written = snprintf(response, sizeof(response), "{\"status\":\"NOT_FOUND\"}\n");
                } else {
                    written = snprintf(response, sizeof(response), "{\"status\":\"FOUND\",\"results\":[");

                    for (int i = 0; i < DICT_CAPACITY && match_count < MAX_SEARCH_RESULTS; i++) {
                        if (!dict[i].is_occupied) continue;
                        if (strcasestr(dict[i].filename, query) == NULL) continue;

                        /* Reserve ~64 bytes for the closing "],\"count\":NN}\n" */
                        int remaining = sizeof(response) - written - 64;
                        if (remaining < 256) break;

                        /* TODO: filenames containing '"' or '\' will produce malformed JSON.
                         * Acceptable for friends-tier deployment; add escape pass if it ever bites. */
                        int n = snprintf(response + written, remaining,
                                         "%s{\"file_id\":%lu,\"filename\":\"%s\",\"size\":%lu,\"bitrate\":%u}",
                                         match_count == 0 ? "" : ",",
                                         dict[i].file_id,
                                         dict[i].filename,
                                         (uint64_t)dict[i].file_size,
                                         dict[i].bitrate);
                        if (n < 0 || n >= remaining) break;
                        written += n;
                        match_count++;
                    }

                    if (match_count == 0) {
                        written = snprintf(response, sizeof(response), "{\"status\":\"NOT_FOUND\"}\n");
                    } else {
                        int n = snprintf(response + written, sizeof(response) - written,
                                         "],\"count\":%d}\n", match_count);
                        if (n > 0) written += n;
                    }
                }

                rcu_read_unlock(reader_id);

                WORKER_LOG("Search query, %d matches.", match_count, 0, 0, 0);
                send(client_fd, response, written, 0);
                break;
            }
            case 0x0002: {
                WORKER_LOG("Validating auth token of length %d", item->length, 0, 0, 0);
                break;
            }
            default:
                WORKER_LOG("Unknown TLV type 0x%04X, skipping.", item->type, 0, 0, 0);
                break;
        }

        current_offset += sizeof(struct TLV_Item) + item->length;
    }
}

/* *****************************************************************************
 * Inotify Daemon: Boot Ingestion + Runtime Watch
 * -----------------------------------------------------------------------------
 * Owns ALL ingestion responsibilities: at startup it walks the entire tree
 * (registering inotify watches at each subdir along the way and ingesting
 * files into a fresh batch dict, then publishing it atomically). After that
 * it sits in the inotify event loop, reacting to file/dir creation, deletion,
 * and rename events, and growing/shrinking the watch set as subdirs come and go.
 *
 * Cookie matching for renames: IN_MOVED_FROM and IN_MOVED_TO with the same
 * cookie are the two halves of a rename. They can fire on different watches
 * (cross-directory move), so PendingMove stores the FULL old path. End-of-
 * cycle sweep treats any unmatched MOVED_FROM as a deletion.
 *
 * NOT supported: renaming a directory. The kernel doesn't help us here —
 * files inside the renamed directory all get new effective paths but no
 * events fire for them. Treat dir renames as "don't" for now; mv files
 * individually if you want to reorganize.
 * ****************************************************************************/
void* ingestion_daemon_main(void* arg) {
    (void)arg;

    /* Resolve the media root from the environment exactly once, before any
     * code that reads the globals (boot scan, relative_path) runs. */
    const char *env_root = getenv("MEDIA_ROOT");
    media_root = (env_root && env_root[0]) ? env_root : MEDIA_ROOT_DEFAULT;
    media_root_len = strlen(media_root);
    /* Strip a single trailing slash if present, for join consistency */
    if (media_root_len > 0 && media_root[media_root_len - 1] == '/') {
        media_root_len--;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    int fd = inotify_init();
    if (fd < 0) {
        fprintf(stderr, "Fatal: inotify_init failed.\n");
        return NULL;
    }

    /* Boot scan: walks the tree, adds watches per directory, ingests files
     * into a batch dict, atomically publishes. Until this returns, the
     * dispatcher's boot barrier (waiting on global_dictionary != NULL) holds. */
    batch_ingest_directory(media_root, fd);

    WORKER_LOG("Runtime Ingestion Daemon armed.", 0, 0, 0, 0);

    char buffer[INOTIFY_BUF_LEN];
    struct PendingMove pending[MAX_PENDING_MOVES] = {0};

    while (1) {
        int length = read(fd, buffer, INOTIFY_BUF_LEN);
        if (length < 0) continue;

        int i = 0;
        while (i < length) {
            struct inotify_event* event = (struct inotify_event*)&buffer[i];

            /* Watch removed (kernel auto-cleanup when dir is deleted, or
             * explicit inotify_rm_watch). Drop our table entry. */
            if (event->mask & IN_IGNORED) {
                unregister_watch(event->wd);
                i += INOTIFY_EVENT_SIZE + event->len;
                continue;
            }

            /* Need a name and a known watch dir to do anything */
            const char* watch_dir = path_for_wd(event->wd);
            if (!event->len || event->name[0] == '.' || !watch_dir) {
                i += INOTIFY_EVENT_SIZE + event->len;
                continue;
            }

            /* Construct the full path of the event's subject */
            char full_path[MAX_PATH_LEN];
            int n = snprintf(full_path, sizeof(full_path), "%s/%s", watch_dir, event->name);
            if (n < 0 || n >= (int)sizeof(full_path)) {
                i += INOTIFY_EVENT_SIZE + event->len;
                continue;
            }

            if (event->mask & IN_ISDIR) {
                /* Directory event: only care about creation / move-in (need
                 * a new watch). Deletion auto-fires IN_IGNORED above for the
                 * deleted dir's own watch. */
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                    handle_new_directory(full_path, fd);
                }
            } else {
                /* File event */
                if (event->mask & IN_MOVED_FROM) {
                    if (is_valid_media_file(event->name)) {
                        for (int j = 0; j < MAX_PENDING_MOVES; j++) {
                            if (!pending[j].valid) {
                                pending[j].cookie = event->cookie;
                                snprintf(pending[j].full_path, MAX_PATH_LEN, "%s", full_path);
                                pending[j].valid = 1;
                                break;
                            }
                        }
                    }
                }
                else if (event->mask & IN_MOVED_TO) {
                    if (is_valid_media_file(event->name)) {
                        int matched = 0;
                        for (int j = 0; j < MAX_PENDING_MOVES; j++) {
                            if (pending[j].valid && pending[j].cookie == event->cookie) {
                                handle_renamed_file(pending[j].full_path, full_path);
                                pending[j].valid = 0;
                                matched = 1;
                                break;
                            }
                        }
                        if (!matched) {
                            /* MOVED_TO with no MOVED_FROM partner: file came
                             * from outside the watched tree. Treat as new. */
                            handle_new_file(full_path);
                        }
                    }
                }
                else if (event->mask & IN_CLOSE_WRITE) {
                    if (is_valid_media_file(event->name)) {
                        handle_new_file(full_path);
                    }
                }
                else if (event->mask & IN_DELETE) {
                    if (is_valid_media_file(event->name)) {
                        handle_removed_file(full_path);
                    }
                }
            }

            i += INOTIFY_EVENT_SIZE + event->len;
        }

        /* End-of-cycle sweep: unmatched MOVED_FROM means the file left the
         * watched tree entirely. Treat as deletion. */
        for (int j = 0; j < MAX_PENDING_MOVES; j++) {
            if (pending[j].valid) {
                handle_removed_file(pending[j].full_path);
                pending[j].valid = 0;
            }
        }
    }
    return NULL;
}

/* *****************************************************************************
 * Synchronous Worker Loop
 * ****************************************************************************/
void* worker_daemon_main(void* arg) {
    int worker_id = (int)(intptr_t)arg;

    /* Core 0: Dispatcher, Core 1: Logger/Ingestion. Workers take Cores 2+. */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(worker_id + 2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    /* Payload buffers now arrive pre-read from the dispatcher via JobPayload.
     * No local buffer needed; worker is purely CPU-bound from this point. */

    /* Boot ingestion has moved to the ingestion daemon (single writer to the
     * dictionary). Workers just enter the work loop and wait for jobs. */

    struct JobPayload job;
    int spin_count = 0;

    WORKER_LOG("Worker Thread %d active on Core %d.", worker_id, worker_id + 2, 0, 0);

    while (1) {
        if (work_queue_pop(&job) == 0) {
            spin_count = 0;

            /* Payload was pre-read by the dispatcher via io_uring; process directly. */
            if (job.payload != NULL && job.header.payload_len > 0) {
                process_tlv_payload(job.payload, job.header.payload_len, job.client_fd, worker_id);
                free(job.payload);
            }
            close(job.client_fd);
        } else {
            spin_count++;
            if (spin_count < 1000) {
                _mm_pause();
            } else {
                usleep(100);
                spin_count = 0;
            }
        }
    }

    return NULL;
}
