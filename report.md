# Exercise 1 — TCP Throughput Benchmark: Report

## Implementation Overview

The benchmark consists of a client and a server sharing a common library. The client connects to the server via TCP and measures unidirectional throughput for message sizes ranging from 1 byte to 1 MB (powers of two, 21 sizes total).

**Files:**
- `common.h` / `common.cpp` — shared declarations and utilities: socket creation, reliable send/receive wrappers, throughput computation, and message-size generation.
- `server.cpp` — listens on a hardcoded port, receives batches of messages from the client, and acknowledges each batch.
- `client.cpp` — connects to the server, sends warm-up and timed messages for each size, measures elapsed time, and prints throughput results.

The server takes no arguments (port 12345). The client takes the server IP as its only argument.

## Design Decisions

### TCP_NODELAY
Nagle's algorithm is disabled on the client socket (`TCP_NODELAY`). Without this, the kernel would coalesce small outgoing packets, introducing artificial delays and distorting throughput measurements for small message sizes.

### Warm-Up Phase
Before each timed batch, 100 warm-up messages of the same size are sent (untimed). This saturates the TCP slow-start window, ensuring the timed measurements reflect steady-state throughput rather than the initial ramp-up. The server receives and discards these warm-up messages, keeping the protocol synchronized.

### Throughput Measurement
The timer starts just before the timed batch begins and stops after the server's acknowledgment is received. This captures the full send + receive round-trip, measuring the network rather than just the client's send rate. Throughput is computed in bits per second and auto-scaled to the most readable unit (bps, Kbps, Mbps, or Gbps).

### Message Count Selection
The number of messages per size (`MSG_COUNTS[]`) decreases as message size grows — from 100,000 messages for tiny sizes (1–64 B) down to 100 messages for 1 MB. This keeps the total benchmark runtime under 30 seconds while providing enough iterations for stable timing at small sizes (where per-system-call overhead dominates).

The exact values were determined using a convergence detector (included but commented out in `client.cpp`). For each message size, it starts with a small batch and repeatedly doubles until the measured throughput variance between iterations falls below 1%.

### Reliable I/O
`sent_full()` and `recv_full()` wrap the standard `send()`/`recv()` calls, looping until all requested bytes are transferred. They handle partial operations and `EINTR` interruptions correctly.

## Performance Results

Test environment: Linux loopback (127.0.0.1), single machine.

| Message Size (bytes) | Throughput | Unit |
|---------------------|------------|------|
| 1 | 8.79 | Mbps |
| 2 | 6.31 | Mbps |
| 4 | 11.46 | Mbps |
| 8 | 24.20 | Mbps |
| 16 | 38.20 | Mbps |
| 32 | 104.09 | Mbps |
| 64 | 159.12 | Mbps |
| 128 | 327.97 | Mbps |
| 256 | 662.10 | Mbps |
| 512 | 1.29 | Gbps |
| 1024 | 2.49 | Gbps |
| 2048 | 4.85 | Gbps |
| 4096 | 13.02 | Gbps |
| 8192 | 26.95 | Gbps |
| 16384 | 41.08 | Gbps |
| 32768 | 61.58 | Gbps |
| 65536 | 57.77 | Gbps |
| 131072 | 68.91 | Gbps |
| 262144 | 90.65 | Gbps |
| 524288 | 70.42 | Gbps |
| 1048576 | 78.96 | Gbps |

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
