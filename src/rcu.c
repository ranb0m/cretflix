#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>
#include <emmintrin.h>

#include "common.h"
#include "rcu.h"

/* The single RCU-protected dictionary pointer */
_Atomic(struct FileEntry*) global_dictionary = NULL;

/* Reader sequence counters; static-zero-initialized, all readers idle */
struct ReaderState reader_states[MAX_READERS];

/* ---------------------------------------------------------------------------
 * GC queue: SPSC ring buffer
 *
 * Producer = ingestion daemon (single-threaded RCU writer).
 * Consumer = logger daemon.
 * Capacity is small but fits well above any realistic burst of inotify
 * events. If somehow the producer outruns the consumer, gc_enqueue blocks
 * rather than dropping the pointer.
 * ------------------------------------------------------------------------- */

#define GC_QUEUE_SIZE 64

struct GCSlot {
    void* dict;
    int   fd_to_close;
};

static struct {
    _Atomic(uint32_t) tail __attribute__((aligned(64)));
    _Atomic(uint32_t) head __attribute__((aligned(64)));
    struct GCSlot slots[GC_QUEUE_SIZE];
} gc_queue __attribute__((aligned(64)));


/* ---------------------------------------------------------------------------
 * Dictionary mutation
 * ------------------------------------------------------------------------- */

/*
 * Insert/update a slot. Returns the displaced fd if the file_id was already
 * present (file replaced in place), EMPTY_FD otherwise. The displaced-fd
 * case fires only for the originally-inserting item; Robin Hood swaps that
 * happen during probing reposition existing entries but never displace them
 * (each file_id is unique in a valid dict).
 */
int internal_insert(struct FileEntry* dict, uint64_t file_id, int fd, off_t size, uint32_t bitrate, const char* filename) {
    uint64_t hash = file_id * 2654435761;
    uint32_t index = hash & (DICT_CAPACITY - 1);

    struct FileEntry new_entry = {
        .file_id = file_id,
        .fd = fd,
        .file_size = size,
        .bitrate = bitrate,
        .probe_dist = 0,
        .is_occupied = 1
    };
    strncpy(new_entry.filename, filename, MAX_FILENAME_LEN - 1);
    new_entry.filename[MAX_FILENAME_LEN - 1] = '\0';

    while (1) {
        if (!dict[index].is_occupied) {
            dict[index] = new_entry;
            return EMPTY_FD;
        }

        if (dict[index].file_id == file_id) {
            /* In-place replacement. Capture the old fd so the caller can
             * route it through GC; overwrite metadata. */
            int displaced = dict[index].fd;
            dict[index].fd = fd;
            dict[index].file_size = size;
            dict[index].bitrate = bitrate;
            strncpy(dict[index].filename, filename, MAX_FILENAME_LEN - 1);
            dict[index].filename[MAX_FILENAME_LEN - 1] = '\0';
            return displaced;
        }

        if (dict[index].probe_dist < new_entry.probe_dist) {
            struct FileEntry temp = dict[index];
            dict[index] = new_entry;
            new_entry = temp;
        }

        index = (index + 1) & (DICT_CAPACITY - 1);
        new_entry.probe_dist++;
    }
}

/* ---------------------------------------------------------------------------
 * RCU writers: produce a new dict via copy, publish, enqueue old dict for GC
 *
 * All three writers follow the same shape:
 *   1. Allocate new_dict
 *   2. Walk old_dict, populate new_dict
 *   3. Apply the actual mutation
 *   4. atomic_store_release(global_dictionary, new_dict)  -- publish
 *   5. gc_enqueue(old_dict, displaced_or_removed_fd)      -- defer cleanup
 *
 * Only ingestion daemon calls these at runtime; boot path uses direct
 * publish (no old dict to GC).
 * ------------------------------------------------------------------------- */

void rcu_insert_file(uint64_t file_id, int fd, off_t size, uint32_t bitrate, const char* filename) {
    struct FileEntry* new_dict = calloc(DICT_CAPACITY, sizeof(struct FileEntry));
    if (!new_dict) {
        fprintf(stderr, "rcu_insert_file: OOM. Aborting ingestion of %s.\n", filename);
        return;
    }

    struct FileEntry* old_dict = atomic_load_explicit(&global_dictionary, memory_order_relaxed);
    if (old_dict) {
        for (int i = 0; i < DICT_CAPACITY; i++) {
            if (old_dict[i].is_occupied) {
                /* Each call inserts a unique file_id into a fresh dict;
                 * cannot displace, return value always EMPTY_FD. */
                internal_insert(new_dict, old_dict[i].file_id, old_dict[i].fd,
                                old_dict[i].file_size, old_dict[i].bitrate,
                                old_dict[i].filename);
            }
        }
    }

    /* The new entry. THIS call may displace an existing fd if the file_id
     * was already present (file replaced in place). */
    int displaced_fd = internal_insert(new_dict, file_id, fd, size, bitrate, filename);

    atomic_store_explicit(&global_dictionary, new_dict, memory_order_release);

    if (old_dict) {
        gc_enqueue(old_dict, displaced_fd);
    } else if (displaced_fd != EMPTY_FD) {
        /* No old dict but displacement happened? Logically impossible — but
         * if we ever hit it (paranoia), don't leak the fd. */
        close(displaced_fd);
    }
}

int rcu_remove_file(uint64_t file_id) {
    struct FileEntry* old_dict = atomic_load_explicit(&global_dictionary, memory_order_relaxed);
    if (!old_dict) return 0;

    struct FileEntry* new_dict = calloc(DICT_CAPACITY, sizeof(struct FileEntry));
    if (!new_dict) {
        fprintf(stderr, "rcu_remove_file: OOM. File %lu not removed.\n", file_id);
        return 0;
    }

    int removed_fd = EMPTY_FD;

    for (int i = 0; i < DICT_CAPACITY; i++) {
        if (!old_dict[i].is_occupied) continue;

        if (old_dict[i].file_id == file_id) {
            removed_fd = old_dict[i].fd;
            continue; /* skip; this is the entry being dropped */
        }

        internal_insert(new_dict, old_dict[i].file_id, old_dict[i].fd,
                        old_dict[i].file_size, old_dict[i].bitrate,
                        old_dict[i].filename);
    }

    if (removed_fd == EMPTY_FD) {
        /* file_id not found; throw away new_dict, don't swap */
        free(new_dict);
        return 0;
    }

    atomic_store_explicit(&global_dictionary, new_dict, memory_order_release);
    gc_enqueue(old_dict, removed_fd);
    return 1;
}

void rcu_rename_file(uint64_t old_file_id, uint64_t new_file_id, const char* new_filename) {
    struct FileEntry* old_dict = atomic_load_explicit(&global_dictionary, memory_order_relaxed);
    if (!old_dict) return;

    struct FileEntry* new_dict = calloc(DICT_CAPACITY, sizeof(struct FileEntry));
    if (!new_dict) {
        fprintf(stderr, "rcu_rename_file: OOM. File %lu not renamed.\n", old_file_id);
        return;
    }

    int found = 0;
    for (int i = 0; i < DICT_CAPACITY; i++) {
        if (!old_dict[i].is_occupied) continue;

        if (old_dict[i].file_id == old_file_id) {
            /* fd preserved; just changes identity */
            internal_insert(new_dict, new_file_id, old_dict[i].fd,
                            old_dict[i].file_size, old_dict[i].bitrate,
                            new_filename);
            found = 1;
        } else {
            internal_insert(new_dict, old_dict[i].file_id, old_dict[i].fd,
                            old_dict[i].file_size, old_dict[i].bitrate,
                            old_dict[i].filename);
        }
    }

    if (!found) {
        free(new_dict);
        return;
    }

    atomic_store_explicit(&global_dictionary, new_dict, memory_order_release);
    /* No fd to close: same fd lives in the new dict */
    gc_enqueue(old_dict, EMPTY_FD);
}


/* ---------------------------------------------------------------------------
 * Grace period
 * ------------------------------------------------------------------------- */

void rcu_synchronize(void) {
    uint64_t snapshot[MAX_READERS];

    /* Snapshot every reader's seq. Acquire ordering pairs with the readers'
     * release stores on entry/exit, so we observe the latest published seq. */
    for (int i = 0; i < MAX_READERS; i++) {
        snapshot[i] = atomic_load_explicit(&reader_states[i].seq, memory_order_acquire);
    }

    /* For each reader that was in a critical section (odd seq), wait for it
     * to advance past that value. Readers that were idle are skipped: their
     * next critical section will load the new dict pointer, since the
     * publish that preceded this synchronize was release-ordered. */
    for (int i = 0; i < MAX_READERS; i++) {
        if ((snapshot[i] & 1) == 0) continue; /* idle */

        int spin = 0;
        while (atomic_load_explicit(&reader_states[i].seq, memory_order_acquire) == snapshot[i]) {
            if (++spin < 1000) {
                _mm_pause();
            } else {
                /* Reader hasn't advanced after a long spin; yield core */
                sched_yield();
                spin = 0;
            }
        }
    }
}


/* ---------------------------------------------------------------------------
 * GC queue: enqueue (writer side), drain (consumer side)
 * ------------------------------------------------------------------------- */

void gc_enqueue(void* dict, int fd_to_close) {
    uint32_t t = atomic_load_explicit(&gc_queue.tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&gc_queue.head, memory_order_acquire);

    /* Block if the queue is full. This shouldn't happen at our scale — it
     * would mean the logger is wedged or readers are stuck indefinitely. */
    while (t - h >= GC_QUEUE_SIZE) {
        usleep(1000);
        h = atomic_load_explicit(&gc_queue.head, memory_order_acquire);
    }

    uint32_t idx = t & (GC_QUEUE_SIZE - 1);
    gc_queue.slots[idx].dict = dict;
    gc_queue.slots[idx].fd_to_close = fd_to_close;

    /* Release on tail pairs with the consumer's acquire load below */
    atomic_store_explicit(&gc_queue.tail, t + 1, memory_order_release);
}

void gc_drain(void) {
    uint32_t h = atomic_load_explicit(&gc_queue.head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&gc_queue.tail, memory_order_acquire);

    if (h == t) return; /* fast path: nothing queued */

    /*
     * One synchronize covers the entire batch. By the time it returns, no
     * reader is using any pointer that was published before this call;
     * every dict currently in the queue (which was a snapshot at acquire
     * time above) is safe to free.
     */
    rcu_synchronize();

    while (h != t) {
        struct GCSlot* slot = &gc_queue.slots[h & (GC_QUEUE_SIZE - 1)];
        if (slot->fd_to_close != EMPTY_FD) {
            close(slot->fd_to_close);
        }
        free(slot->dict);
        h++;
    }

    atomic_store_explicit(&gc_queue.head, h, memory_order_release);
}
