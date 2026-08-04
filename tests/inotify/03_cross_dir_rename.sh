#!/bin/bash
# tests/inotify/03_cross_dir_rename.sh
# Verifies: mv across watched subdirs (different inotify watches) correlates
# correctly via kernel cookie. After rename: old hash dies, new hash lives.

set -uo pipefail
TEST_NAME="inotify_03_cross_dir_rename"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

src="$MEDIA_ROOT/movies/dark_knight.mp4"
dst="$MEDIA_ROOT/tv/dark_knight.mp4"
[ -f "$src" ] || { fail "fixture missing: $src"; exit 1; }

mv "$src" "$dst"

if wait_for_log "Renamed\." 5; then
    pass "rename event emitted"
else
    fail "no rename event within 5s"
fi

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client

# Old path's hash should now be unstreamable (NOT_FOUND => connection close)
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=2) as c:
    fid_old = c.hash_relative("movies/dark_knight.mp4")
    data, _ = c.stream(fid_old, 0, 1024)
    assert len(data) == 0, f"old path still served {len(data)} bytes after rename"

# New path's hash should serve content
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid_new = c.hash_relative("tv/dark_knight.mp4")
    data, _ = c.stream(fid_new, 0, 1024)
    assert len(data) == 1024, f"new path: got {len(data)}"

print("ok: rename swap verified")
PY
assert_eq "$?" "0" "cross-directory rename: old hash dies, new lives"
