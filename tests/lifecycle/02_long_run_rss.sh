#!/bin/bash
# tests/lifecycle/02_long_run_rss.sh
# Verifies: server's resident memory does not grow unboundedly under
# sustained load. Streams continuously for $DURATION seconds, samples RSS
# every 5 seconds, asserts max RSS is within reasonable bounds of the
# starting RSS.
#
# Tunable via env: DURATION (default 60s) — bump to 600+ for real soak runs.

set -uo pipefail
TEST_NAME="lifecycle_02_long_run_rss"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

DURATION="${DURATION:-60}"
SAMPLE_INTERVAL=5

server_start
sleep 2  # let dispatcher quiesce post-boot

rss_kb() {
    grep "^VmRSS:" "/proc/$SERVER_PID/status" 2>/dev/null \
        | awk '{print $2}' || echo 0
}

start_rss=$(rss_kb)
log "starting RSS: ${start_rss} KB"

# Background continuous streamer
streamer_log="$LOG_DIR/${TEST_NAME}.streamer.log"
(
    set +e
    python3 - <<PY
import sys, time, os
sys.path.insert(0, "$TESTS_DIR")
from client import Client
size = os.path.getsize("$MEDIA_ROOT/inception.mkv")
end = time.monotonic() + $DURATION
n = 0
while time.monotonic() < end:
    try:
        with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
            fid = c.hash_relative("inception.mkv")
            c.stream(fid, 0, size)
        n += 1
    except Exception as e:
        print("err:", e)
print(f"{n} streams")
PY
) > "$streamer_log" 2>&1 &
streamer_pid=$!

# Sample RSS while streamer runs
max_rss=0
samples=0
elapsed=0
while [ "$elapsed" -lt "$DURATION" ]; do
    sleep "$SAMPLE_INTERVAL"
    elapsed=$((elapsed + SAMPLE_INTERVAL))
    cur=$(rss_kb)
    log "  t=${elapsed}s  RSS=${cur} KB"
    if [ "$cur" -gt "$max_rss" ]; then max_rss=$cur; fi
    samples=$((samples + 1))
done

wait "$streamer_pid" 2>/dev/null || true
log "streamer summary: $(tail -1 "$streamer_log")"

end_rss=$(rss_kb)
log "ending RSS:  ${end_rss} KB"
log "max RSS:     ${max_rss} KB"
log "growth:      $((max_rss - start_rss)) KB over $samples samples"

# Allow up to 2x growth (generous for fragmentation, page-cache pinning,
# etc.). Real leaks would blow well past this.
limit=$((start_rss * 2))
if [ "$max_rss" -le "$limit" ]; then
    pass "RSS stayed under 2x baseline (max=${max_rss}, baseline=${start_rss})"
else
    fail "RSS exceeded 2x baseline: max=${max_rss} > limit=${limit}"
fi
