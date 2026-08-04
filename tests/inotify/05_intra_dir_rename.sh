#!/bin/bash
# tests/inotify/05_intra_dir_rename.sh
# Verifies: rename within a single watched directory matches MOVED_FROM and
# MOVED_TO via cookie. Should produce exactly ONE rename event (not a
# delete + insert pair).

set -uo pipefail
TEST_NAME="inotify_05_intra_dir_rename"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

src="$MEDIA_ROOT/movies/dark_knight.mp4"
dst="$MEDIA_ROOT/movies/the_dark_knight_2008.mp4"
mv "$src" "$dst"

# Single rename event should appear; no separate Removed/Indexed pair
if wait_for_log "Renamed\." 5; then
    pass "single rename event emitted"
else
    fail "no rename event within 5s"
fi

# Sleep a moment so any racing remove-then-add events would have shown up
sleep 1

removed=$(grep -c "Removed file. ID:" "$SERVER_LOG" || true)
indexed=$(grep -c "Indexed new file. ID:" "$SERVER_LOG" || true)
assert_eq "$removed" "0" "no spurious 'Removed' event for intra-dir rename"
assert_eq "$indexed" "0" "no spurious 'Indexed' event for intra-dir rename"

# New path streamable, old path not
python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid_new = c.hash_relative("movies/the_dark_knight_2008.mp4")
    data, _ = c.stream(fid_new, 0, 1024)
    assert len(data) == 1024
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=2) as c:
    fid_old = c.hash_relative("movies/dark_knight.mp4")
    data, _ = c.stream(fid_old, 0, 1024)
    assert len(data) == 0
print("ok")
PY
assert_eq "$?" "0" "post-rename: new path streamable, old not"
