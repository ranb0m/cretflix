#ifndef IPC_H
#define IPC_H

// The file path for our localized Unix Domain Socket 
// This lives on RAM.
#define WORKER_UDS_PATH "/tmp/media_worker.sock"

#include "protocol.h"

/* * Extracts a live network socket from the Dispatcher's address space
 * and forces it through the IPC pipe to the Worker pool.
 */
int send_fd_to_worker(int uds_socket, int network_fd, struct MediaHeader* header);

/* * The reciprocal function called by the Worker daemon to receive
 * the live network socket from the kernel's SCM_RIGHTS control message.
 */
int receive_fd_from_dispatcher(int uds_socket, struct MediaHeader* header);

#endif // IPC_H
