#!/bin/bash
# tests/protocol/06_extension_filter.sh
# Verifies: files with non-media extensions never enter the dictionary.
# Distinct from your old test (which conflated "no data" with "filter worked"):
# we directly verify the dict by searching for a substring that's only in
# the filtered file's name.

set -uo pipefail
TEST_NAME="protocol_06_extension_filter"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start  # fixture has subtitles.srt; should be filtered out

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client

# "subtitles" appears only in the .srt filename. If it were ingested,
# query would return FOUND. We expect NOT_FOUND.
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    resp = c.query_metadata("subtitles")
    print("subtitles ->", resp)
    assert resp.get("status") == "NOT_FOUND", \
        f".srt was ingested! response: {resp}"

print("ok: .srt filtered at ingestion (not in dictionary)")
PY
assert_eq "$?" "0" "subtitle file filtered out of dictionary"

# Also: server log should NOT contain an "Ingested ID" line for the .srt
ingested=$(grep -c "Ingested ID:" "$SERVER_LOG" || true)
assert_eq "$ingested" "4" "exactly 4 files ingested (excludes .srt)"
