#!/bin/bash
# tests/concurrency/04_connection_storm.sh
# Verifies: server doesn't crash or wedge when hammered with many concurrent
# connections. Replaces test_exhaustion.py; this version actually asserts
# something instead of "watch for 10 seconds and hope."

set -uo pipefail
TEST_NAME="concurrency_04_connection_storm"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

# Bump our own ulimit so the test client can actually open many sockets
ulimit -n 4096 2>/dev/null || log "warning: could not raise ulimit"

baseline_fds=$(server_fd_count)
log "baseline fds: $baseline_fds"

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys, socket, time, threading
sys.path.insert(0, "$TESTS_DIR")
from client import Client, HEADER_FMT, MAGIC, VERSION, CMD_STREAM_FILE
import struct

PORT = $TEST_PORT
N = 500          # concurrent open sockets
HOLD = 3.0       # seconds to hold them open

errors = []
sockets = []

def opener(i):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(("127.0.0.1", PORT))
        sockets.append(s)
    except Exception as e:
        errors.append(f"{i}: {e}")

threads = [threading.Thread(target=opener, args=(i,)) for i in range(N)]
t0 = time.monotonic()
for t in threads: t.start()
for t in threads: t.join()
opened = len(sockets)
print(f"opened {opened}/{N} sockets in {time.monotonic() - t0:.2f}s")
if errors:
    print(f"  open errors: {len(errors)} (e.g. {errors[0]})")

# Hold them
time.sleep(HOLD)

# Server should still be alive — try a fresh connection
with Client(port=PORT, timeout=3) as c:
    pass
print("ok: server still accepting after hold")

# Close them all
for s in sockets:
    try:
        s.close()
    except OSError:
        pass

# Server should NOT have crashed even if it ran out of accept slots
print(f"opened {opened} sockets total")
assert opened >= 200, f"only opened {opened} — server may be capping accept queue too low"
PY
assert_eq "$?" "0" "$N concurrent connections held without crash"

# Give the server a moment to clean up after the close storm
sleep 1
final_fds=$(server_fd_count)
log "after storm fds: $final_fds"
if [ "$final_fds" -le "$((baseline_fds + 5))" ]; then
    pass "fd count returned near baseline (before=$baseline_fds, after=$final_fds)"
else
    fail "fd count high after storm: before=$baseline_fds, after=$final_fds"
fi

assert_log_not_contains "Sanitizer|segfault|Aborted" "no crash markers"
