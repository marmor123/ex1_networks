# Exercise 1 — TCP Throughput Benchmark: Report

## Implementation Overview

The benchmark consists of a client and a server sharing a common library. The client connects to the server via TCP and measures unidirectional throughput for message sizes ranging from 1 byte to 1 MB (powers of two, 21 sizes total).

**Files:**
- `common.h` / `common.cpp` — shared declarations and utilities: socket creation, reliable send/receive wrappers, throughput computation, and message-size generation. `MSG_COUNTS[]` is defined here as a single source of truth for both executables.
- `server.cpp` — listens on a hardcoded port, receives warm-up and timed messages for each size in separate loops, and acknowledges each timed batch.
- `client.cpp` — connects to the server, sends warm-up and timed messages for each size, measures elapsed time with high-resolution clock, and prints throughput results.

The server takes no arguments (port 12345). The client takes the server IP as its only argument.

## Design Decisions

### TCP_NODELAY
Nagle's algorithm is disabled on both the client socket and the server's accepted socket. Without this, the kernel would delay small sends (up to 200ms) to coalesce segments — distorting throughput for small messages and introducing ACK latency. Both sides of the connection use `TCP_NODELAY`.

### Warm-Up Phase
Before each timed batch, 100 warm-up messages of the same size are sent (untimed). This saturates the TCP slow-start window per message size — the lecturer noted there may be TCP dynamics beyond simple congestion-window behavior that benefit from per-size warm-up.

The server receives warm-up and timed messages in **separate loops**, discarding the warm-up data and only acknowledging the timed batch. This prevents warm-up transmission time from contaminating the throughput measurement: the client's timer starts after warm-up send and stops after the ACK, which is only sent once the timed batch is fully received.

### Throughput Measurement
The timer starts before the timed batch and stops after receiving the server's acknowledgment. This captures the full send + server receive + ACK return path, measuring the network round-trip rather than just the local send rate. Throughput is computed in bits per second and auto-scaled to the most readable unit (bps, Kbps, Mbps, or Gbps).

### Message Count Selection
The number of messages per size (`MSG_COUNTS[]`) decreases as message size grows — from 100,000 messages for tiny sizes (1–64 B) down to 100 messages for 1 MB. This keeps total benchmark runtime under 30 seconds.

The exact values were determined using a convergence detector (included but commented out in `client.cpp`). For each message size, it starts with a small count and repeatedly doubles until the measured throughput variance between consecutive counts falls below 1%. The variance is measured between different counts (e.g., 100 vs 200), not between repeated runs of the same count — the goal is to find the minimum count where increasing further does not meaningfully change throughput.

### Reliable I/O & Signal Safety
`sent_full()` and `recv_full()` wrap the standard `send()`/`recv()` calls, looping until all requested bytes are transferred. They handle partial operations and `EINTR` interruptions correctly. `send_full()` uses `MSG_NOSIGNAL` to prevent `SIGPIPE` from killing the process if the peer disconnects.

### ACK Protocol
The server sends a single fixed-size acknowledgment after receiving all timed messages for a batch. The ACK serves purely as a synchronization barrier — the client blocks until it arrives, then stops the timer and proceeds to the next size. The ACK value is not used.

### Compiler Optimizations
Compiled with `-O3` and `-Wall -Wextra` (zero warnings). Maximum optimization is appropriate for a benchmark whose goal is measuring the network, not the compiler's debug instrumentation.

## Performance Results

Test environment: Linux mlx-stud-03 and mlx-stud-04.

| Message Size (bytes) | Throughput | Unit |
|---------------------|------------|------|
| 1 | 13.00 | Mbps |
| 2 | 24.83 | Mbps |
| 4 | 51.74 | Mbps |
| 8 | 102.69 | Mbps |
| 16 | 214.76 | Mbps |
| 32 | 411.92 | Mbps |
| 64 | 868.12 | Mbps |
| 128 | 936.78 | Mbps |
| 256 | 938.69 | Mbps |
| 512 | 937.24 | Mbps |
| 1024 | 939.39 | Mbps |
| 2048 | 940.09 | Mbps |
| 4096 | 940.58 | Mbps |
| 8192 | 938.67 | Mbps |
| 16384 | 939.01 | Mbps |
| 32768 | 939.51 | Mbps |
| 65536 | 934.99 | Mbps |
| 131072 | 933.93 | Mbps |
| 262144 |931.50 | Mbps |
| 524288 | 937.61 | Mbps |
| 1048576 | 938.09 | Mbps |

### Analysis

**Small messages (1–64 B):** Throughput climbs from 13 Mbps to 868 Mbps as message size increases. Per-system-call overhead dominates at the smallest sizes — each `send()`/`recv()` call has a fixed cost, and with tiny payloads the ratio of overhead to data is unfavorable. Throughput roughly doubles with each doubling of payload size, which is the signature of a per-packet fixed-cost bottleneck.

**Medium messages (128 B – 8 KB):** Throughput reaches the ~940 Mbps plateau. The per-call overhead is amortized over larger payloads, and the warm-up phase has already opened the TCP congestion window before timing begins.

**Large messages (16 KB – 1 MB):** Throughput holds steady at ~940 Mbps — the 1 Gbps Ethernet link limit. At these sizes, per-message overhead is negligible and the NIC wire speed is the bottleneck.

The ~940 Mbps ceiling is consistent with the expected throughput of a 1 Gbps Ethernet link between the two lab machines after accounting for TCP/IP and Ethernet framing overhead.

## Building and Running

```bash
make all        # builds server and client
make test       # builds and runs unit tests
./server        # start server (port 12345)
./client <ip>   # run client against server
```
