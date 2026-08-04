# Media Server — Project Context

This document is for a Claude instance picking up work on a low-latency media streaming server. Read it once, then start. The user is `cristian`; he's the sole developer and operator.

## What it is

A C-based media streaming server. Friends-tier deployment (think: locally-hosted 123movies for ~5-10 friends). Stack: io_uring, RCU, lock-free queues, zero-copy splice via Linux pipe pairs, token-bucket pacing per connection. Custom binary protocol, 64-byte aligned headers + optional TLV payloads.

Hardware: production target is a single Lenovo workstation, ~16-core Intel, 16TB NVMe (XFS), Slackware Linux, custom-compiled kernel. User develops on a laptop with NVMe + same custom kernel (less tuned).

Scale: a few thousand media files maximum, never more. Concurrent streams: small dozens. This is **not** a CDN; it is one machine serving people the operator personally knows.

## User communication conventions — read this before responding

These are not suggestions. The user's preferred mode:

- **Terse technical prose. No bullets unless the content is genuinely a list.** Don't write `**Bold:** explanation` for every paragraph. Headers are fine when they help scanning; they are not decoration.
- **No "Welcome!" or "Great question!" preambles.** Get to the point in sentence one.
- **No "Let me know if you have questions" closers.** End on substance.
- **Pragmatic tradeoffs over architectural purity.** This is a friends-tier server; "what if a hostile actor" is rarely the right framing. He will tell you when adversarial robustness matters.
- **Push back when something is wrong.** He has explicitly thanked the assistant for catching design errors. Sycophancy is worse than disagreement here.
- **Discussion shape:** design → decisions → code → notes on what's not handled → updated open list → "which next?" — he often replies with `let's do X next` and expects the assistant to ask which open item to tackle when a pass completes.
- **Don't ask permission for obvious next steps** within an in-flight task. If you're mid-refactor and there's an inevitable cleanup, just do it.
- He's not always rigorous with capitalization or punctuation. Don't mirror the style; just respond normally.

When uncertain whether to do or describe a task: do it, then describe what you did. Showing working code beats prose every time.

## Prior conversations

Every prior session is in `/mnt/transcripts/`. The most recent and richest is `/mnt/transcripts/2026-05-01-04-34-08-media-server-audit-refactor.txt`. A catalog lives at `/mnt/transcripts/journal.txt`. Use the `view` tool on these when you need the full audit reasoning behind a decision; don't re-derive things that were already settled.

## Architecture in 200 words

Three thread roles, all pinned:
- **Dispatcher** (core 0): owns the io_uring ring, accepts connections, runs the connection FSM (`STATE_READ_HEADER` → `STATE_READ_PAYLOAD` for slow-path, or → `STATE_SENDFILE` ↔ `STATE_PACING_WAIT` for fast-path streaming). All splice operations go through here. Reads the dictionary; never writes.
- **Workers** (cores 2+): pop `JobPayload`s from a lock-free MPMC work queue and CPU-process slow-path commands (currently just `CMD_QUERY_METADATA` substring search). Pure consumers, never write the dict.
- **Ingestion daemon** (core 1): owns boot scan AND runtime inotify. Sole writer to the dictionary. Does recursive directory walk at boot, registering inotify watches per directory as it descends, then atomically publishes the dict and enters the event loop.
- **Logger** (core 1, separate thread): drains two lock-free queues — SPSC for dispatcher, MPSC for workers — and writes to stdout. Also drives RCU GC: calls `gc_drain()` once per loop iteration, which runs `rcu_synchronize` and closes any displaced fds enqueued by writers.

Key data structures:
- `global_dictionary`: `_Atomic(struct FileEntry*)`, swapped via copy-on-write under RCU. Entries hold `file_id` (djb2 of full path), open `fd`, size, bitrate, and the relative path for display/search.
- `reader_states[MAX_READERS]`: per-reader sequence counters for QSBR-lite grace-period detection. Reader IDs: dispatcher=0, workers 1..N.
- `gc_queue`: SPSC ring of fds awaiting close after a grace period.
- `work_queue`: MPMC fixed-slot queue of `JobPayload` (header + heap-alloc payload pointer).

Hot paths:
- Streaming: client → dispatcher's accept → header read → dict lookup (RCU read) → `dup()` of file fd → `pipe2()` per stream → splice file→pipe→socket loop with token-bucket between iterations.
- Metadata: dispatcher reads header + payload, pushes `JobPayload` to work queue, free-runs; worker pops, scans dict via substring match on relative-path field, writes JSON response, closes socket.
- Inotify: kernel event → daemon constructs full path from watch table + event name → routes to insert/remove/rename → RCU writer copies dict, mutates, swaps; old dict's displaced fd goes through GC.

## File layout

```
media-server/
├── Makefile
├── server                          # built binary
├── include/
│   ├── common.h                    # FileEntry, ClientContext, daemon prototypes
│   ├── protocol.h                  # MediaHeader (64B), TLV_Item, command IDs
│   ├── logger.h                    # WORKER_LOG (CAS MPSC), DISPATCH_LOG (SPSC)
│   ├── work_queue.h                # MPMC queue, JobPayload struct
│   └── rcu.h                       # ReaderState, rcu_read_lock/unlock, gc API
├── src/
│   ├── dispatcher.c                # io_uring loop, connection FSM, splice pipeline
│   ├── worker.c                    # ingestion daemon + worker pool + libavformat
│   ├── logger.c                    # log drains + GC driver
│   └── rcu.c                       # dict writers, internal_insert, gc_enqueue
├── scripts/
│   └── nvme_apst.sh                # forces drive to PS0 (no APST sleep states)
├── tests/                          # see Testing section below
└── logs/                           # generated; one timestamped dir per run
```

Compile-time defaults that are now runtime-overridable:
- `MEDIA_ROOT`: env var `MEDIA_ROOT`, falls back to `/mnt/media_test`. Resolved once at top of `ingestion_daemon_main`. `media_root_len` cached, single trailing slash stripped.
- `PORT`: env var `PORT`, falls back to 8080. Resolved at top of `init_server`.

Compile-time constants worth knowing (in common.h / source):
- `DICT_CAPACITY` — max concurrent file entries in the dictionary
- `MAX_PAYLOAD_SIZE` — caps slow-path payload reads
- `MAX_CONCURRENT_CLIENTS` — listen backlog
- `BURST_SIZE` — used by the legacy fadvise/prefetch logic (now mostly vestigial since APST is disabled, but still warms page cache)
- `MAX_PATH_LEN` 1024, `MAX_WATCHES` 1024, `MAX_PENDING_MOVES` 16, `GC_QUEUE_SIZE` 64, `MAX_READERS` 64

## Completed work (audit fixes)

In rough chronological order from the audit. Everything here was discussed, decided, implemented, and tested:

1. **SQLite removed.** Replaced with in-memory `strcasestr` linear scan over `global_dictionary`. The "control plane" SQLite handle had a single `sqlite3*` shared across workers, which was actually serialized — a worse pattern than just scanning RAM. Files: deleted `database.c`, `ipc.h`, `ipc.c` from the project. The relative-path-based filename in `FileEntry` is what gets searched.

2. **Worker `read()` deblocked.** Previously workers did synchronous `read()` for slow-path payloads. Moved into dispatcher's io_uring loop with a linked 5s timeout. New `add_read_payload_request()`, activated `STATE_READ_PAYLOAD`. `ClientContext` now has a heap-allocated `payload_buf` that the dispatcher fills before pushing to workers.

3. **RCU rework.** Replaced `sleep(1)` (fake grace period) with QSBR-lite: per-reader sequence counters in `reader_states[]`, real `rcu_synchronize()` polls all readers past their start sequences. Replaced single-slot `rcu_garbage_bin` with SPSC ring buffer (`gc_queue`). Fixed fd lifecycle race: `lookup_file_metadata()` `dup()`s the fd at extraction so the dispatcher can't have it pulled out from under it during a concurrent rename/delete. `internal_insert()` now returns the displaced fd; RCU writers route through GC, boot ingestion (single-threaded) closes directly. RCU writers moved from `dispatcher.c` into `rcu.c`.

4. **MPSC log queue deadlock fix.** Worker log macro previously did unconditional `fetch_add(tail)`, which created abandonment paths if the producer crashed mid-write. Replaced with CAS-based reservation: only commits when slot is in `[head, head + SIZE)`. Drops are silent on full-queue. SPSC dispatch log was unaffected.

5. **Bitrate via libavformat.** Replaced `st.st_size / 7200` placeholder with real `avformat_open_input` + `avformat_find_stream_info` probe. Returns bytes/sec capped at `UINT32_MAX`. Failure → 0 → dispatcher's pacing logic skips the token bucket entirely (graceful degradation). `pthread_once` sets `av_log_set_level(AV_LOG_ERROR)` to silence libavformat's chatter. Makefile gained `-lavformat -lavcodec -lavutil`.

6. **Path-aware hashing + recursive inotify.** Renamed `hash_filename` → `hash_path`. Now hashes the full path (`/mnt/media_test/movies/inception.mkv`), not the basename. `FileEntry::filename` stores the relative path (`movies/inception.mkv`) for display + substring search. New `relative_path()` helper. Watch table maps `wd → directory_path`; boot scan adds an inotify watch for each directory before descending into it. Runtime: `IN_CREATE | IN_ISDIR` and `IN_MOVED_TO | IN_ISDIR` trigger `handle_new_directory()` which adds a watch + recursively scans for files that arrived in the race window. `IN_IGNORED` cleans up the watch table when the kernel drops a watch (e.g. dir deleted). Cookie matching for renames stores full paths in `PendingMove`, which makes cross-directory mv work. Symlinks skipped via d_type. **Not supported**: directory rename — the kernel doesn't replay events for files inside a renamed dir.

7. **NVMe APST inversion.** `scripts/nvme_apst.sh` was doing the opposite of what was wanted: setting `pm_qos_latency_tolerance_us=50000` permits PS3/PS4 deep sleep, creating the 46ms wake-stall the architecture doc cited as a problem. Inverted to set tolerance to 0 (drive stays in PS0). Documented persistent fix: `nvme_core.default_ps_max_latency_us=0` on kernel cmdline. The thermal-throttling justification in the original script doesn't hold for read-heavy workloads.

8. **Pacing deadlock.** Found by `protocol/01_stream_basic.sh` on first run of the new harness. `add_sendfile_request()` had `min_burst = min(remaining, 65536)` and `max_tokens = bitrate * 2`, which deadlocked any file with bitrate < 32 KB/s — tokens cap below `min_burst`, wait never completes. One-line fix: `if (min_burst > max_tokens) min_burst = max_tokens;`. Real video at 1+ Mbps was unaffected; low-bitrate audio fixtures or low-bitrate audio-only media files would lock up.

9. **Runtime-configurable `MEDIA_ROOT` and `PORT`.** Both are now env-driven. Required for the test harness to use `/tmp/media_test_<test>/` per test rather than fighting over `/mnt/media_test`.

## Test harness

Lives at `tests/`. Run with `bash tests/run.sh` from repo root, or subset with `bash tests/run.sh smoke protocol`. Logs accumulate in `logs/<timestamp>/`; one server log + one client log + one runner log per test, all timestamp-correlated. Open with `lnav logs/<timestamp>/` for unified scrollable view.

Layout:
- `tests/run.sh` — discovers + runs + summarizes
- `tests/lib/common.sh` — colors, assertions (`pass`, `fail`, `assert_eq`, `assert_log_contains`, `assert_log_not_contains`, `wait_for_log`), `test_summary`
- `tests/lib/server.sh` — `server_start`, `server_stop`, `server_fd_count`, `media_root_setup`, `test_cleanup`. Auto-allocates per-test port (8100-8999, hash of test name) and `/tmp/media_test_<test>/` root
- `tests/fixtures/make.sh` — generates real probeable media files via ffmpeg lavfi (sine waves → AAC). Critically NOT `dd urandom`, which produces unprobeable bytes that silently disable pacing
- `tests/client.py` — protocol-aware Python client. Used as library (`from client import Client`) or CLI (`python3 client.py hash /path`, `... stream foo.mkv`, `... query Batman`). Hashes are over the FULL path; `c.hash_relative("foo")` joins with `media_root` for you
- 5 categories: `smoke/`, `protocol/`, `inotify/`, `concurrency/`, `lifecycle/`. 20 tests written, structure documented below

Pattern every test follows:
```bash
set -uo pipefail
TEST_NAME="category_NN_short_name"
source "$(dirname "$0")/../lib/common.sh"
source "$(dirname "$0")/../lib/server.sh"
trap test_cleanup EXIT
server_start  # auto-fixtures + port + media root
# ... assertions ...
```

Each `server_start` wipes `/tmp/media_test_<test>/`, regenerates fixtures, launches server with env vars, blocks until `Entering io_uring loop` appears in its log (10s timeout). `trap test_cleanup EXIT` ensures the server is killed even on assertion failure.

Tests by category and what each covers:

**smoke** — does it boot:
- `01_starts.sh` — boots, ingests fixtures, filter rejects .srt, port log line matches
- `02_accepts_connection.sh` — TCP accept works, connect+close doesn't crash

**protocol** — wire format + commands:
- `01_stream_basic.sh` — full-file stream, byte count exact
- `02_invalid_id.sh` — bogus file_id closes connection cleanly, server stays up
- `03_range_request.sh` — partial range, byte-identical to dd at that offset
- `04_malformed_header.sh` — bad magic, unknown command, oversized payload_len; server stays up
- `05_metadata_search.sh` — FOUND/NOT_FOUND JSON shapes, multi-result correctness
- `06_extension_filter.sh` — .srt verifiably absent from dict (via metadata query for "subtitles" → NOT_FOUND)

**inotify** — runtime mutations:
- `01_path_aware_hash.sh` — same-basename files in different subdirs get distinct IDs (catches the basename-hash bug from the audit)
- `02_recursive_subdir.sh` — runtime mkdir + file inside, watch added + indexed
- `03_cross_dir_rename.sh` — mv across directories matches via cookie, old hash dies, new lives
- `04_delete_routes_to_gc.sh` — rm a file, verify fd count drops (GC actually closes the fd)
- `05_intra_dir_rename.sh` — rename within one watch produces single rename event, not del+add pair

**concurrency** — load + RCU correctness:
- `01_n_streams.sh` — 20 concurrent streams of high-bitrate files, all complete with correct bytes
- `02_disconnect_mid_stream.sh` — 10 violent RST disconnects, fd count returns to baseline
- `03_mutation_under_streams.sh` — 10 readers stream persistent files for 15s while a background mutator churns scratch files (insert/delete every 100ms). Persistent reads must never short-read. **This is the test most likely to find the next bug.**
- `04_connection_storm.sh` — 500 simultaneous open sockets held 3s, server still responsive, fd count returns near baseline after close

**lifecycle** — long-term stability:
- `01_fd_stable.sh` — 100 sequential stream cycles, fd delta ≤ ±2
- `02_long_run_rss.sh` — sustained streaming for `$DURATION` (default 60s, env-tunable; bump to 600+ for soak), max RSS ≤ 2× baseline
- `03_sigterm_clean_exit.sh` — SIGTERM during active streams exits within 5s, no segfault

When the user reports a test failure, the relevant log files are:
- `logs/<ts>/<category>_<name>.runner.log` — the runner's view (assertion outputs)
- `logs/<ts>/<category>_<name>.log` — the test script's own log
- `logs/<ts>/<category>_<name>.server.log` — full server stdout/stderr for that test
- `logs/<ts>/<category>_<name>.client.log` — Python client output where applicable
- `logs/<ts>/<category>_<name>.fixture.log` — ffmpeg output during fixture build

Pull all four when diagnosing. Don't speculate without reading them.

## Open TODOs

### Already on the list (small)

- 64-byte-vs-40-byte comment cleanup in `protocol.h` (says "Exactly 40 bytes", actual struct is 64 — comment lies)
- Kernel cmdline: `isolcpus=`, `nohz_full=`, `rcu_nocbs=`, `nvme_core.default_ps_max_latency_us=0`. Slackware uses LILO; user has the custom kernel built but cmdline isn't fully tuned. Config file edit, no code change.
- More tests not yet written: `inotify/06_directory_delete.sh` (IN_IGNORED watch_table cleanup), `inotify/07_watch_table_full.sh` (graceful degradation past MAX_WATCHES), `concurrency/05_metadata_storm.sh` (100 concurrent metadata queries with real assertions), `lifecycle/04_valgrind_clean.sh` (short run under valgrind, zero leaks)

### Missing functionality before this is a usable streaming server

The protocol works, but a streaming client (mpv, VLC, web player, custom) can't actually use it as-is. In rough priority order for a v1 prototype:

**Listing API.** Currently the only way to discover files is to substring-search via `CMD_QUERY_METADATA`. A client needs to enumerate. Either a new command (`CMD_LIST_FILES` returning a paginated list of `{file_id, relative_path, size, bitrate}`) or — better — an HTTP gateway (see below) with a `GET /api/files` endpoint.

**HTTP gateway.** The custom binary protocol forecloses on every off-the-shelf player. Two options:
- (a) Build a small HTTP frontend in the same process on a different port. `GET /stream/<file_id>` translates HTTP `Range:` headers into the existing FSM. `GET /api/files` returns JSON listing. This is the universal-compat play.
- (b) Run a separate gateway process (Go, Rust, whatever) that speaks HTTP outward and the binary protocol inward. More moving parts but cleaner separation.
For friends-tier, (a) is probably the right call. Keep the binary protocol for the few cases where you want to skip HTTP framing overhead (large in-LAN streams).

**Authentication.** `CMD_AUTHENTICATE` exists in the protocol (`0x0030`) but isn't implemented. Friends-tier minimum: shared secret in the header's `flags` or a TLV field, checked once per connection before any other command is honored. If remote access ever happens, this needs TLS — likely via stunnel in front rather than libtls integration.

**Time-based seeking.** `CMD_STREAM_FILE` takes byte ranges. Players seek by time. Either store a keyframe index per file (parsed at ingestion time, alongside bitrate) or accept that the player has to estimate position via `byte_offset = duration * bitrate * fraction` (works adequately for CBR, awful for VBR). Storing a simple `(time_ms, byte_offset)` table at ingestion is the right answer; it costs maybe 10KB per file in the dict.

**Richer metadata.** `FileEntry` has `file_id`, `fd`, `size`, `bitrate`, `filename`. A real player wants duration, codec, video resolution, audio channels, subtitle stream presence. libavformat already gives us this during the bitrate probe; we throw it away. Add fields, return in metadata responses.

**Error responses.** Currently the dispatcher closes the connection on any failure (bad id, bad header, file gone). The client gets EOF and has no idea why. Define a small set of error codes and a one-shot reply format (e.g. magic + version + command=0x00FE + error_code) before the close.

**Graceful shutdown.** SIGTERM kills threads abruptly. In-flight streams just RST. Add a signal handler that flips a "draining" atomic, dispatcher stops accepting new connections, workers drain the queue, ingestion daemon stops processing inotify events, then exit. Lifecycle/03 currently passes only because abrupt exit ≠ crash; tighten when this is implemented.

**Resume / range-seek robustness.** A reconnecting client should be able to say "give me file X starting at byte Y" — it can already, via `start_byte`. But there's no playback session ID, so there's no way to know a reconnection IS a resume vs a fresh stream. For per-client analytics or bandwidth fairness this matters; for friends-tier basic playback, it doesn't.

**Per-client bandwidth budget.** Token bucket is per-stream, sized by file bitrate. Friend on a slow link competing with a friend on a fast link gets no fairness. Probably YAGNI for friends-tier; note for later.

**Health/metrics endpoint.** Hard to know if the server is misbehaving without one. Even a simple `GET /health` returning `{streams_active, dict_size, fd_count, uptime}` would help. Falls naturally out of the HTTP gateway item.

**Subtitles + multi-track.** Currently `.srt` is filtered out at ingestion. A real player wants subtitles as a separate stream (or sidecar files served alongside the main file). MKV embeds them; MP4 typically uses sidecar SRT. Decide: serve sidecars (.srt next to .mkv → present in metadata), or pass through MKV's embedded tracks (no work needed, but client needs to know they're there).

### Hardware/system tuning still pending

- Confirm the laptop's NVMe respects host PM hints (some consumer firmwares don't; would surface as wake-stall in benchmarks despite APST being off in software)
- Set up the production XFS partition with `mount -o noatime,largeio,inode64` permanently in fstab
- Bump `/proc/sys/fs/inotify/max_user_watches` if the library grows past ~5000 subdirs
- Bump `ulimit -n` for the server process if connection counts ever justify it

## Build + invariants

- `make` produces `./server` (48KB binary at -O3 -flto)
- Build is clean under `-Wall -Wextra -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wundef`
- Build is clean under `-fsanitize=undefined,address`
- Single dictionary writer (ingestion daemon). Multiple readers (dispatcher + worker pool). Guard this invariant when adding code.
- Don't use `printf`/`fprintf` directly outside of fatal-error paths — use `WORKER_LOG` or `DISPATCH_LOG`. Direct prints break the lock-free logging guarantee.
- `WORKER_LOG` takes 4 uint64_t-castable args after the format string; pad with zeros if fewer. The format string is what gets `printf`'d in the logger thread.
- All RCU writers must route displaced fds through `gc_enqueue`, never `close()` directly. The exception is boot scan, which runs single-threaded before any reader exists.

## Resume point

The user just received the env-var-configurable build + the full test harness (20 tests across 5 categories). The runtime fix and harness wiring were verified end-to-end in the dev environment but the user is going to run the suite on his own hardware (laptop with NVMe) and report results.

Expected next message: test output. Failures most likely to appear:
- `concurrency/03_mutation_under_streams.sh` — RCU correctness under pressure; this is the deepest test and most likely to surface latent bugs in the displaced-fd routing or grace-period detection
- `lifecycle/01_fd_stable.sh` — slow leaks the eyeball can't catch; a delta of even +5 indicates something not closing
- `concurrency/02_disconnect_mid_stream.sh` — fd not reclaimed after RST suggests the cancellation path in dispatcher doesn't fully clean up the per-stream pipe and dup'd fd

When test output arrives: read the listed log files first, don't speculate. Diagnose, propose a fix, implement, re-test. The user will say `let's do X next` for picking the work order if multiple things fail.

Open list maintenance: when a fix lands, strikethrough the relevant TODO line and report the updated list back to him at the end of the response. He uses these as the running task list.
