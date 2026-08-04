#!/bin/bash
# tests/lib/common.sh
# ----------------------------------------------------------------------------
# Sourced by every test. Provides:
#   - colored pass/fail output
#   - assertion helpers
#   - log file paths and a tee-style logger
#
# Tests should source this exactly once at the top:
#   source "$(dirname "$0")/../lib/common.sh"
# ----------------------------------------------------------------------------

# Colors only if stdout is a tty
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    CYAN='\033[0;36m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' CYAN='' BOLD='' NC=''
fi

# State for the assertion counters within a test
TEST_PASSED=0
TEST_FAILED=0
TEST_NAME="${TEST_NAME:-$(basename "$0" .sh)}"

# Resolve repo root once so tests can find fixtures, server binary, etc.
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
LOG_DIR="${LOG_DIR:-$REPO_ROOT/logs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$LOG_DIR"

# log <message>  -> writes to test log AND stdout with timestamp
log() {
    local ts
    ts="$(date '+%H:%M:%S.%3N')"
    echo "[$ts] $*" | tee -a "$LOG_DIR/${TEST_NAME}.log"
}

# pass <description>
pass() {
    TEST_PASSED=$((TEST_PASSED + 1))
    echo -e "  ${GREEN}PASS${NC}: $1" | tee -a "$LOG_DIR/${TEST_NAME}.log"
}

# fail <description>  -> records a failure, does NOT exit
fail() {
    TEST_FAILED=$((TEST_FAILED + 1))
    echo -e "  ${RED}FAIL${NC}: $1" | tee -a "$LOG_DIR/${TEST_NAME}.log"
}

# assert_eq <actual> <expected> <description>
assert_eq() {
    if [ "$1" = "$2" ]; then
        pass "$3"
    else
        fail "$3 (expected '$2', got '$1')"
    fi
}

# assert_log_contains <pattern> <description>
# Searches the server log for a regex pattern.
assert_log_contains() {
    local pattern="$1"
    local desc="$2"
    if grep -qE "$pattern" "$SERVER_LOG" 2>/dev/null; then
        pass "$desc"
    else
        fail "$desc (pattern '$pattern' not found in server log)"
    fi
}

# assert_log_not_contains <pattern> <description>
assert_log_not_contains() {
    local pattern="$1"
    local desc="$2"
    if ! grep -qE "$pattern" "$SERVER_LOG" 2>/dev/null; then
        pass "$desc"
    else
        fail "$desc (pattern '$pattern' unexpectedly found in server log)"
    fi
}

# wait_for_log <pattern> <timeout_seconds>
# Blocks until pattern appears in $SERVER_LOG or timeout elapses.
# Returns 0 on match, 1 on timeout.
wait_for_log() {
    local pattern="$1"
    local timeout="${2:-10}"
    local elapsed=0
    while [ "$elapsed" -lt "$((timeout * 10))" ]; do
        if grep -qE "$pattern" "$SERVER_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# Test summary: call from teardown trap
test_summary() {
    local total=$((TEST_PASSED + TEST_FAILED))
    echo ""
    if [ "$TEST_FAILED" -eq 0 ]; then
        echo -e "${GREEN}${BOLD}[$TEST_NAME] $TEST_PASSED/$total assertions passed${NC}"
    else
        echo -e "${RED}${BOLD}[$TEST_NAME] $TEST_FAILED/$total assertions FAILED${NC}"
    fi
    echo "  log: $LOG_DIR/${TEST_NAME}.log"
}
