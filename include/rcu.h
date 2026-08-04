#ifndef RCU_H
#define RCU_H

#include <stdint.h>
#include <stdatomic.h>

/*
 * Userspace RCU via per-reader sequence counters (QSBR-lite).
 *
 * Each registered reader has a sequence counter. Readers increment it on
 * entry to a critical section (making it odd) and again on exit (back to
 * even). The writer can snapshot all reader counters; for any reader that
 * was odd at snapshot time, the writer waits until that counter changes,
 * proving the reader has exited and released any pointer it had captured.
 *
 * Idle readers (even at snapshot) need no wait because the publish that
 * preceded the synchronize was release-ordered: any subsequent critical
 * section those readers enter will load the new pointer, not the old one.
 */

#define MAX_READERS 64

#define READER_DISPATCHER  0
#define READER_WORKER_BASE 1
/* Worker N maps to READER_WORKER_BASE + N */

struct ReaderState {
    _Atomic(uint64_t) seq;
    char pad[64 - sizeof(_Atomic(uint64_t))];
} __attribute__((aligned(64)));

extern struct ReaderState reader_states[MAX_READERS];

/*
 * Reader-side primitives. Pattern:
 *
 *     rcu_read_lock(READER_X);
 *     ptr = atomic_load_explicit(&shared, memory_order_acquire);
 *     ... use *ptr (must NOT escape the unlock) ...
 *     rcu_read_unlock(READER_X);
 *
 * If a value extracted from *ptr (e.g. an fd) needs to outlive the read-lock,
 * the reader must take ownership of it inside the critical section (e.g. dup
 * the fd) so the writer's eventual cleanup of *ptr's resources is safe.
 */
static inline void rcu_read_lock(int reader_id) {
    uint64_t s = atomic_load_explicit(&reader_states[reader_id].seq, memory_order_relaxed);
    /* Increment to odd; release ordering pairs with the synchronize's acquire
     * load and prevents the dict-pointer load from reordering before this. */
    atomic_store_explicit(&reader_states[reader_id].seq, s + 1, memory_order_release);
}

static inline void rcu_read_unlock(int reader_id) {
    uint64_t s = atomic_load_explicit(&reader_states[reader_id].seq, memory_order_relaxed);
    /* Increment to even; release ordering ensures all prior reads from the
     * dict happen-before this announcement of "I am no longer reading". */
    atomic_store_explicit(&reader_states[reader_id].seq, s + 1, memory_order_release);
}

/*
 * Writer-side: blocks until all readers that were in a critical section at
 * call time have left it. Called from the logger thread before freeing old
 * dicts.
 */
void rcu_synchronize(void);

/*
 * GC queue. SPSC ring buffer: ingestion daemon enqueues, logger daemon
 * drains. Each entry holds an old dict pointer (to free) and at most one
 * fd to close after the grace period.
 */
void gc_enqueue(void* dict, int fd_to_close);
void gc_drain(void);

#endif /* RCU_H */
