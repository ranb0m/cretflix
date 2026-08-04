#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>
#include <sys/un.h>
#include <pthread.h>
#include <sched.h>
#include "logger.h"
#include "protocol.h"
#include "common.h"
#include "work_queue.h"
#include "rcu.h"

#define QUEUE_DEPTH 4096
#define PORT_DEFAULT 8080
static int server_port = PORT_DEFAULT;

/*
 * NVMe operations are done in bursts to maximize DMA efficiency
 * (not for thermal management — APST is disabled separately).
 */
#define BURST_SIZE (256 * 1024 * 1024) /* 256 MB */

struct io_uring ring;
int server_fd;


/***************************************************
 *                  HELPERS                        *
 **************************************************/

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void free_ctx(struct ClientContext* ctx) {
    if (!ctx) return;
    if (ctx->payload_buf) free(ctx->payload_buf);
    free(ctx);
}

/*
 * Look up a file by id and return a fresh fd duplicated from the dict's
 * stored fd. The dup is critical: it gives the dispatcher its own fd that
 * survives any subsequent GC of the dict (which would close the original
 * fd). The dispatcher closes its dup when the streaming session ends.
 *
 * dup() syscall happens inside the read-lock, so the entry's fd is
 * guaranteed to still be open at the moment of the dup. After unlock, GC
 * may close the original at any time — the dup remains valid.
 */
static int lookup_file_metadata(uint64_t target_file_id, uint32_t* out_bitrate) {
    int result_fd = EMPTY_FD;

    rcu_read_lock(READER_DISPATCHER);

    struct FileEntry* current_dict = atomic_load_explicit(&global_dictionary, memory_order_acquire);
    if (current_dict) {
        uint64_t hash = target_file_id * 2654435761;
        uint32_t index = hash & (DICT_CAPACITY - 1);
        uint32_t current_probe_dist = 0;

        while (1) {
            struct FileEntry* entry = &current_dict[index];

            if (!entry->is_occupied) break;

            if (entry->file_id == target_file_id) {
                *out_bitrate = entry->bitrate;
                /* dup() inside the read-lock; the original entry->fd is
                 * still open here. The result_fd is the dispatcher's own
                 * private fd from this point on. */
                result_fd = dup(entry->fd);
                break;
            }

            if (current_probe_dist > entry->probe_dist) break;

            index = (index + 1) & (DICT_CAPACITY - 1);
            current_probe_dist++;
        }
    }

    rcu_read_unlock(READER_DISPATCHER);
    return result_fd;
}

static struct ClientContext* allocate_context(int fd, enum ConnectionState state) {
    struct ClientContext* ctx = calloc(1, sizeof(struct ClientContext));
    if (!ctx) {
        DISPATCH_LOG("OOM allocating context for FD %d. Dropping client.", fd, 0, 0, 0);
        return NULL;
    }
    ctx->client_socket_fd = fd;
    ctx->state = state;

    ctx->timeout.tv_sec = 5;
    ctx->timeout.tv_nsec = 0;

    return ctx;
}

static void add_accept_request(struct ClientContext* accept_ctx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, accept_ctx);
}

static void add_read_header_request(struct ClientContext* ctx) {
    struct io_uring_sqe *sqe_read = io_uring_get_sqe(&ring);
    io_uring_prep_recv(sqe_read, ctx->client_socket_fd, &ctx->header, sizeof(struct MediaHeader), MSG_WAITALL);
    sqe_read->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_read, ctx);

    ctx->timeout.tv_sec = 5;
    ctx->timeout.tv_nsec = 0;

    struct io_uring_sqe *sqe_timeout = io_uring_get_sqe(&ring);
    io_uring_prep_link_timeout(sqe_timeout, &ctx->timeout, 0);
    io_uring_sqe_set_data(sqe_timeout, NULL);
}

static void add_read_payload_request(struct ClientContext* ctx) {
    struct io_uring_sqe *sqe_read = io_uring_get_sqe(&ring);
    io_uring_prep_recv(sqe_read, ctx->client_socket_fd,
                       ctx->payload_buf, ctx->header.payload_len, MSG_WAITALL);
    sqe_read->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_read, ctx);

    ctx->timeout.tv_sec = 5;
    ctx->timeout.tv_nsec = 0;

    struct io_uring_sqe *sqe_timeout = io_uring_get_sqe(&ring);
    io_uring_prep_link_timeout(sqe_timeout, &ctx->timeout, 0);
    io_uring_sqe_set_data(sqe_timeout, NULL);
}

static void add_cancel_request(struct ClientContext* ctx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_cancel(sqe, ctx, 0);
    ctx->state = STATE_CANCELLING;
    io_uring_sqe_set_data(sqe, ctx);
}

static void add_sendfile_request(struct ClientContext* ctx) {
    off_t remaining = ctx->bytes_to_send - ctx->bytes_sent_total;
    off_t current_offset = ctx->header.start_byte + ctx->bytes_sent_total;

    size_t chunk_size = remaining > 1048576 ? 1048576 : remaining;

    if (ctx->bitrate > 0) {
        uint64_t now = get_time_ns();
        uint64_t delta_ns = now - ctx->last_tx_time_ns;
        ctx->last_tx_time_ns = now;

        ctx->tokens += (delta_ns * ctx->bitrate) / 1000000000ULL;

        uint64_t max_tokens = ctx->bitrate * 2;
        if (ctx->tokens > max_tokens) ctx->tokens = max_tokens;

        if (chunk_size > max_tokens) chunk_size = max_tokens;

        size_t min_burst = remaining > 65536 ? 65536 : remaining;
        /* If max_tokens < min_burst (low-bitrate file), the wait would never
         * complete — tokens cap below the threshold we're waiting for.
         * Cap min_burst at max_tokens; we'll just send in smaller chunks. */
        if (min_burst > max_tokens) min_burst = max_tokens;

        if (ctx->tokens < min_burst) {
            uint64_t wait_ns = ((min_burst - ctx->tokens) * 1000000000ULL) / ctx->bitrate;

            ctx->timeout.tv_sec = wait_ns / 1000000000ULL;
            ctx->timeout.tv_nsec = wait_ns % 1000000000ULL;

            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_timeout(sqe, &ctx->timeout, 0, 0);

            ctx->state = STATE_PACING_WAIT;
            io_uring_sqe_set_data(sqe, ctx);
            return;
        }

        if (chunk_size > ctx->tokens) chunk_size = ctx->tokens;

        ctx->tokens -= chunk_size;
    }

    struct io_uring_sqe *sqe_in = io_uring_get_sqe(&ring);
    io_uring_prep_splice(sqe_in, ctx->target_file_fd, current_offset, ctx->pipe_fds[1], -1, chunk_size, 0);
    sqe_in->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data(sqe_in, NULL);

    struct io_uring_sqe *sqe_out = io_uring_get_sqe(&ring);
    io_uring_prep_splice(sqe_out, ctx->pipe_fds[0], -1, ctx->client_socket_fd, -1, chunk_size, 0);
    io_uring_sqe_set_data(sqe_out, ctx);
}

static void init_server(void) {
    /* Resolve listen port from PORT env var, fall back to default. Done here
     * (not main) so any pre-init logging from spawned threads sees a sane
     * value via the global. */
    const char *env_port = getenv("PORT");
    if (env_port && env_port[0]) {
        int p = atoi(env_port);
        if (p > 0 && p < 65536) {
            server_port = p;
        }
    }

    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init failed");
        exit(1);
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(server_port)
    };

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    if (listen(server_fd, MAX_CONCURRENT_CLIENTS) < 0) {
        perror("Listen failed");
        exit(1);
    }
}

static void close_client_context(struct ClientContext* ctx) {
    if (!ctx) return;

    if (ctx->state == STATE_SENDFILE || ctx->state == STATE_PACING_WAIT || ctx->state == STATE_CANCELLING) {
        if (ctx->pipe_fds[0] > 0) close(ctx->pipe_fds[0]);
        if (ctx->pipe_fds[1] > 0) close(ctx->pipe_fds[1]);
        /* target_file_fd is the dup'd fd that the dispatcher owns; must close */
        if (ctx->target_file_fd > 0) close(ctx->target_file_fd);
    }

    if (ctx->client_socket_fd > 0) close(ctx->client_socket_fd);

    free_ctx(ctx);
}

int main(void) {
    init_server();

    cpu_set_t cpuset_main;
    CPU_ZERO(&cpuset_main);
    CPU_SET(0, &cpuset_main);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset_main) != 0) {
        DISPATCH_LOG("Failed to pin Dispatcher to Core 0: %s", strerror(errno), 0, 0, 0);
    }

    pthread_t logger_thread;
    if (pthread_create(&logger_thread, NULL, logger_daemon_main, NULL) != 0) {
        DISPATCH_LOG("Failed to spawn Logger thread: %s", strerror(errno), 0, 0, 0);
        exit(1);
    }

    pthread_t ingestion_thread;
    if (pthread_create(&ingestion_thread, NULL, ingestion_daemon_main, NULL) != 0) {
        DISPATCH_LOG("Failed to spawn Ingestion thread: %s", strerror(errno), 0, 0, 0);
        exit(1);
    }

    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 1;

    int num_workers = num_cores - 2;
    if (num_workers < 1) num_workers = 1;

    /* RCU reader IDs: 0 = dispatcher, 1..num_workers = workers. */
    if (READER_WORKER_BASE + num_workers > MAX_READERS) {
        DISPATCH_LOG("Too many workers (%d) for MAX_READERS (%d). Aborting.",
                     num_workers, MAX_READERS, 0, 0);
        exit(1);
    }

    DISPATCH_LOG("Hardware Topology: %ld cores detected. Allocating %d Worker threads.",
                 num_cores, num_workers, 0, 0);

    pthread_t* worker_threads = malloc(sizeof(pthread_t) * num_workers);
    if (!worker_threads) {
        DISPATCH_LOG("Fatal OOM during Worker Pool allocation.", 0, 0, 0, 0);
        exit(1);
    }

    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&worker_threads[i], NULL, worker_daemon_main, (void*)(intptr_t)i) != 0) {
            DISPATCH_LOG("Failed to spawn Worker thread %d", i, 0, 0, 0);
            exit(1);
        }
    }

    /* Boot barrier: wait for worker 0 to publish initial dict */
    while (atomic_load_explicit(&global_dictionary, memory_order_acquire) == NULL) {
        usleep(10000);
    }

    struct ClientContext* accept_ctx = allocate_context(server_fd, STATE_ACCEPT);
    add_accept_request(accept_ctx);
    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    unsigned head;

    DISPATCH_LOG("Dispatcher locked to port %d. Entering io_uring loop...", server_port, 0, 0, 0);

    while (1) {
        io_uring_submit_and_wait(&ring, 1);

        unsigned count = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            count++;

            struct ClientContext *ctx = (struct ClientContext *)io_uring_cqe_get_data(cqe);
            if (!ctx) continue;

            if (cqe->res < 0) {
                if (ctx->state == STATE_PACING_WAIT && cqe->res == -ETIME) {
                    /* not an error; fall through to switch */
                }
                else if (ctx->state == STATE_CANCELLING) {
                    DISPATCH_LOG("Cancellation complete. Safely freeing memory.", 0, 0, 0, 0);
                    close_client_context(ctx);
                    continue;
                }
                else if (ctx->state == STATE_ACCEPT && (cqe->res == -EMFILE || cqe->res == -ENFILE)) {
                    DISPATCH_LOG("WARNING: FD Exhaustion. Initiating 100ms backoff.", 0, 0, 0, 0);

                    ctx->timeout.tv_sec = 0;
                    ctx->timeout.tv_nsec = 100000000;

                    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_timeout(sqe, &ctx->timeout, 0, 0);
                    ctx->state = STATE_ACCEPT_BACKOFF;
                    io_uring_sqe_set_data(sqe, ctx);
                    continue;
                }
                else if (ctx->state == STATE_ACCEPT_BACKOFF && cqe->res == -ETIME) {
                    ctx->state = STATE_ACCEPT;
                    add_accept_request(ctx);
                    continue;
                }
                else {
                    DISPATCH_LOG("KERNEL VETO: Client dropped. State %d, Error %d", ctx->state, -cqe->res, 0, 0);

                    if (ctx->state == STATE_ACCEPT) {
                        add_accept_request(ctx);
                    }
                    else if (ctx->state == STATE_PACING_WAIT || ctx->state == STATE_SENDFILE) {
                        add_cancel_request(ctx);
                    }
                    else {
                        close_client_context(ctx);
                    }
                    continue;
                }
            }

            switch (ctx->state) {
                case STATE_ACCEPT: {
                    struct ClientContext *new_client = allocate_context(cqe->res, STATE_READ_HEADER);
                    if (new_client) {
                        add_read_header_request(new_client);
                    } else {
                        close(cqe->res);
                    }
                    add_accept_request(ctx);
                    break;
                }
                case STATE_READ_HEADER: {
                    if (cqe->res != sizeof(struct MediaHeader) ||
                        ctx->header.magic != PROTOCOL_MAGIC) {
                        DISPATCH_LOG("Header REJECTED. Res: %d, Magic: 0x%X", cqe->res, ctx->header.magic, 0, 0);
                        close(ctx->client_socket_fd);
                        free_ctx(ctx);
                        break;
                    }

                    DISPATCH_LOG("Header VALIDATED. Magic: 0x%X | Requested File ID: %lu",
                               ctx->header.magic, ctx->header.file_id, 0, 0);

                    if (ctx->header.command == CMD_TEARDOWN) {
                        DISPATCH_LOG("Client requested graceful teardown. Releasing FD %d.", ctx->client_socket_fd, 0, 0, 0);
                        close(ctx->client_socket_fd);
                        free_ctx(ctx);
                        break;
                    }

                    if (ctx->header.payload_len > 0) {
                        if (ctx->header.payload_len > MAX_PAYLOAD_SIZE) {
                            DISPATCH_LOG("Payload too large (%u). Dropping FD %d.",
                                         ctx->header.payload_len, ctx->client_socket_fd, 0, 0);
                            close(ctx->client_socket_fd);
                            free_ctx(ctx);
                            break;
                        }

                        ctx->payload_buf = malloc(ctx->header.payload_len);
                        if (!ctx->payload_buf) {
                            DISPATCH_LOG("OOM allocating %u-byte payload buffer for FD %d.",
                                         ctx->header.payload_len, ctx->client_socket_fd, 0, 0);
                            close(ctx->client_socket_fd);
                            free_ctx(ctx);
                            break;
                        }

                        ctx->state = STATE_READ_PAYLOAD;
                        add_read_payload_request(ctx);
                        break;
                    }

                    if (ctx->header.command == CMD_STREAM_FILE) {
                        ctx->target_file_fd = lookup_file_metadata(ctx->header.file_id, &ctx->bitrate);
                        if (ctx->target_file_fd < 0) {
                            DISPATCH_LOG("FILE NOT FOUND. Requested ID %lu does not exist.", ctx->header.file_id, 0, 0, 0);
                            close(ctx->client_socket_fd);
                            free_ctx(ctx);
                            break;
                        }

                        if (pipe2(ctx->pipe_fds, O_NONBLOCK) < 0) {
                            perror("Failed to allocate pipe bridge");
                            close(ctx->target_file_fd);
                            close(ctx->client_socket_fd);
                            free_ctx(ctx);
                            break;
                        }

                        if (fcntl(ctx->pipe_fds[0], F_SETPIPE_SZ, 1048576) < 0) {
                            DISPATCH_LOG("Warning: Failed to expand pipe size. Defaulting to 64KB.", 0, 0, 0, 0);
                        }

                        ctx->bytes_to_send = ctx->header.end_byte - ctx->header.start_byte;
                        ctx->bytes_sent_total = 0;
                        ctx->state = STATE_SENDFILE;

                        ctx->next_fadvise_offset = ctx->header.start_byte + BURST_SIZE;

                        ctx->last_tx_time_ns = get_time_ns();
                        ctx->tokens = ctx->bitrate * 2;

                        posix_fadvise(ctx->target_file_fd, ctx->header.start_byte, BURST_SIZE, POSIX_FADV_WILLNEED);

                        add_sendfile_request(ctx);
                    }
                    break;
                }
                case STATE_READ_PAYLOAD: {
                    if (cqe->res != (int)ctx->header.payload_len) {
                        DISPATCH_LOG("Payload read incomplete. Expected %u, got %d.",
                                     ctx->header.payload_len, cqe->res, 0, 0);
                        close(ctx->client_socket_fd);
                        free_ctx(ctx);
                        break;
                    }

                    if (work_queue_push(ctx->client_socket_fd, &ctx->header, ctx->payload_buf) < 0) {
                        DISPATCH_LOG("Work Queue Full. Dropping FD %d.", ctx->client_socket_fd, 0, 0, 0);
                        close(ctx->client_socket_fd);
                        free_ctx(ctx);
                        break;
                    }

                    ctx->payload_buf = NULL;
                    free_ctx(ctx);
                    break;
                }
                case STATE_SENDFILE: {
                    ctx->bytes_sent_total += cqe->res;

                    off_t current_absolute_offset = ctx->header.start_byte + ctx->bytes_sent_total;
                    off_t watermark_trigger = ctx->next_fadvise_offset - (BURST_SIZE / 4);

                    if (current_absolute_offset >= watermark_trigger &&
                        ctx->next_fadvise_offset < (off_t)ctx->header.end_byte) {

                        posix_fadvise(ctx->target_file_fd, ctx->next_fadvise_offset, BURST_SIZE, POSIX_FADV_WILLNEED);
                        ctx->next_fadvise_offset += BURST_SIZE;
                    }

                    if (ctx->bytes_sent_total < ctx->bytes_to_send) {
                        add_sendfile_request(ctx);
                    } else {
                        close(ctx->pipe_fds[0]);
                        close(ctx->pipe_fds[1]);
                        close(ctx->target_file_fd);  /* dup'd fd, dispatcher owns it */
                        close(ctx->client_socket_fd);
                        free_ctx(ctx);
                    }
                    break;
                }
                case STATE_PACING_WAIT: {
                    ctx->state = STATE_SENDFILE;
                    add_sendfile_request(ctx);
                    break;
                }
                case STATE_WORKER_HANDOFF:
                case STATE_CANCELLING:
                case STATE_ACCEPT_BACKOFF:
                    break;
                case STATE_CLEANUP:
                    close(ctx->client_socket_fd);
                    free_ctx(ctx);
                    break;
            }
        }
        io_uring_cq_advance(&ring, count);
    }
    free(worker_threads);
    return 0;
}
