#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "logger.h"
#include "common.h"
#include "rcu.h"

/*
 * Logger Daemon (Core 1)
 *
 * Each loop iteration drains, in order:
 *   1. Dispatcher SPSC log queue   -> stdout
 *   2. Worker      MPSC log queue  -> stdout
 *   3. RCU GC queue (gc_drain)     -> free old dicts, close removed fds
 *
 * The GC drain is now backed by a real grace-period mechanism (rcu_synchronize
 * in rcu.h); the previous sleep(1)-based "fake grace period" + single-slot
 * rcu_garbage_bin is gone.
 */

struct LogQueue dispatcher_log_queue = {0};
struct LogQueue worker_log_queue     = {0};

/*
 * SPSC drain: dispatcher's writer publishes everything via the release-store
 * on tail, so once we observe the new tail (via acquire), the slot is fully
 * written. format_string is the first field assigned, never used as a commit
 * flag in the SPSC path.
 */
static void drain_dispatcher_log(void) {
    while (1) {
        uint32_t head = atomic_load_explicit(&dispatcher_log_queue.head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&dispatcher_log_queue.tail, memory_order_acquire);

        if (head == tail) return;

        uint32_t index = head & (LOG_QUEUE_SIZE - 1);
        struct LogEvent* entry = &dispatcher_log_queue.events[index];

        if (entry->format_string) {
            fprintf(stdout, "[DISP] ");
            fprintf(stdout, entry->format_string, entry->arg1, entry->arg2, entry->arg3, entry->arg4);
            fputc('\n', stdout);
            entry->format_string = NULL; /* defensive; not required for SPSC */
        }

        atomic_store_explicit(&dispatcher_log_queue.head, head + 1, memory_order_release);
    }
}

/*
 * MPSC drain: worker producers fetch_add(tail) to claim a slot, then write
 * args, then a release fence, then format_string LAST as the commit flag.
 * Tail advancing past a slot does NOT mean the slot is committed yet — we
 * must check format_string per slot. NULL means "producer hasn't finished
 * writing this one"; bail and try again on the next pass.
 *
 * NOTE: known issue — if a producer claims a slot via fetch_add and then
 * never gets around to writing format_string (crash, ENOMEM, anything),
 * this drain stalls on that slot forever. Slated for the MPSC abandoned-slot
 * fix in a future pass.
 */
static void drain_worker_log(void) {
    while (1) {
        uint32_t head = atomic_load_explicit(&worker_log_queue.head, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&worker_log_queue.tail, memory_order_acquire);

        if (head == tail) return;

        uint32_t index = head & (LOG_QUEUE_SIZE - 1);
        struct LogEvent* entry = &worker_log_queue.events[index];

        const char* fmt = entry->format_string;
        if (!fmt) {
            /* Producer hasn't committed this slot; come back next iteration. */
            return;
        }
        /* Pair with producer's atomic_thread_fence(release) before writing fmt */
        atomic_thread_fence(memory_order_acquire);

        fprintf(stdout, "[WORK] ");
        fprintf(stdout, fmt, entry->arg1, entry->arg2, entry->arg3, entry->arg4);
        fputc('\n', stdout);

        /* Clear commit flag so a future producer landing on this slot via
         * tail wraparound writes a fresh fmt_str cleanly. */
        entry->format_string = NULL;

        atomic_store_explicit(&worker_log_queue.head, head + 1, memory_order_release);
    }
}

void* logger_daemon_main(void* arg) {
    (void)arg;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    fprintf(stdout, "[LOGGER] Daemon active on Core 1.\n");
    fflush(stdout);

    while (1) {
        drain_dispatcher_log();
        drain_worker_log();

        /* Real grace-period-backed GC. Fast path is two atomic loads + early
         * return when the queue is empty; with pending dicts, runs one
         * rcu_synchronize and frees the whole batch. */
        gc_drain();

        fflush(stdout);
        usleep(1000); /* 1ms idle backoff */
    }

    return NULL;
}
