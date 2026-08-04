#!/bin/bash

# Exit immediately if any command fails
set -e

cd "$(dirname "$0")/.."

echo "=== [1/5] INITIATING HPC PRE-FLIGHT CHECKS ==="
# Kill any dangling server processes from previous crashed tests
sudo pkill -9 server || true
sudo rm -f /tmp/media_worker.sock

echo "=== [2/5] CONFIGURING KERNEL & STORAGE SUBSTRATE ==="
# 1. Apply NVMe power state configurations
sudo ./scripts/nvme_apst.sh

# 2. Mount the XFS partition (only if it isn't already mounted)
if ! mountpoint -q /mnt/media_test; then
    echo "Mounting XFS partition to /mnt/media_test..."
    sudo mount -t xfs -o noatime,largeio,inode64 /dev/nvme0n1p6 /mnt/media_test
else
    echo "XFS partition already mounted."
fi

echo "=== [3/5] SYNTHESIZING DICTIONARY MATRIX ==="
sudo touch /mnt/media_test/subtitles.srt

if [ ! -f /mnt/media_test/Inception.mkv ]; then
    echo "Synthesizing 1GB dummy file for Inception.mkv..."
    sudo dd if=/dev/urandom of=/mnt/media_test/Inception.mkv bs=1M count=1024 status=none
fi

# HPC FIX: Grant ownership to the current user so the server NEVER needs sudo
sudo chown -R $USER: /mnt/media_test

echo "=== [4/5] COMPILING ARCHITECTURE ==="
make clean && make

echo "=== [5/5] LAUNCHING EVENT-DRIVEN TMUX ORCHESTRATOR ==="
SESSION_NAME="media_harness"
STATE_FILE="/tmp/media_test_stage1_done"

# Clear any old synchronization states
rm -f $STATE_FILE
tmux kill-session -t $SESSION_NAME 2>/dev/null || true
tmux new-session -d -s $SESSION_NAME

# --- Pane 1: The Server (Top Half) ---
# Notice we dropped 'sudo'. It runs cleanly in userspace.
tmux send-keys -t $SESSION_NAME "./server" C-m
tmux split-window -v -p 40 -t $SESSION_NAME

# --- Pane 2: The Integration Suite (Bottom Left) ---
# Native bash TCP polling. It waits mathematically for the port to open, runs the test, and signals completion.
POLL_CMD="echo 'Waiting for Dispatcher to bind port 8080...'; while ! (echo > /dev/tcp/127.0.0.1/8080) &>/dev/null; do sleep 0.1; done"
RUN_CLIENT_CMD="python3 scripts/client-test.py && touch $STATE_FILE"
tmux send-keys -t $SESSION_NAME "$POLL_CMD; clear; $RUN_CLIENT_CMD" C-m

tmux split-window -h -t $SESSION_NAME

# --- Pane 3: The Concurrency Storms (Bottom Right) ---
# Silently waits for the state file to be touched by Pane 2.
SYNC_CMD="echo 'Waiting for Integration Suite to finish...'; while [ ! -f $STATE_FILE ]; do sleep 0.1; done"

# We run the Streaming Storm first, wait 3 seconds, then detonate the Metadata Contention Storm
RUN_STORMS_CMD="python3 scripts/stress_test.py && sleep 3 && clear && python3 scripts/metadata_storm.py"

tmux send-keys -t $SESSION_NAME "$SYNC_CMD; clear; $RUN_STORMS_CMD" C-m

# Lock focus on the server logs
tmux select-pane -t $SESSION_NAME -U
tmux attach-session -t $SESSION_NAME
