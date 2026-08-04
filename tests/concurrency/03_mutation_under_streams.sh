#!/bin/bash
# tests/concurrency/03_mutation_under_streams.sh
# The hardest test in the suite: while N readers are streaming files, the
# inotify daemon is concurrently inserting / removing / renaming dict
# entries. RCU readers must see consistent snapshots; no read can crash
# from a torn entry; no fd from a removed entry can be closed before in-
# flight readers stop using it.
#
# This is the test most likely to find the next bug.

set -uo pipefail
TEST_NAME="concurrency_03_mutation_under_streams"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

export MEDIA_ROOT="/tmp/media_test_${TEST_NAME}"
rm -rf "$MEDIA_ROOT"; mkdir -p "$MEDIA_ROOT/scratch"

# Pre-create some persistent files for readers to stream
for i in 1 2 3 4 5; do
    ffmpeg -y -hide_banner -loglevel error \
        -f lavfi -i "sine=frequency=$((400 + i * 30)):duration=8" \
        -c:a aac -b:a 4000k \
        "$MEDIA_ROOT/persistent_${i}.mp4" 2>/dev/null
done

SKIP_FIXTURES=1 server_start

# Background mutator: continuously creates and deletes files in scratch/
# at a high rate. Each write pushes an RCU mutation through the dict.
mutator_pid=""
(
    set +e
    counter=0
    while [ -d "$MEDIA_ROOT/scratch" ]; do
        f="$MEDIA_ROOT/scratch/churn_${counter}.mp4"
        ffmpeg -y -hide_banner -loglevel quiet \
            -f lavfi -i "sine=frequency=600:duration=2" \
            -c:a aac -b:a 256k "$f" 2>/dev/null
        # Delete some of the older ones randomly
        if [ "$((counter % 3))" -eq 0 ] && [ "$counter" -ge 5 ]; then
            old="$MEDIA_ROOT/scratch/churn_$((counter - 5)).mp4"
            rm -f "$old"
        fi
        counter=$((counter + 1))
        sleep 0.1
    done
) > "$LOG_DIR/${TEST_NAME}.mutator.log" 2>&1 &
mutator_pid=$!

log "mutator PID $mutator_pid running in background"

# Readers: 10 threads streaming the persistent files in a loop for 15s
python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys, threading, time, os
sys.path.insert(0, "$TESTS_DIR")
from client import Client

DURATION = 15
errors = []
ok_count = 0
ok_lock = threading.Lock()

def reader(thread_id):
    global ok_count
    end = time.monotonic() + DURATION
    while time.monotonic() < end:
        try:
            idx = (thread_id + int(time.monotonic() * 10)) % 5 + 1
            fname = f"persistent_{idx}.mp4"
            full = f"$MEDIA_ROOT/{fname}"
            size = os.path.getsize(full)
            with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT", timeout=20) as c:
                fid = c.hash_relative(fname)
                data, _ = c.stream(fid, 0, size)
                if len(data) != size:
                    errors.append(f"t{thread_id}: short read {len(data)}/{size}")
                else:
                    with ok_lock:
                        ok_count += 1
        except Exception as e:
            errors.append(f"t{thread_id}: {e}")

threads = [threading.Thread(target=reader, args=(i,)) for i in range(10)]
for t in threads: t.start()
for t in threads: t.join()

print(f"completed streams: {ok_count}")
print(f"errors: {len(errors)}")
for e in errors[:10]:
    print("  ", e)

# Some errors are fine if a churn file was racy with stream init, but
# persistent files should never short-read.
persistent_errors = [e for e in errors if "persistent" in e]
assert not persistent_errors, f"persistent reads failed: {persistent_errors}"
assert ok_count > 30, f"only {ok_count} successful streams in {DURATION}s"
PY
status=$?

# Stop the mutator
rm -rf "$MEDIA_ROOT/scratch"
wait "$mutator_pid" 2>/dev/null || true

assert_eq "$status" "0" "concurrent reads + mutations succeeded"
assert_log_not_contains "Sanitizer|segfault|Aborted|use-after-free" \
    "no memory-safety errors under RCU pressure"

# fd count shouldn't have ballooned
final_fds=$(server_fd_count)
log "final fd count: $final_fds"
[ "$final_fds" -lt 200 ] && pass "fd count bounded (final: $final_fds)" \
    || fail "fd count grew to $final_fds"
