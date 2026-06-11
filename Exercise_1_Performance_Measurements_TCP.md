# Exercise #1: TCP Throughput Benchmark — Report

## Authors
- Student ID(s): `<id1>_<id2>`

## 1. Implementation Overview

The benchmark consists of two standalone executables: `server` and `client`. The client connects to the server, iterates over 21 message sizes (1 B to 1 MB in powers of two), and for each size sends a warmup batch followed by a timed batch. The server drains all bytes and acknowledges each batch. Throughput is calculated on the client as `(msg_size × msg_count × 8) / elapsed_seconds`, with units auto-scaled (bps, Kbps, Mbps, Gbps).

### Architecture

Both executables are **self-contained single-file programs** — all helper functions, constants, and protocol definitions are inlined. This avoids cross-translation-unit linkage overhead and ensures the compiler can fully inline the hot-path code. The convergence-detector and warmup-probe tools (used to calibrate `MSG_COUNTS` and `WARMUP_COUNTS`) live in their own subdirectories with their own Makefiles.

### File Structure

| File | Purpose |
|------|---------|
| `server.cpp` | Listens, accepts one connection, drains warmup+timed bytes in batched gulps, ACKs |
| `client.cpp` | Connects, sends warmup+timed batches per size, measures elapsed time, prints results |
| `Makefile` | Builds server and client with `g++ -std=c++17 -Wall -Wextra -O3` |
| `convergence_detector/` | Finds optimal `MSG_COUNTS` per size (variance < 1% between doubled counts) |
| `warmup_probe/` | Finds optimal `WARMUP_COUNTS` per size (variance < 1% between doubled warmup levels) |
| `Exercise_1_Performance_Measurements_TCP.md` | This report |

## 2. Methodology

### 2.1 Message Counts — Convergence Detector

The number of timed messages per size (`MSG_COUNTS[]`) was determined by the **convergence detector** tool. For each message size, it starts with a small count and doubles it until the measured throughput variance between consecutive doubled counts falls below 1%. This finds the minimum count that produces stable measurements:

- Small messages (1–16 B): require large counts (100K–1.3M) because per-syscall overhead dominates — many iterations are needed for stable timing.
- Large messages (32 KB–1 MB): require few counts (80–160) because data transfer time dwarfs syscall cost — fewer iterations suffice.
- The total runtime across all 21 sizes is ~3–5 seconds.

### 2.2 Warmup Counts — Warmup Probe

The number of warmup messages per size (`WARMUP_COUNTS[]`) was determined by the **warmup probe** tool. For each message size, it starts with 2 warmup messages and doubles until throughput variance between consecutive warmup levels falls below 1%. This finds the minimum warmup needed to fully open the TCP congestion window:

- 1–16 B: need 4–32 warmup messages (very small warmup data volume).
- 32 B–1 MB: all converged at 4 warmup messages — the TCP window opens quickly for these sizes.

### 2.3 Protocol

For each message size, the protocol executes:

```
CLIENT                                  SERVER
  │                                       │
  │── batched warmup send ────────────────│  drained in chunks (discarded)
  │   (WARMUP_COUNTS[i] × size bytes)     │
  │                                       │
  │   start = now()                       │
  │                                       │
  │── per-message timed send ─────────────│  batched recv in 1 MB chunks
  │   (MSG_COUNTS[i] msgs, size bytes)    │  (Opt A — drains kernel buffer fast)
  │                                       │
  │                        ACK (8 bytes) ←│  send(&ack=0)
  │                                       │
  │   end = now()                         │
  │   throughput = bytes×8 / elapsed      │
```

**Key design choices:**
- The server uses **batched recv** (1 MB chunks) for both warmup and timed phases — it never inspects individual message boundaries, only total byte counts.
- The client uses **batched send** for warmup (to saturate the window quickly) and **per-message send** for the timed batch (matching the exercise's per-message measurement model).
- The ACK serves purely as a synchronization barrier — the client waits for it before stopping the timer.

## 3. Optimizations Applied

### Buffered Batched I/O (Opt A)

The server drains all timed bytes in large 1 MB chunks rather than per-message `recv()`. This reduces the server's syscall count from `MSG_COUNTS[i]` (up to 1.3M for 1B messages) down to 1–2 per size. Since the server's receive work is inside the client's timer window, every syscall saved directly improves measured throughput.

### Single Buffer Reuse (Opt E)

One 1 MB buffer is allocated once and reused for all 21 iterations, eliminating heap allocation overhead between sizes. The buffer is sized to the largest single message (1 MB), and batched I/O stays within this ceiling: receive chunks are capped at 1 MB.

### Large Socket Buffers (Opt C)

Both client and server set `SO_SNDBUF` and `SO_RCVBUF` to 16 MB. This prevents the client's `send()` from blocking due to a full kernel buffer during the timed batch, especially for large messages where a 208 KB default buffer fills in a few sends.

### TCP_NODELAY

Disabled Nagle's algorithm on both sides to prevent the kernel from delaying small sends (up to 200 ms coalescing). This is critical for small-message measurements where Nagle would dominate the timing.

### Inline Helpers

`send_all()` and `recv_all()` are `static inline` in each file, letting the compiler inline them into the hot loops with `-O3`. No function-call overhead across translation units.

## 4. Performance Results

Results below are from a **localhost loopback** test (WSL2 Ubuntu on Windows 11, g++ 13.3.0, `-O3`):

| Message Size | Throughput | Unit |
|-------------:|-----------:|------|
| 1 | 2.48 | Mbps |
| 2 | 4.78 | Mbps |
| 4 | 9.22 | Mbps |
| 8 | 18.79 | Mbps |
| 16 | 37.06 | Mbps |
| 32 | 70.21 | Mbps |
| 64 | 140.46 | Mbps |
| 128 | 288.02 | Mbps |
| 256 | 591.70 | Mbps |
| 512 | 1.16 | Gbps |
| 1024 | 2.13 | Gbps |
| 2048 | 4.29 | Gbps |
| 4096 | 10.61 | Gbps |
| 8192 | 30.45 | Gbps |
| 16384 | 47.43 | Gbps |
| 32768 | 37.16 | Gbps |
| 65536 | 34.20 | Gbps |
| 131072 | 22.62 | Gbps |
| 262144 | 25.61 | Gbps |
| 524288 | 27.89 | Gbps |
| 1048576 | 27.89 | Gbps |

**Observations:**
- Throughput scales with message size for small messages (1 B–16 KB): larger messages mean fewer syscalls per byte transferred.
- Throughput peaks at ~16 KB (47 Gbps) and plateaus around 20–40 Gbps for larger sizes — this is the WSL2 virtual network interface's practical ceiling on loopback.
- Real hardware between two physical lab machines is expected to show lower peak throughput (limited by NIC speed, typically 1–10 Gbps) but the same scaling pattern.

## 5. Build & Usage

```bash
make all              # builds server and client
```

**Server:**
```bash
./server
```

**Client:**
```bash
./client <server-ip>
```

Output format: three tab-delimited columns: `<message-size>\t<throughput>\t<unit>`

## 6. Compile-Time Guarantees

- Both `server.cpp` and `client.cpp` contain identical copies of `MSG_COUNTS[]` and `WARMUP_COUNTS[]` (standalone design). Any mismatch between them would manifest as a protocol desynchronization — the server would expect different byte counts than the client sends.
- `generate_sizes()` produces exactly 21 sizes (2^0 through 2^20), matching the array lengths.
- Compilation with `-Wall -Wextra` at `-O3` produces zero warnings.
