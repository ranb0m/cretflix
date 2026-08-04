#!/bin/bash
# tests/concurrency/01_n_streams.sh
# Verifies: N concurrent stream requests against a mix of files all complete
# with the right number of bytes. Exercises the io_uring loop under load,
# the splice pipeline, the token bucket per connection, and basic dict
# read concurrency.

set -uo pipefail
TEST_NAME="concurrency_01_n_streams"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

# Use higher bitrate fixtures here so pacing doesn't dominate timing.
# Override the default fixture by generating directly.
export MEDIA_ROOT="/tmp/media_test_${TEST_NAME}"
rm -rf "$MEDIA_ROOT"
mkdir -p "$MEDIA_ROOT/movies" "$MEDIA_ROOT/tv"

# 5-second files at higher bitrates ⇒ ~3 MB each. Bitrate ~600 KB/s, well
# above the min_burst threshold so pacing doesn't deadlock.
for i in 1 2 3 4 5; do
    ffmpeg -y -hide_banner -loglevel error \
        -f lavfi -i "sine=frequency=$((400 + i * 50)):duration=5" \
        -c:a aac -b:a 4000k \
        "$MEDIA_ROOT/movies/file_${i}.mp4" 2>/dev/null
done

SKIP_FIXTURES=1 server_start

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys, time, threading
sys.path.insert(0, "$TESTS_DIR")
from client import Client

N = 20
results = [None] * N

def worker(i):
    fname = f"movies/file_{(i % 5) + 1}.mp4"
    full = f"$MEDIA_ROOT/{fname}"
    import os
    expected = os.path.getsize(full)
    try:
        with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=20) as c:
            fid = c.hash_relative(fname)
            data, dur = c.stream(fid, 0, expected)
            results[i] = (len(data), expected, dur, None)
    except Exception as e:
        results[i] = (0, expected, 0, str(e))

start = time.monotonic()
threads = [threading.Thread(target=worker, args=(i,)) for i in range(N)]
for t in threads: t.start()
for t in threads: t.join()
total_dur = time.monotonic() - start

ok = sum(1 for r in results if r and r[0] == r[1])
total_bytes = sum(r[0] for r in results if r)
mb = total_bytes / (1024 * 1024)
print(f"{ok}/{N} streams completed exactly")
print(f"aggregate: {mb:.1f} MB in {total_dur:.2f}s = {mb / total_dur:.1f} MB/s")

failures = [(i, r) for i, r in enumerate(results) if not r or r[0] != r[1]]
for i, r in failures:
    print(f"  client {i}: {r}")

assert ok == N, f"only {ok}/{N} streams completed correctly"
PY
assert_eq "$?" "0" "all $N concurrent streams returned correct bytes"
assert_log_not_contains "Sanitizer|segfault|Aborted" "no crash markers under concurrent load"
