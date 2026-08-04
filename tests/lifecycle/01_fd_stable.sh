#!/bin/bash
# tests/lifecycle/01_fd_stable.sh
# Verifies: 100 sequential connect/stream/close cycles do not leak fds.
# Catches the slow leak that's invisible to single-shot tests but accumulates
# over hours/days of real use.

set -uo pipefail
TEST_NAME="lifecycle_01_fd_stable"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

# Quiesce, then take baseline
sleep 1
baseline=$(server_fd_count)
log "baseline fds: $baseline"

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
import os
size = os.path.getsize("$MEDIA_ROOT/inception.mkv")
for i in range(100):
    with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
        fid = c.hash_relative("inception.mkv")
        data, _ = c.stream(fid, 0, size)
        assert len(data) == size
print("100 cycles complete")
PY
assert_eq "$?" "0" "100 stream cycles all completed"

# Allow GC to drain
sleep 2
final=$(server_fd_count)
log "final fds: $final"

# Strict: should be exactly baseline. Allow 1-2 transient fds.
delta=$((final - baseline))
if [ "$delta" -le 2 ] && [ "$delta" -ge -2 ]; then
    pass "fd count stable (delta=$delta after 100 cycles)"
else
    fail "fd leak: delta=$delta (baseline=$baseline final=$final)"
fi
