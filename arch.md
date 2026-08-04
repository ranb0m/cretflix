# ARCHITECTURE SPECIFICATION: High-Performance Asynchronous Media Server

## 1. Executive Summary
The system is a bare-metal, C-based server designed to deliver zero-copy video streaming and microsecond-latency metadata queries. It completely eschews POSIX thread blocking and mutexes in favor of a strictly partitioned, lock-free architecture built atop Linux `io_uring`, Read-Copy-Update (RCU) concurrency, and Multi-Version Concurrency Control (MVCC) via an embedded SQLite B-Tree.

---

## 2. Hardware Topology & CPU Pinning
The fundamental constraint of high-performance systems engineering is the physical reality of the silicon. To eliminate CPU cache thrashing, bus locking, and context-switching overhead, the architecture enforces strict thread-to-core physical pinning.

*   **Core 0 (The Dispatcher):** The ingress gateway. Executes the master `io_uring` event loop. Handles all TCP handshakes, socket validation, fast-path streaming, and $O(1)$ memory lookups. It is mathematically forbidden from blocking, sleeping, or executing synchronous disk I/O.
*   **Core 1 (The Daemons):** The background substrate.
    *   *Logger Daemon:* Drains the lock-free SPSC and MPSC log queues and executes RCU Garbage Collection.
    *   *Ingestion Daemon:* Executes `inotify` filesystem watches, hashes new files, and asynchronously writes to the SQLite B-Tree.
*   **Cores 2+ (The Worker Matrix):** The Control Plane execution pool. A dynamically scaled pool of threads (based on available silicon) that poll a Single-Producer, Multiple-Consumer (SPMC) queue. They execute computationally expensive tasks: TLV parsing, relational SQL queries, and string formatting.

---

## 3. Inter-Thread Communication (The Eradication of IPC)
The architecture achieves microsecond socket handoff by strictly relying on shared-memory concurrency, explicitly rejecting legacy Inter-Process Communication (IPC) mechanisms.

### 3.1. The UDS / SCM_RIGHTS Fallacy
Traditional web servers utilizing a multi-process (`fork()`) model must employ Unix Domain Sockets (UDS) and `SCM_RIGHTS` payload headers to pass File Descriptors across isolated process boundaries. Because this architecture is exclusively multi-threaded (`pthreads`), all execution contexts mathematically share a singular, global File Descriptor table. The use of UDS for thread-to-thread communication is an anti-pattern that introduces catastrophic user-to-kernel context switching overhead merely to transmit a 32-bit integer.

### 3.2. Lock-Free User-Space Handoff
To completely bypass the kernel during connection routing, the server employs a Single-Producer, Multiple-Consumer (SPMC) ring buffer mapped in user-space RAM.
1.  **The Push:** When Core 0 (The Dispatcher) validates a client's 64-byte TCP header and determines it requires Control Plane processing, it executes an atomic hardware instruction to push the raw integer File Descriptor into the ring buffer.
2.  **The Pop:** The Worker Matrix continuously polls the buffer utilizing `_mm_pause()` thermal backoff. A Worker atomically claims the index, retrieves the integer, and instantly begins reading the socket payload. 
This strictly shared-memory handoff reduces routing latency from kernel-bound millisecond scales to deterministic nanosecond scales.

## 4. The Data Plane (Fast-Path Streaming)
The Data Plane is responsible for physical byte delivery. It operates entirely within Core 0 and bypasses user-space memory entirely.

### 4.1. Zero-Copy `splice` Pipeline
File delivery utilizes a kernel-level pipe bridge. Data is pulled from the NVMe block device via `splice()` into the pipe, and subsequently pushed via a second `splice()` into the TCP socket. Data never crosses the kernel/user-space boundary.

### 4.2. Asynchronous Token Bucket Pacing
To prevent overwhelming client network buffers, the Dispatcher implements a mathematical Token Bucket algorithm. 
*   Tokens are generated dynamically based on the file's pre-calculated bitrate and the precise `clock_gettime(CLOCK_MONOTONIC)` nanosecond delta.
*   If a client requests a chunk larger than the bucket allows, the Dispatcher injects an `IORING_OP_TIMEOUT` SQE to yield the kernel loop, shifting the socket into `STATE_PACING_WAIT` until sufficient time has elapsed.

### 4.3. Lock-Free RCU Dictionary
File metadata required for the Fast Path is stored in a highly optimized $O(1)$ array. Lookups utilize Knuth Multiplicative Hashing resolved via Robin Hood linear probing.
*   **Concurrency:** The Worker/Ingestion threads update the dictionary using strict Read-Copy-Update (RCU). They clone the array, apply the mutation, and execute an `atomic_store_explicit(..., memory_order_release)` to hot-swap the pointer. Core 0 reads the pointer via `memory_order_acquire` without ever executing a lock.

---

## 5. The Relational Control Plane (Metadata Engine)
The Control Plane handles discovery, search, and fuzzy logic. It is powered by an embedded SQLite B-Tree, meticulously decoupled to prevent database locks from halting the network matrix.

### 5.1. Thread-Local Bytecode Compilation
To prevent Virtual Machine state corruption across 100+ parallel connections, the Dispatcher initializes a single `sqlite3 *global_db` pointer. However, during boot, every Worker thread physically compiles its own private bytecode instructions (`sqlite3_stmt`) via `sqlite3_prepare_v2`. This isolation allows 100 Workers to simultaneously execute `sqlite3_step()` across the mapped memory file without POSIX lock collisions.

### 5.2. MVCC via Write-Ahead Logging (WAL)
The B-Tree is initialized with `PRAGMA journal_mode=WAL;`. This enforces Multi-Version Concurrency Control. The single Ingestion Daemon (Writer) appends to a separate `-wal` disk file, mathematically guaranteeing that the Worker Matrix (Readers) are never blocked by filesystem mutations.

### 5.3. Type-Length-Value (TLV) Protocol
Clients interface with the Control Plane by submitting a 64-byte `MediaHeader` with `CMD_QUERY_METADATA`, followed by a binary TLV payload. The Workers parse the TLV, bind the extracted value to their thread-local SQL virtual machine, execute the search, and return a JSON-formatted payload containing the $O(1)$ 64-bit `file_id`.

---

## 6. Network Armor & State Machine
The Dispatcher event loop is hardened against hostile actors and physical resource exhaustion via a deterministic finite state machine.

*   **The Guillotine (Slowloris Defense):** Upon accepting a connection, Core 0 submits an `IORING_OP_RECV` for the 64-byte header, simultaneously submitting an unlinked 5-second `IORING_OP_LINK_TIMEOUT`. If the timer expires before the header is received, the kernel automatically aborts the read (returning `-ECANCELED`), and the Dispatcher severs the socket.
*   **FD Exhaustion Backoff:** If the OS file descriptor limit is hit, `accept()` returns `-EMFILE`. The architecture intercepts this, injects a 100ms `IORING_OP_TIMEOUT` SQE, and shifts to `STATE_ACCEPT_BACKOFF`, peacefully yielding the core until the FD pressure subsides.
*   **The Cancellation Matrix:** If a client physically terminates a connection (TCP RST) while the Dispatcher is awaiting a disk or pacing operation, the server traps the failure and injects an `IORING_OP_CANCEL` SQE, transitioning to `STATE_CANCELLING` to mathematically prevent Use-After-Free segfaults.

---

## 7. Substrate Ingestion & Telemetry

### 7.1. Dual-Phase Ingestion
1.  **Boot Phase:** Worker 0 executes a recursive filesystem traversal mapping all valid media files into the initial RCU Matrix before releasing the boot barrier.
2.  **Runtime Phase:** The Ingestion daemon utilizes `inotify` to monitor the media directory. When a file is dropped, it computes the djb2 hash, executes the RCU memory mutation (Hot Path), and performs an `INSERT` into the SQLite B-Tree (Cold Path).

### 7.2. Asynchronous Logging
`printf` is a blocking system call. The architecture forbids its use on the network core.
*   The Dispatcher wait-free pushes log metadata (format strings and integers) into a Single-Producer, Single-Consumer (SPSC) ring buffer.
*   The Worker Matrix pushes into a Multiple-Producer, Single-Consumer (MPSC) ring buffer utilizing an atomic commit-flag to prevent Data Tearing.
*   The Logger Daemon (Core 1) wakes every 1 millisecond, drains both buffers, executes the physical `printf` to the terminal, and safely frees memory dropped into the `rcu_garbage_bin`.

kernel config for latency
	loaded with elilo
	slackware linux custom compiled kernel
	pruned POSIX, sysvinit w/ no background logging
	config_preempt_NONE selected
	sysvinit, no systemd, also wondering if htere's a way to get rid of initrd and if that would be beneficial in any way in terms of app latency



client:
in its most basic form
must read bytes, demux with libavformat, or whatever else, then decode with libavcodec, then render