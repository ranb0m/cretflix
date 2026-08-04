#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <protocol.h>
#include "ipc.h"
#include "logger.h"

/* *****************************************************************************
 * Mitosis Phase 1: The Ejection
 * -----------------------------------------------------------------------------
 * Tears the network socket from the Dispatcher and sends it through the pipe.
 * ****************************************************************************/
int send_fd_to_worker(int uds_socket, int network_fd, struct MediaHeader* header) {
    struct msghdr msg = {0};
    
    // POSIX Mandate: We cannot send *only* ancillary data. We must send at 
    // least one byte of standard payload data for the control message to ride on.
    struct iovec iov;
    iov.iov_base = header;
    iov.iov_len = sizeof(struct MediaHeader);
    
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // The Memory Alignment Union. 
    // We must allocate a buffer large enough to hold a cmsghdr struct plus 
    // an integer (the FD), aligned perfectly to the CPU's word boundary.
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control_buf;

    msg.msg_control = control_buf.buf;
    msg.msg_controllen = sizeof(control_buf.buf);
    
    // We clear the buffer to prevent uninitialized memory from corrupting the headers
    memset(control_buf.buf, 0, sizeof(control_buf.buf));

    // Construct the Control Message Header
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    // We use the CMSG_DATA macro to find the exact memory address immediately 
    // following the header, cast it to an int pointer, and write our network_fd.
    *((int *) CMSG_DATA(cmsg)) = network_fd;

    // Execute the transmission
    if (sendmsg(uds_socket, &msg, 0) < 0) {
        DISPATCH_LOG("sendmsg SCM_RIGHTS failed. Errno: %d", errno, 0, 0, 0);
        return -1;
    }

    return 0;
}

/* *****************************************************************************
 * Mitosis Phase 2: The Implantation
 * -----------------------------------------------------------------------------
 * The Worker wakes up, reads the pipe, and extracts the newly minted FD.
 * ****************************************************************************/
int receive_fd_from_dispatcher(int uds_socket, struct MediaHeader* header) {
    struct msghdr msg = {0};
    
    // We must provide a buffer to receive the dummy byte sent by the Dispatcher.
    struct iovec iov;
    iov.iov_base = header;
    iov.iov_len = sizeof(struct MediaHeader);
    
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // Allocate the exact same alignment union on the receiving side
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control_buf;

    msg.msg_control = control_buf.buf;
    msg.msg_controllen = sizeof(control_buf.buf);

    // Block until the Dispatcher pushes a client through the socket
    if (recvmsg(uds_socket, &msg, 0) < 0) {
        WORKER_LOG("recvmsg SCM_RIGHTS failed. Errno: %d", errno, 0, 0, 0);
        return -1;
    }

    // Iterate through the received control messages (though we only expect one)
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            // Extract the integer from the data section of the header
            int received_fd = *((int *) CMSG_DATA(cmsg));
            return received_fd;
        }
    }

    // If we reach here, the Dispatcher sent us a message, but it contained no FD.
    return -1;
}
