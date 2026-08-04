#!/bin/bash
# tests/protocol/02_invalid_id.sh
# Verifies: a bogus file_id closes the connection cleanly and the server
# stays responsive afterward.

set -uo pipefail
TEST_NAME="protocol_02_invalid_id"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client

with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=3) as c:
    data, _ = c.stream(file_id=0xDEADBEEFCAFEBABE, start_byte=0, end_byte=1024)
    assert len(data) == 0, f"server leaked {len(data)} bytes for invalid id"
    print("ok: zero bytes returned")

# Server still responsive?
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=2) as c:
    pass
print("ok: still accepting connections")
PY
assert_eq "$?" "0" "invalid id rejected, server stays up"
