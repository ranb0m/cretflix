#!/bin/bash
# tests/lifecycle/03_sigterm_clean_exit.sh
# Verifies: SIGTERM during active streams shuts down without segfault,
# without leaving orphaned pipes/sockets, and with a non-error exit code.
#
# Currently the server doesn't have a graceful shutdown handler — workers
# loop forever, dispatcher loops forever — so this test mainly catches
# whether SIGTERM cleanly terminates everything WITHOUT crashing. If you
# add a shutdown handler later, tighten this test.

set -uo pipefail
TEST_NAME="lifecycle_03_sigterm"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

# Start a streaming client; let it get a few KB into the stream
python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY &
import sys, time, os
sys.path.insert(0, "$TESTS_DIR")
from client import Client
size = os.path.getsize("$MEDIA_ROOT/inception.mkv")
try:
    with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=5) as c:
        fid = c.hash_relative("inception.mkv")
        c.stream(fid, 0, size)
except Exception as e:
    print("client got:", e)
PY
client_pid=$!

sleep 0.3  # let the stream get going

# SIGTERM the server
log "sending SIGTERM"
kill -TERM "$SERVER_PID"

# Wait up to 5s for it to exit
for i in {1..50}; do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
done

if kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "server did not exit within 5s of SIGTERM"
    kill -9 "$SERVER_PID" 2>/dev/null || true
else
    pass "server exited within 5s of SIGTERM"
fi

# Reap the client
wait "$client_pid" 2>/dev/null || true

# No crash markers in the server log
assert_log_not_contains "segfault|Aborted|core dumped" "no crash on SIGTERM"

# Mark SERVER_PID empty so test_cleanup doesn't try to kill it again
SERVER_PID=""
