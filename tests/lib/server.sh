#!/bin/bash
# tests/lib/server.sh
# ----------------------------------------------------------------------------
# Server lifecycle helpers. Every test starts its own server instance to
# guarantee isolation. Server is launched with stdout+stderr captured to a
# log file in $LOG_DIR; the file is named after the test so failure
# investigation is one `cat` away.
#
# Each test gets a unique port (derived from its name hash) and an isolated
# /tmp/media_test_<test>/ media root. Both are exported to the server via
# env vars (PORT, MEDIA_ROOT) and to the Python client via $TEST_PORT and
# $MEDIA_ROOT.
#
# Public API:
#   server_start         -- generate fixture, spawn server, wait for ready
#   server_stop          -- send SIGTERM, wait for exit (or SIGKILL after 2s)
#   server_fd_count      -- number of open fds in /proc/$SERVER_PID/fd
#   media_root_clear     -- nuke and recreate the per-test media root
#
# Globals exported on success:
#   SERVER_PID           -- the server's PID
#   SERVER_LOG           -- path to its captured log
#   MEDIA_ROOT           -- per-test isolated media root (env to server too)
#   TEST_PORT            -- per-test listen port (env'd to server as PORT)
#
# All tests must `trap test_cleanup EXIT` so the server is killed even on
# test failure.
# ----------------------------------------------------------------------------

# Use the binary in the repo root unless overridden
SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/server}"

# Per-test port: derive from test name hash so collisions are rare even when
# you bounce around running individual tests. Range 8100-8999 (avoid 8080).
test_port() {
    local hash
    hash=$(echo "$TEST_NAME" | cksum | cut -d' ' -f1)
    echo $((8100 + hash % 900))
}

# Per-test media root in /tmp. Survives across test runs but gets nuked on
# server_start so each test starts from a known fixture state.
test_media_root() {
    echo "/tmp/media_test_${TEST_NAME}"
}

# Wipe and recreate $MEDIA_ROOT, then run fixtures/make.sh into it.
media_root_setup() {
    rm -rf "$MEDIA_ROOT"
    mkdir -p "$MEDIA_ROOT"
    "$TESTS_DIR/fixtures/make.sh" "$MEDIA_ROOT" \
        >> "$LOG_DIR/${TEST_NAME}.fixture.log" 2>&1 \
        || { fail "fixture build failed (see ${TEST_NAME}.fixture.log)"; exit 1; }
}

# Allocate test port + media root and start the server.
# Tests can opt out of fixture generation by setting SKIP_FIXTURES=1 before
# calling, or override MEDIA_ROOT/TEST_PORT entirely.
server_start() {
    if [ ! -x "$SERVER_BIN" ]; then
        fail "Server binary not found or not executable: $SERVER_BIN"
        exit 1
    fi

    : "${TEST_PORT:=$(test_port)}"
    : "${MEDIA_ROOT:=$(test_media_root)}"
    export TEST_PORT MEDIA_ROOT

    if [ -z "${SKIP_FIXTURES:-}" ]; then
        media_root_setup
    fi

    SERVER_LOG="$LOG_DIR/${TEST_NAME}.server.log"
    : > "$SERVER_LOG"

    log "Starting server"
    log "  binary:     $SERVER_BIN"
    log "  PORT:       $TEST_PORT"
    log "  MEDIA_ROOT: $MEDIA_ROOT"
    log "  log:        $SERVER_LOG"

    # `setsid` detaches the server from this shell's process group so
    # SIGINT/SIGHUP from the runner don't take the server down with them
    # before our trap handler can stop it cleanly.
    PORT="$TEST_PORT" MEDIA_ROOT="$MEDIA_ROOT" \
        setsid "$SERVER_BIN" >> "$SERVER_LOG" 2>&1 < /dev/null &
    SERVER_PID=$!

    if ! wait_for_log "Entering io_uring loop" 10; then
        fail "Server did not become ready within 10s"
        log "--- server log tail ---"
        tail -20 "$SERVER_LOG" | tee -a "$LOG_DIR/${TEST_NAME}.log"
        kill -9 "$SERVER_PID" 2>/dev/null || true
        exit 1
    fi
    log "Server ready (PID $SERVER_PID)"
}

server_stop() {
    if [ -z "${SERVER_PID:-}" ]; then return 0; fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then return 0; fi

    log "Stopping server (PID $SERVER_PID)"
    kill -TERM "$SERVER_PID" 2>/dev/null || true

    local waited=0
    while kill -0 "$SERVER_PID" 2>/dev/null; do
        sleep 0.1
        waited=$((waited + 1))
        if [ "$waited" -ge 20 ]; then
            log "Server did not exit on TERM, sending KILL"
            kill -9 "$SERVER_PID" 2>/dev/null || true
            break
        fi
    done
    SERVER_PID=""
}

server_fd_count() {
    if [ -z "${SERVER_PID:-}" ]; then echo 0; return; fi
    ls "/proc/$SERVER_PID/fd" 2>/dev/null | wc -l
}

# Default cleanup. Tests can override by defining their own test_cleanup
# before sourcing this file.
test_cleanup() {
    server_stop
    # Leave MEDIA_ROOT in /tmp for post-mortem inspection on failure.
    # The next test_start nukes it via media_root_setup.
    test_summary
    if [ "$TEST_FAILED" -gt 0 ]; then exit 1; fi
}
