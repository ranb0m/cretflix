#!/bin/bash
# tests/concurrency/02_disconnect_mid_stream.sh
# Verifies: violent client disconnect mid-stream is handled cleanly. After
# the disconnect, the server's fd count returns to (or below) the pre-test
# baseline — the cancellation/cleanup path actually closes the client fd
# and releases the per-stream pipe.
#
# Replaces test_cancellation.py from the old harness, with proper assertions.

set -uo pipefail
TEST_NAME="concurrency_02_disconnect_mid_stream"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

# Use a larger fixture so the stream is in flight long enough to hit it
# during cancellation
export MEDIA_ROOT="/tmp/media_test_${TEST_NAME}"
rm -rf "$MEDIA_ROOT"; mkdir -p "$MEDIA_ROOT"
ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "sine=frequency=440:duration=30" \
    -c:a aac -b:a 4000k "$MEDIA_ROOT/big.mp4" 2>/dev/null

SKIP_FIXTURES=1 server_start

baseline_fds=$(server_fd_count)
log "baseline server fds: $baseline_fds"

# Run 10 connect/stream/violent-disconnect cycles
python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys, socket, struct, time
sys.path.insert(0, "$TESTS_DIR")
from client import Client, HEADER_FMT, MAGIC, VERSION, CMD_STREAM_FILE

c0 = Client(port=$TEST_PORT, media_root="$MEDIA_ROOT")
import os
fsize = os.path.getsize("$MEDIA_ROOT/big.mp4")
fid = c0.hash_relative("big.mp4")

for i in range(10):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", $TEST_PORT))
    hdr = struct.pack(HEADER_FMT, MAGIC, VERSION, CMD_STREAM_FILE,
                      0, 0, fid, 0, fsize, b"\x00" * 24)
    s.sendall(hdr)
    # Read just a bit, then SO_LINGER=0 + close → RST
    s.recv(65536)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                 struct.pack("ii", 1, 0))
    s.close()
    time.sleep(0.05)
print("10 disconnect cycles complete")
PY
assert_eq "$?" "0" "10 mid-stream disconnects executed"

# Give the io_uring cancellation pipeline time to drain
sleep 1

after_fds=$(server_fd_count)
log "after disconnect cycles: $after_fds fds"

# fds should be at or below baseline. Allow +2 slack for transient
# accept-pending fds.
if [ "$after_fds" -le "$((baseline_fds + 2))" ]; then
    pass "fd count returned to baseline (before=$baseline_fds, after=$after_fds)"
else
    fail "fd leak: before=$baseline_fds, after=$after_fds"
fi

assert_log_not_contains "Sanitizer|segfault" "no crash from violent disconnect"
