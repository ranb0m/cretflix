#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* ASCII for 'MEDI' */
#define PROTOCOL_MAGIC      0x4D454449
#define PROTOCOL_VERSION_1  0x0001

/*
 * Command Space:
 * CMD_STREAM_FILE:    fast path -> sendfile
 * CMD_QUERY_METADATA: worker handoff
 * CMD_AUTHENTICATE:   worker handoff
 * CMD_TEARDOWN:       resource cleanup (headers, etc.)
 */
#define CMD_STREAM_FILE     0x0010
#define CMD_QUERY_METADATA  0x0020
#define CMD_AUTHENTICATE    0x0030
#define CMD_TEARDOWN        0x0040

/*
 * The Bipartite Header. Exactly 64 bytes (cache-line aligned).
 */
struct MediaHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t command;
    uint32_t payload_len;
    uint32_t flags;

    /* Fast-path arguments */
    uint64_t file_id;
    uint64_t start_byte;
    uint64_t end_byte;

    uint8_t  hardware_pad[24];
} __attribute__((aligned(64)));

_Static_assert(sizeof(struct MediaHeader) == 64, "MediaHeader must be strictly 64 bytes.");

/*
 * Type-Length-Value (TLV) Item Header
 * Used within the variable-length payload for slow-path requests.
 *   type 0x0001 = "Search String"
 *   type 0x0002 = "Auth Token"
 */
struct __attribute__((packed)) TLV_Item {
    uint16_t type;
    uint16_t length;
};

#endif /* PROTOCOL_H */
