#!/bin/bash
# tests/protocol/03_range_request.sh
# Verifies: a partial-range request (start_byte > 0) returns exactly the
# requested slice, byte-identical to what's actually at that offset on disk.

set -uo pipefail
TEST_NAME="protocol_03_range_request"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

# Use a fixture file. Pull a 1 KB slice at offset 32 KB (well past the
# container header so we're reading actual frame data).
FILE="$MEDIA_ROOT/inception.mkv"
size=$(stat -c%s "$FILE")
offset=32768
length=1024
[ "$((offset + length))" -le "$size" ] || { fail "fixture too small: $size"; exit 1; }

# Compute expected bytes via dd as the source of truth
expected_hex=$(dd if="$FILE" bs=1 skip=$offset count=$length status=none | xxd -p | tr -d '\n')

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid = c.hash_relative("inception.mkv")
    data, _ = c.stream(fid, $offset, $offset + $length)
    assert len(data) == $length, f"got {len(data)} bytes, wanted $length"
    print("HEX:" + data.hex())
PY
status=$?
assert_eq "$status" "0" "range request returned correct length"

actual_hex=$(grep "^HEX:" "$LOG_DIR/${TEST_NAME}.log" | sed 's/^HEX://')
assert_eq "$actual_hex" "$expected_hex" "byte content matches dd at offset $offset"
