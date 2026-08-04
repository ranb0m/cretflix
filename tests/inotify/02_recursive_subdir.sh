#!/bin/bash
# tests/inotify/02_recursive_subdir.sh
# Verifies: creating a subdirectory at runtime adds an inotify watch + scans
# its contents; a file dropped into it becomes streamable.

set -uo pipefail
TEST_NAME="inotify_02_recursive_subdir"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

new_dir="$MEDIA_ROOT/runtime_added"
new_file="$new_dir/lecture.mp4"
mkdir "$new_dir"
ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "sine=frequency=700:duration=10" \
    -c:a aac -b:a 256k "$new_file"

if wait_for_log "Added watch on new subdir" 5; then
    pass "new subdir got an inotify watch"
else
    fail "new subdir did NOT get a watch within 5s"
fi
if wait_for_log "Indexed new file" 5; then
    pass "new file under new subdir was indexed"
else
    fail "new file was NOT indexed within 5s"
fi

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid = c.hash_relative("runtime_added/lecture.mp4")
    data, _ = c.stream(fid, 0, 1024)
    assert len(data) == 1024, f"got {len(data)}"
print("ok")
PY
assert_eq "$?" "0" "runtime-added file is streamable"
