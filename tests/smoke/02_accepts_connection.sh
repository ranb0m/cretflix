#!/bin/bash
# tests/smoke/02_accepts_connection.sh
# Verifies: dispatcher accepts a TCP connection without crashing.

set -uo pipefail
TEST_NAME="smoke_02_accepts"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"

trap test_cleanup EXIT

server_start

python3 - >> "$LOG_DIR/${TEST_NAME}.log" 2>&1 <<PY
import sys; sys.path.insert(0, "$TESTS_DIR")
from client import Client
with Client(port=$TEST_PORT, media_root="$MEDIA_ROOT") as c:
    pass
print("connect+close ok")
PY
assert_eq "$?" "0" "client connect+close succeeded"
assert_log_not_contains "segfault|Aborted|Sanitizer" "no crash markers in server log"
