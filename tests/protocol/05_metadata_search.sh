#!/bin/bash
# tests/protocol/05_metadata_search.sh
# Verifies: CMD_QUERY_METADATA returns valid JSON; matching substring yields
# FOUND with results, missing string yields NOT_FOUND.

set -uo pipefail
TEST_NAME="protocol_05_metadata_search"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start  # fixture has inception.mkv in two places, dark_knight.mp4, episode_01.mp4

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client

# Match: "inception" should appear in two filenames
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    resp = c.query_metadata("inception")
    print("inception ->", resp)
    assert resp.get("status") == "FOUND", f"expected FOUND, got {resp}"
    results = resp.get("results", [])
    assert len(results) >= 2, f"expected >=2 hits for 'inception', got {len(results)}"

# Match a string that's in only one file
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    resp = c.query_metadata("dark_knight")
    print("dark_knight ->", resp)
    assert resp.get("status") == "FOUND"
    assert len(resp.get("results", [])) == 1

# Miss
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    resp = c.query_metadata("xyzzy_no_match")
    print("xyzzy ->", resp)
    assert resp.get("status") == "NOT_FOUND", f"expected NOT_FOUND, got {resp}"

print("ok")
PY
assert_eq "$?" "0" "metadata search returns expected JSON shapes"
