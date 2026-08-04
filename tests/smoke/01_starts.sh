#!/bin/bash
# tests/smoke/01_starts.sh
# Verifies: server starts, ingests fixtures, accepts connections.

set -uo pipefail
TEST_NAME="smoke_01_starts"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

assert_log_contains "Batch Ingestion Complete" "boot ingestion finished"
assert_log_contains "Runtime Ingestion Daemon armed" "inotify daemon armed"
assert_log_contains "Entering io_uring loop" "dispatcher entered io_uring loop"
assert_log_contains "locked to port $TEST_PORT" "bound the requested port"

ingested=$(grep -c "Ingested ID:" "$SERVER_LOG" || true)
assert_eq "$ingested" "4" "exactly 4 media files ingested (srt filtered)"
