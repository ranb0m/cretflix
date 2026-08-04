#!/bin/bash
# tests/protocol/01_stream_basic.sh
# Verifies: a stream request returns exactly the expected bytes.

set -uo pipefail
TEST_NAME="protocol_01_stream_basic"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start
expected=$(stat -c%s "$MEDIA_ROOT/inception.mkv")

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid = c.hash_relative("inception.mkv")
    data, dur = c.stream(fid, 0, $expected)
    print(f"received {len(data)} bytes in {dur:.3f}s")
    assert len(data) == $expected, f"got {len(data)}, expected $expected"
PY
assert_eq "$?" "0" "stream returned exact-size payload"
assert_log_not_contains "Sanitizer" "no sanitizer reports during stream"
