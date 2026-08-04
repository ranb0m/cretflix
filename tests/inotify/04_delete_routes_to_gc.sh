#!/bin/bash
# tests/inotify/04_delete_routes_to_gc.sh
# Verifies: deleting a file removes it from the dict AND its open fd is
# eventually closed via the RCU GC pipeline. We measure server fd count
# before and after to confirm the fd actually goes back to the OS.

set -uo pipefail
TEST_NAME="inotify_04_delete_gc"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

# Baseline fd count (after boot, before delete)
fd_before=$(server_fd_count)
log "fds before delete: $fd_before"

# Delete one fixture file
victim="$MEDIA_ROOT/tv/episode_01.mp4"
[ -f "$victim" ] || { fail "fixture missing: $victim"; exit 1; }
rm "$victim"

if wait_for_log "Removed file\\. ID:" 5; then
    pass "delete event processed"
else
    fail "no remove event within 5s"
fi

# The fd is enqueued for GC. The next rcu_synchronize cycle (driven by
# the logger's drain loop) closes it. Give it a couple of seconds.
sleep 2

fd_after=$(server_fd_count)
log "fds after delete + GC: $fd_after"

# We expect at least one fewer fd. (Could be more if other GCable resources
# were drained, but never more than before.)
if [ "$fd_after" -le "$((fd_before - 1))" ]; then
    pass "fd count decreased after delete (before=$fd_before, after=$fd_after)"
else
    fail "fd not reclaimed: before=$fd_before, after=$fd_after"
fi

# Verify the deleted file's hash is no longer streamable
python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=2) as c:
    fid = c.hash_relative("tv/episode_01.mp4")
    data, _ = c.stream(fid, 0, 1024)
    assert len(data) == 0, f"deleted file still streamable: {len(data)} bytes"
print("ok")
PY
assert_eq "$?" "0" "deleted file no longer streamable"
