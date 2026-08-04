#!/bin/bash
# tests/inotify/01_path_aware_hash.sh
# Catches the audit's old hash-of-basename bug: two files named identically
# in different subdirectories must be served as distinct entities.

set -uo pipefail
TEST_NAME="inotify_01_path_aware_hash"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start  # fixture has /inception.mkv and /movies/inception.mkv

ids=$(grep "Ingested ID:" "$SERVER_LOG" | awk '{print $4}' | sort -u | wc -l)
total=$(grep -c "Ingested ID:" "$SERVER_LOG" || true)
assert_eq "$ids" "$total" "all ingested IDs are unique (no basename collision)"

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client

with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid_top = c.hash_relative("inception.mkv")
    data1, _ = c.stream(fid_top, 0, 1024)
    assert len(data1) == 1024, f"top-level: {len(data1)}"

with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    fid_sub = c.hash_relative("movies/inception.mkv")
    data2, _ = c.stream(fid_sub, 0, 1024)
    assert len(data2) == 1024, f"subdir: {len(data2)}"

# Different sine frequencies → different AAC payloads
assert data1 != data2, "same content from both — collision!"
print("ok: same-basename files distinct")
PY
assert_eq "$?" "0" "same-basename files served independently"
