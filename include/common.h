#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "protocol.h"
#include <stdatomic.h>
#include <liburing.h>

/* limits of our pre-allocated memory pools */
#define MAX_CONCURRENT_CLIENTS 4096
#define MAX_OPEN_FILES 10000
#define DICT_CAPACITY 8192
#define EMPTY_FD -1
#define MAX_PAYLOAD_SIZE (8 * 1024 * 1024)
#define MAX_FILENAME_LEN 256

/* The discrete states of our asynchronous machine */
enum ConnectionState {
    STATE_ACCEPT = 0,
    STATE_READ_HEADER,
    STATE_READ_PAYLOAD,
    STATE_SENDFILE,
    STATE_WORKER_HANDOFF,
    STATE_PACING_WAIT,
    STATE_CLEANUP,
    STATE_CANCELLING,
    STATE_ACCEPT_BACKOFF
};

/*
 * The Persistent Client Memory Model
 * Passed to the kernel via io_uring_sqe->user_data so we can resume execution
 * when an interrupt fires.
 */
struct ClientContext {
    int client_socket_fd;
    int target_file_fd;
    enum ConnectionState state;

    struct MediaHeader header;
    uint8_t *payload_buf;            /* Allocated when payload_len > 0; ownership transfers to worker on successful queue push */

    off_t bytes_sent_total;
    off_t bytes_to_send;

    struct __kernel_timespec timeout;
    int pipe_fds[2];

    off_t next_fadvise_offset;
    uint32_t bitrate;
    uint64_t tokens;
    uint64_t last_tx_time_ns;
};

/*
 * Robin Hood Hash Map for files.
 * Now also stores the filename inline so the worker matrix can do
 * substring search without touching a database.
 */
struct FileEntry {
    uint64_t file_id;
    int fd;
    off_t file_size;
    uint32_t bitrate;
    uint16_t probe_dist;
    uint8_t is_occupied;             /* 1 if active, 0 if empty */
    char filename[MAX_FILENAME_LEN]; /* null-terminated; basename only */
};

/* Global pointer to our dictionary, updated with atomic RCU sync */
extern _Atomic(struct FileEntry*) global_dictionary;

/*
 * RCU dictionary API
 * -------------------
 * insert  -- add a new file (or update fd/size/bitrate if file_id already
 *            present, in which case the displaced fd is routed through GC)
 * remove  -- delete a file by id; the removed fd is enqueued for closure
 *            after the grace period. Returns 1 on success, 0 if not found.
 * rename  -- change a file's identity in place; preserves fd/size/bitrate
 *            from the old entry (used by the inotify rename path)
 *
 * internal_insert returns the displaced fd if the file_id was already in
 * the dict (file replaced in place), EMPTY_FD otherwise. Callers in RCU
 * writers route this through GC; callers in the boot path can close it
 * directly (hash collisions only).
 *
 * Real grace periods are now provided via reader_states + rcu_synchronize.
 * See rcu.h for the read-side API.
 */
void rcu_insert_file(uint64_t file_id, int fd, off_t size, uint32_t bitrate, const char* filename);
int  rcu_remove_file(uint64_t file_id);
void rcu_rename_file(uint64_t old_file_id, uint64_t new_file_id, const char* new_filename);
int  internal_insert(struct FileEntry* dict, uint64_t file_id, int fd, off_t size, uint32_t bitrate, const char* filename);

/*
 * Daemon thread entry points. Defined in their respective TUs; declared here
 * so dispatcher's pthread_create call sites and the definitions both see a
 * consistent prototype.
 */
void* logger_daemon_main(void* arg);
void* worker_daemon_main(void* arg);
void* ingestion_daemon_main(void* arg);

#endif /* COMMON_H */
