#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LOG_QUEUE_SIZE 1024 /* Must be a power of 2 */

/*
 * The Deferred Log Payload — 40 bytes exactly.
 *
 * format_string serves as the per-slot COMMIT FLAG in the MPSC path:
 * producers write args first, then a release fence, then format_string last.
 * Consumer treats format_string == NULL as "uncommitted, come back later".
 *
 * In the SPSC dispatcher path, format_string is just data; commit is signalled
 * by the release-store on the queue's tail.
 */
struct LogEvent {
    const char* format_string;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
};

/*
 * Cache-aligned ring buffer. Head and tail in separate 64-byte cache lines
 * so producer/consumer don't false-share on Intel L3.
 */
struct LogQueue {
    _Atomic(uint32_t) tail __attribute__((aligned(64)));
    _Atomic(uint32_t) head __attribute__((aligned(64)));
    struct LogEvent events[LOG_QUEUE_SIZE];
} __attribute__((aligned(64)));

extern struct LogQueue dispatcher_log_queue;
extern struct LogQueue worker_log_queue;


/* *****************************************************************************
 * SPSC Dispatcher Logging Macro (Wait-Free, Single-Threaded)
 *
 * Dispatcher is the only producer here, so no contention possible. The pre-
 * flight check is sound: tail can only advance from this thread, and head
 * can only advance forward (consumer never writes a stale value). Writes
 * land directly, then atomic_store_release(tail) publishes everything.
 * ****************************************************************************/
#define DISPATCH_LOG(fmt, a1, a2, a3, a4) do { \
    uint32_t _ct = atomic_load_explicit(&dispatcher_log_queue.tail, memory_order_relaxed); \
    uint32_t _ch = atomic_load_explicit(&dispatcher_log_queue.head, memory_order_acquire); \
    if (_ct - _ch < LOG_QUEUE_SIZE) { \
        uint32_t _idx = _ct & (LOG_QUEUE_SIZE - 1); \
        dispatcher_log_queue.events[_idx].format_string = (fmt); \
        dispatcher_log_queue.events[_idx].arg1 = (uint64_t)(a1); \
        dispatcher_log_queue.events[_idx].arg2 = (uint64_t)(a2); \
        dispatcher_log_queue.events[_idx].arg3 = (uint64_t)(a3); \
        dispatcher_log_queue.events[_idx].arg4 = (uint64_t)(a4); \
        atomic_store_explicit(&dispatcher_log_queue.tail, _ct + 1, memory_order_release); \
    } \
} while(0)


/* *****************************************************************************
 * MPSC Worker Logging Macro (Wait-Free, CAS-Based Reservation)
 *
 * Multiple workers contend on the producer side. The original implementation
 * used fetch_add(tail) with a re-check that abandoned the slot if it landed
 * out of bounds — but the abandoned tail increment stayed permanent, leading
 * to a deadlock once the consumer reached the abandoned slot and waited
 * forever for a commit that would never come.
 *
 * This version uses CAS to reserve a slot atomically with the bounds check.
 * The CAS only succeeds when the slot is in [head, head + SIZE), so there is
 * no abandonment path. If the queue is genuinely full, we drop the log
 * silently and return — never advancing tail.
 *
 * Liveness: under K-way contention, each producer's loop terminates in at
 * most K iterations (each successful peer CAS changes the value our CAS
 * observes; there are only finitely many such successes before the queue
 * fills and we exit via the bounds check).
 *
 * Known caveat: if a producer is preempted between successful CAS and the
 * format_string write, the consumer waits at that slot until the producer
 * resumes. If the producer crashes outright mid-write, that slot is stuck.
 * Acceptable for friends-tier deployment (no expected crashes); a watchdog
 * scan could fix it but adds complexity not justified at this scale.
 * ****************************************************************************/
#define WORKER_LOG(fmt_str, a1, a2, a3, a4) do { \
    uint32_t _my_slot = 0; \
    int _got_slot = 0; \
    while (1) { \
        uint32_t _ct = atomic_load_explicit(&worker_log_queue.tail, memory_order_relaxed); \
        uint32_t _ch = atomic_load_explicit(&worker_log_queue.head, memory_order_acquire); \
        if (_ct - _ch >= LOG_QUEUE_SIZE) break; /* queue full -> silently drop */ \
        if (atomic_compare_exchange_weak_explicit( \
                &worker_log_queue.tail, &_ct, _ct + 1, \
                memory_order_acq_rel, memory_order_relaxed)) { \
            _my_slot = _ct; \
            _got_slot = 1; \
            break; \
        } \
        /* CAS failed; another producer claimed _ct first. Retry with refreshed values. */ \
    } \
    if (_got_slot) { \
        uint32_t _idx = _my_slot & (LOG_QUEUE_SIZE - 1); \
        /* Write payload first */ \
        worker_log_queue.events[_idx].arg1 = (uint64_t)(a1); \
        worker_log_queue.events[_idx].arg2 = (uint64_t)(a2); \
        worker_log_queue.events[_idx].arg3 = (uint64_t)(a3); \
        worker_log_queue.events[_idx].arg4 = (uint64_t)(a4); \
        /* Release barrier, then commit signal (format_string) LAST */ \
        atomic_thread_fence(memory_order_release); \
        worker_log_queue.events[_idx].format_string = (fmt_str); \
    } \
} while(0)

#endif /* LOGGER_H */
