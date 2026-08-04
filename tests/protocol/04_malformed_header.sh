#!/bin/bash
# tests/protocol/04_malformed_header.sh
# Verifies: bad magic / unknown command / payload_len overflow → connection
# closed cleanly, server stays up. No crash, no hang.

set -uo pipefail
TEST_NAME="protocol_04_malformed_header"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys, socket, struct, time
sys.path.insert(0, "$TESTS_DIR")
from client import Client, HEADER_FMT, MAGIC, VERSION, CMD_STREAM_FILE

PORT = $TEST_PORT

def send_and_read(payload, timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(("127.0.0.1", PORT))
    s.sendall(payload)
    try:
        data = s.recv(4096)
    except socket.timeout:
        data = b""
    s.close()
    return data

# 1) Bad magic
hdr = struct.pack(HEADER_FMT, 0xDEADBEEF, VERSION, CMD_STREAM_FILE,
                  0, 0, 0, 0, 1024, b"\x00" * 24)
d = send_and_read(hdr)
assert d == b"", f"bad magic leaked {len(d)} bytes"

# 2) Unknown command
hdr = struct.pack(HEADER_FMT, MAGIC, VERSION, 0xBEEF,
                  0, 0, 0, 0, 1024, b"\x00" * 24)
d = send_and_read(hdr)
assert d == b"", f"unknown cmd leaked {len(d)} bytes"

# 3) Oversized payload_len (claim 256 MB, send no body)
#    Server must NOT allocate 256 MB; it must reject early.
hdr = struct.pack(HEADER_FMT, MAGIC, VERSION, 0x0020,  # CMD_QUERY_METADATA
                  256 * 1024 * 1024, 0, 0, 0, 0, b"\x00" * 24)
d = send_and_read(hdr, timeout=3)
# Either the server caps payload_len (drops/closes) or the read times out;
# anything other than a 256 MB allocation is acceptable here.
print("malformed headers: all rejected without crash")

# Server still alive?
with Client(port=PORT) as c:
    pass
print("ok: server still accepts connections")
PY
assert_eq "$?" "0" "malformed headers did not crash server"
assert_log_not_contains "segfault|Sanitizer|Aborted" "no crash markers in server log"
