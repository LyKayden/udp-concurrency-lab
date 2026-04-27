# udp-concurrency-lab

A practical exploration and implementation of a high-concurrency UDP server in Linux. This project documents a step-by-step debugging journey, from observing unexpected packet routing behavior to building a stable, session-routed UDP gateway that handles asynchronous kernel dispatch, race windows, and traffic spikes.

---

## 📖 The Exploration Journey

This repository records a continuous process of testing, observing, analyzing kernel behavior, and refining the architecture. It focuses on solving real problems encountered during development rather than pursuing theoretical ideals.


### 🔍 Phase 1: Discovering the Routing Anomaly Through Iterative Debugging
Initially, the server was designed to borrow TCP's connection-oriented model for UDP: dynamically create a dedicated socket per client, `bind()` it to the service port, and `connect()` it to the client's address. The expectation was that **all initial datagrams would be consumed by `udp_accept()`**, with subsequent packets routing directly to the dedicated sockets.

Through repeated stress testing using a client that opens 1,024 sockets and sends exactly one datagram from each, a consistent pattern emerged in the server logs: **in a typical batch of 1,024 packets, approximately one initial datagram bypassed `udp_accept()` and triggered `read_data()` directly, showing a `dedicated_fd` of `-1`**. Depending on system scheduling and timing, this count occasionally varied between 0 and 5 per batch, and sometimes none occurred. At first, this appeared to be a logging error or a user-space race condition. By tracing the execution flow step-by-step and analyzing the log sequence, it became clear that this was not a code bug, but a direct result of the Linux kernel's asynchronous packet delivery mechanism operating independently of user-space syscalls.

### ⚙️ Phase 2: Understanding the Kernel's Async Dispatch
Further investigation into Linux networking semantics clarified the root cause: **the behavior is a natural consequence of how the kernel handles unconnected UDP sockets.**

Between the `bind()` and `connect()` syscalls, there is a microsecond-level window where the newly created socket exists in the kernel's unconnected (wildcard) lookup table. The kernel's packet delivery (driven by `softirq`/NAPI) runs asynchronously. If a datagram arrives at the NIC during this exact window, the kernel cannot yet match it to a connected 4-tuple. Instead, it routes the packet to a valid unconnected socket bound to the destination port.

Because kernel packet dispatch cannot be synchronized with user-space execution order, **this race window is an inherent characteristic of any model that dynamically creates and binds sockets per datagram.**

### 🔄 Phase 3: Shifting to a Session-Centric Architecture
Attempting to force synchronous routing against the kernel's async nature was impractical. The architecture was restructured from a rigid **`fd-centric`** model to a **`session-centric`** routing layer:
- **Distributed I/O paths are accepted**: Packets may arrive via `listenfd` or be routed by the kernel into a newly bound socket during the race window. The design accepts this dispersion to ensure no packets are lost.
- **Centralized business logic**: A global 4-tuple session table decouples file descriptors from client identity. Regardless of which fd delivers the datagram, routing and state management rely strictly on the real source address.
- **UDP `connect()` as a routing rule**: Rather than a network handshake, `connect()` is used here as a local kernel filter. It enables precise epoll wakeups, isolates client traffic, and prevents cross-client interference.

### 📈 Phase 4: Engineering Refinements & Stability Improvements
The codebase was hardened through continuous debugging and strict C programming practices:
- **Buffer safety**: `recvfrom` returns raw bytes without a null terminator. Using `%s` for printing caused garbage characters by reading into uninitialized stack memory. **Solution:** `memset(buf, 0, sizeof(buf))` is called immediately after buffer definition to clear residual data before any read or print operation.
- **Efficient event draining**: `EPOLLET` is paired with a `while(1)` loop that calls `recvfrom` until `EAGAIN`. This processes queued datagrams in a single event cycle and prevents epoll wakeup storms during traffic spikes.
- **Graceful degradation**: Instead of calling `exit(1)` when session tables or FD limits are exhausted, the server logs a warning and drops excess packets. This keeps the event loop alive and aligns with UDP's stateless design.
- **Architectural clarity**: Borrowing TCP's connected-socket concept was a deliberate choice to achieve precise per-client routing and high concurrency in UDP. By decoupling I/O fds from session state, the design retains UDP's low-latency, connectionless nature while gaining TCP-like routing precision and epoll isolation.

### 🌊 Phase 5: Evaluating Burst Traffic & Defining Boundaries
A practical question arose during testing: **How does this single-threaded design handle sudden traffic spikes?** Stress tests provided clear boundaries:
- **Moderate bursts** (< 20k new connections/sec, < 100k PPS) are handled stably. The `EPOLLET` drain loop prevents wakeup storms, session routing correctly absorbs race-window packets, and FD isolation prevents one client from blocking others.
- **High-intensity bursts** reveal single-threaded limits. Synchronous `socket()/bind()/connect()/epoll_ctl()` calls per packet, combined with `O(N)` linear session lookups, consume CPU cycles. The event loop slows down, kernel receive queues fill, and UDP's lack of backpressure results in expected packet drops.
- **Conclusion**: This is not a design flaw, but a clear performance boundary. The current implementation serves as a stable foundational pattern. Handling extreme bursts does not require rewriting the core logic; it requires standard evolutionary steps: `O(1)` hash routing, a reactor-worker thread model, batched syscalls, and kernel buffer tuning.

---

## 🏗️ Architecture & Core Design
```text
Client A/B/C ──UDP Datagrams──▶ NIC / Kernel Protocol Stack
                                      │
                          ┌───────────┴───────────┐
                          │  Async softirq/NAPI   │
                          │  Socket Lookup & Hash │
                          └───────────┬───────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              ▼                       ▼                       ▼
       [listenfd] (ET)        [Dedicated Socket A]     [Dedicated Socket B]
    (Wildcard/Unconnected)       (Connected/LT)           (Connected/LT)
              │                       │                       │
              └───────────┬───────────┴───────────────────────┘
                          ▼
               epoll_wait() Event Loop
                          │
              ┌───────────┴───────────┐
              ▼                       ▼
        udp_accept()            read_data()
    (Creates/Validates fd)   (Handles race/normal packets)
              │                       │
              └───────────┬───────────┘
                          ▼
               Session Routing Table (4-tuple)
              (Decouples I/O fd from Business Logic)
                          │
                          ▼
               handle_packet() ──▶ Unified Business Layer
```
### Key Design Principles
1. **"listenfd" uses "EPOLLET"**: Triggers only on "empty → non-empty" transitions. Paired with a "while(1)" drain loop to batch-process initial datagrams and minimize epoll wakeups under burst traffic.
2. **Dedicated sockets use "LT" (default)**: Ensures high fault tolerance for established sessions. If business logic blocks or delays, epoll continues to notify until the queue is drained.
3. **Session Table over FD Binding**: File descriptors are treated purely as I/O channels. Client identity and state are managed exclusively via "(src_ip, src_port)" lookup, making the system immune to kernel race-window misrouting.
4. **Graceful Degradation**: When session capacity or FD limits are reached, the server logs warnings and drops excess packets instead of crashing, aligning with UDP's stateless philosophy and enabling client-side backpressure.
5. **Burst Resilience via Decoupling**: By separating fast I/O consumption from heavy connection management, the architecture absorbs moderate spikes without state corruption or cascading failures.

---

## 📊 Burst Traffic Capability & Evolution Path

| Traffic Scenario | Current Architecture Behavior | Root Cause | Production Upgrade Path |
|:---|:---|:---|:---|
| **Moderate Burst** (< 20k conn/s, < 100k PPS) | ✅ Stable. ET drain prevents wakeup storms. Session routing absorbs race packets. Zero state corruption. | Single-threaded event loop + O(N) lookup still within CPU budget. | Ready for production as-is. |
| **High-Intensity Burst** (50k+ conn/s) | ⚠️ Event loop blocks → latency spikes → kernel "rmem" overflow → silent drops. | Synchronous "socket/bind/connect/epoll_ctl" per packet + O(N) linear scan exhausts CPU. | Replace with "uthash" (O(1)) + Reactor-Worker model + lockless queue. |
| **Malicious/Spoofed Burst** | 🛡️ Session table fills → fail-fast triggers → existing sessions survive, new packets dropped. | UDP lacks backpressure. No source-rate limiting. | Add token bucket per /24 subnet + XDP/eBPF pre-filtering. |
| **Sustained High PPS** (> 500k) | 📉 Throughput caps at single-core limit. Context switch & syscall overhead dominates. | User-space ↔ kernel boundary crossed per datagram. | Migrate to "io_uring" (async I/O) or "DPDK/XDP" (kernel bypass). |

**Design Boundary**: UDP concurrency isn't about "never dropping packets". It's about **processing efficiently, degrading observably, and never corrupting state**. This lab achieves the latter two natively; higher throughput is unlocked via the evolutionary path above.

---

📋 Debugging Observations & Practical Solutions

| Concept | Initial Expectation | Actual Behavior & Solution |
|:---|:---|:---|
| **UDP "connect()"** | Works like TCP, establishing a network connection | It is a local kernel rule. It sets the default peer address, enables 4-tuple filtering, and moves the socket to the connected hash table. No network packets are exchanged. |
| **"bind()" → "connect()" Window** | Should be instantaneous and atomic | There is a microsecond gap where the socket is bound but not yet connected. The kernel's async packet delivery can route incoming datagrams to this socket during this window. This is normal and unavoidable in dynamic socket creation. |
| **"recvfrom" & Buffer Printing** | Returns a null-terminated C string | Returns raw bytes without appending \0. Printing with %s reads past the valid data into uninitialized stack memory, causing garbage characters. Solution: Call memset(buf, 0, sizeof(buf)) immediately after defining the buffer to clear residual data before printing. |
| **"EPOLLET" on UDP** | Might miss packets if not handled carefully | Works reliably when paired with a while(1) loop that calls recvfrom until it returns EAGAIN. This drains the queue completely in one event cycle and prevents epoll from triggering repeatedly. |
| **FD Limit vs Session Limit** | "MAX_SESSIONS" should equal "ulimit -n" | They serve different purposes. The session limit manages business state; the FD limit is an OS resource boundary. The application should check its own limits and degrade gracefully before the OS rejects new sockets with EMFILE. |
| **Handling Burst Traffic** | A single-threaded epoll loop can handle all loads | Synchronous socket creation and linear table lookups per packet become performance bottlenecks under high concurrency. Finding: The current design handles moderate bursts stably. For higher loads, the practical next step is to decouple I/O from business processing and replace linear lookups with hash tables. |

---

## 📂 Project Structure
```text
udp-concurrency-lab/
├── .vscode/
│   ├── c_cpp_properties.json  # C/C++ IntelliSense, compiler path & include configuration
│   ├── settings.json          # Workspace-specific editor & formatting settings
│   └── tasks.json             # Build, run & debug task automation (replaces traditional Makefile)
├── udp_server.c               # High-concurrency UDP server with session routing & race-window handling
├── udp_client.c               # Multi-port concurrent UDP client for stress testing
└── README.md                  # This file
```

## 🛠️ Build & Run

### ⚙️ Pre-run Configuration
Before compiling and running the client, you must set the correct target server IP. Run `ip addr` on your server machine to find its IP address, then open `udp_client.c` and update the server address definition in the source code accordingly.

### 📦 Compile & Run (VS Code Workflow)
This project uses `.vscode/tasks.json` for build automation instead of a traditional Makefile.
- **Build**: Press `Ctrl+Shift+B` (or `Cmd+Shift+B` on macOS) to trigger the default build task defined in `tasks.json`.
- **Run Server**: Open the integrated terminal and execute `./udp_server`.
- **Run Client**: Open a second terminal and execute `./udp_client` to start concurrent stress testing.

### 🔧 Manual Compilation (Alternative)
If you prefer to compile directly from the command line:
```bash
gcc udp_server.c -o udp_server
gcc udp_client.c -o udp_client
