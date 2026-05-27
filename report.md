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

Test environment: Linux loopback (127.0.0.1), single machine.

| Message Size (bytes) | Throughput | Unit |
|---------------------|------------|------|
| 1 | 9.20 | Mbps |
| 2 | 11.08 | Mbps |
| 4 | 18.69 | Mbps |
| 8 | 39.43 | Mbps |
| 16 | 76.25 | Mbps |
| 32 | ~100 | Mbps |
| 64 | ~170 | Mbps |
| 128 | ~300 | Mbps |
| 256 | ~680 | Mbps |
| 512 | ~1.3 | Gbps |
| 1024 | ~2.5 | Gbps |
| 2048 | ~5 | Gbps |
| 4096 | ~13 | Gbps |
| 8192 | ~25 | Gbps |
| 16384 | ~40 | Gbps |
| 32768 | ~60 | Gbps |
| 65536 | ~60 | Gbps |
| 131072 | ~70 | Gbps |
| 262144 | ~90 | Gbps |
| 524288 | ~70 | Gbps |
| 1048576 | ~80 | Gbps |

*Note: These are approximate loopback results for illustration. Replace with actual results from lab machines before submission.*

### Analysis

**Small messages (1–256 B):** Throughput is low (Mbps range) because per-system-call overhead dominates. Each `send()`/`recv()` call has a fixed cost, and with tiny payloads the ratio of overhead to data is unfavorable.

**Medium messages (512 B – 8 KB):** Throughput climbs into the Gbps range as message size increases. The per-call overhead is amortized over larger payloads, and the TCP congestion window opens fully.

**Large messages (16 KB – 1 MB):** Throughput plateaus in the 60–90 Gbps range (loopback limit). At these sizes the bottleneck shifts from per-message overhead to memory bandwidth and kernel buffer limits.

The loopback results show the upper bound of what the hardware can achieve; results between two physical machines will be lower due to actual network latency and bandwidth constraints.

## Building and Running

```bash
make all        # builds server and client
make test       # builds and runs unit tests
./server        # start server (port 12345)
./client <ip>   # run client against server
```
