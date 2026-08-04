#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <stdint.h>
#include <stdatomic.h>
#include <emmintrin.h>      /* For _mm_pause() */
#include "protocol.h"

#define WORK_QUEUE_SIZE 1024 /* Must be a power of 2 */

/*
 * JobPayload now carries the pre-read payload buffer pointer. Dispatcher
 * allocates the buffer and reads into it via io_uring; worker takes ownership
 * on a successful pop and is responsible for free()'ing it after processing.
 */
struct JobPayload {
    int client_fd;
    struct MediaHeader header;
    uint8_t *payload;          /* NULL if no payload; worker frees if non-NULL */
};

/*
 * The SPMC Cache-Aligned Ring Buffer
 * Head and Tail are strictly isolated into separate 64-byte cache lines
 * to absolutely eradicate False Sharing across the Intel L3 cache.
 */
struct WorkQueue {
    _Atomic(uint32_t) tail __attribute__((aligned(64))); /* Dispatcher (Producer) writes here */
    _Atomic(uint32_t) head __attribute__((aligned(64))); /* Workers (Consumers) CAS here */

    struct JobPayload jobs[WORK_QUEUE_SIZE];
} __attribute__((aligned(64)));

extern struct WorkQueue global_work_queue;

/* Dispatcher invokes this: Wait-Free enqueue */
static inline int work_queue_push(int client_fd, struct MediaHeader* header, uint8_t* payload) {
    uint32_t current_tail = atomic_load_explicit(&global_work_queue.tail, memory_order_relaxed);
    uint32_t current_head = atomic_load_explicit(&global_work_queue.head, memory_order_acquire);

    /* Queue full → reject */
    if (current_tail - current_head >= WORK_QUEUE_SIZE) return -1;
    uint32_t index = current_tail & (WORK_QUEUE_SIZE - 1);

    global_work_queue.jobs[index].client_fd = client_fd;
    global_work_queue.jobs[index].header = *header;
    global_work_queue.jobs[index].payload = payload;

    /* Publish to workers; release pairs with consumer's acquire on tail */
    atomic_store_explicit(&global_work_queue.tail, current_tail + 1, memory_order_release);
    return 0;
}

/* Workers invoke this: Lock-Free dequeue via Hardware CAS */
static inline int work_queue_pop(struct JobPayload* out_job) {
    uint32_t current_head = atomic_load_explicit(&global_work_queue.head, memory_order_relaxed);

    while (1) {
        uint32_t current_tail = atomic_load_explicit(&global_work_queue.tail, memory_order_acquire);

        /* Queue empty */
        if (current_head == current_tail) return -1;
        uint32_t index = current_head & (WORK_QUEUE_SIZE - 1);
        *out_job = global_work_queue.jobs[index];

        /*
         * CAS to claim this slot. If another worker beat us, current_head is
         * updated to the new value and we re-loop. *out_job from the losing
         * iteration is discarded — only successful CAS exits the function with
         * a valid copy.
         */
        if (atomic_compare_exchange_weak_explicit(
                &global_work_queue.head,
                &current_head,
                current_head + 1,
                memory_order_acq_rel,
                memory_order_relaxed)) {
            return 0;
        }

        /* CAS failed; yield to sibling hyperthread before retry */
        _mm_pause();
    }
}

#endif /* WORK_QUEUE_H */
