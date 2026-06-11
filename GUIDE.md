# Code Walkthrough & Design Rationale

This guide explains every section of the TCP throughput benchmark — line by line, decision by decision. Use this to prepare for code interviews.

---

## Architecture

The benchmark consists of two standalone C++ files: `server.cpp` and `client.cpp`. There is no shared library — both files are fully self-contained with inline helpers and duplicate constant arrays. This design ensures:

1. **Maximum inlining**: `-O3` can inline all helpers directly into the hot loops with no cross-TU (translation unit) linkage overhead.
2. **No ODR risk**: The `MSG_COUNTS[]` and `WARMUP_COUNTS[]` arrays exist in both files. While this means they could theoretically diverge, it eliminates the complexity of shared headers and link-time dependencies.
3. **Single-command compilation**: Each file compiles with a single `g++` invocation — no `.o` files or link step needed.

---

## Makefile

```makefile
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3
```

**Why g++?** The university lab machines run Linux with GCC. No cross-platform concerns.

**Why C++17?** We use `inline` variables (for `MSG_COUNTS` and `WARMUP_COUNTS` in each file) and `constexpr` for compile-time constants.

**Why `-Wall -Wextra`?** Zero warnings is a hard requirement — catches implicit conversions, unused variables, and missing returns at compile time.

**Why `-O3`?** A throughput benchmark must run at maximum speed. We're measuring the network, not the compiler's ability to generate safe debug code.

**Why no `.o` files?** Each executable is compiled directly from its `.cpp` file — no shared object, no separate link step. This is correct because the files are standalone (no `common.h` dependency).

---

## server.cpp

### Shared definitions

```cpp
std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++)
        sizes.push_back(static_cast<size_t>(1ULL << i));
    return sizes;
}
```

**Why `1ULL`?** The `ULL` suffix guarantees 64-bit unsigned arithmetic for the shift. Without it, `1 << 20` would be an `int` (32-bit signed), and `1 << 31` would overflow into the sign bit — undefined behavior.

**Why generate programmatically?** The series is mathematically defined (2^0 through 2^20). Generating it is self-documenting and can't have transcription errors.

### WARMUP_COUNTS

```cpp
inline const size_t WARMUP_COUNTS[] = {
    16, 4,  4,  32, 4,   // 1B  2B  4B  8B  16B
    4,  4,  4,  4,  4,   // 32B 64B 128B 256B 512B
    4,  4,  4,  4,  4,   // 1KB 2KB 4KB 8KB 16KB
    4,  4,  4,  4,  4,   // 32KB 64KB 128KB 256KB 512KB
    4                      // 1MB
};
```

**How were these determined?** The `warmup_probe/` tool measures throughput at increasing warmup counts for each message size. When the variance between consecutive doubled warmup levels falls below 1%, the warmup count is considered converged — the TCP congestion window is fully open.

**Why per-size warmup counts?** Small messages (1–16 B) need more warmup messages (16–32) because individual syscalls are cheap but the window opens slowly. All sizes from 32 B upward converged at just 4 warmup messages.

### MSG_COUNTS

```cpp
inline const size_t MSG_COUNTS[] = {
    1310720, 81920,  655360, 163840, 327680, // 1B  2B  4B  8B  16B
    20480,   81920,  81920,  40960,  20480,   // 32B 64B 128B 256B 512B
    20480,   20480,  20480,  2560,   2560,    // 1KB 2KB 4KB 8KB 16KB
    2560,    640,    320,    160,    160,     // 32KB 64KB 128KB 256KB 512KB
    80                                          // 1MB
};
```

**How were these determined?** The `convergence_detector/` tool doubles the message count until throughput variance between consecutive counts falls below 1%. This finds the minimum count needed for stable measurements.

**Why do counts decrease as message size increases?** Smaller messages have higher per-syscall overhead. For 1-byte messages, 1.3M iterations are needed to get a stable timing measurement. For 1 MB messages, 80 iterations suffice — the data transfer time dominates.

### recv_all() — the core helper

```cpp
static inline ssize_t recv_all(int fd, void* buf, size_t n) {
    size_t total = 0;
    char*  ptr   = static_cast<char*>(buf);
    while (total < n) {
        ssize_t r = recv(fd, ptr + total, n - total, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;   // EOF — connection closed
        total += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(total);
}
```

**Why `static inline`?** Allows the compiler to inline this into call sites. The function is small and called in hot loops — inlining eliminates function-call overhead.

**Why handle `EINTR`?** POSIX signals can interrupt blocking system calls. On `EINTR`, we `continue` to retry — no data was lost.

**Why treat `r == 0` as an error?** `recv()` returning 0 means the peer closed the connection. Our protocol expects exactly `n` bytes, so a premature close is an error.

### Main loop — batched warmup recv

```cpp
// Batched warmup recv — drain all warmup messages in a single pass
size_t remaining = WARMUP_COUNTS[i] * size;
while (remaining > 0) {
    size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
    if (recv_all(client_fd, buf, chunk) < 0) {
        perror("warmup recv");
        // ... cleanup
    }
    remaining -= chunk;
}
```

**Why batched instead of per-message?** The server doesn't need message boundaries — it only needs to drain `WARMUP_COUNTS[i] × size` bytes from the TCP stream before the timed batch begins. Receiving in 1 MB chunks reduces syscall count from potentially thousands down to 1–2 per size. This is "Opt A" from the optimization analysis.

### Main loop — batched timed recv

```cpp
remaining = count * size;
while (remaining > 0) {
    size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
    if (recv_all(client_fd, buf, chunk) < 0) { ... }
    remaining -= chunk;
}
```

**Why is this the critical optimization?** The client's timer runs until the ACK arrives. Every cycle the server spends in `recv()` is on the clock. Per-message `recv()` for 1-byte messages would mean 1.3M syscalls — batched recv reduces this to ~1–2 `recv()` calls that drain the kernel buffer in one gulp.

**Why 1 MB chunks?** The buffer is 1 MB. For sizes where `count × size` exceeds 1 MB, we loop. But on localhost the kernel buffer is already full when we call `recv()`, so the first call drains everything. The loop handles edge cases.

### TCP_NODELAY and SO_RCVBUF

```cpp
int nodelay = 1;
setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
int rcvbuf = 16 * 1024 * 1024;
setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
```

**Why TCP_NODELAY on the accepted socket?** The server sends 8-byte ACK messages. Without `TCP_NODELAY`, Nagle's algorithm could delay these ACKs by up to 200 ms — inflating the client's measured time.

**Why SO_RCVBUF = 16 MB?** The kernel's default receive buffer (~208 KB) fills quickly for large messages. A 16 MB buffer ensures the kernel can accept all timed bytes without the client's `send()` blocking — preventing backpressure from inflating the measurement.

### Single buffer allocation

```cpp
const size_t BUF_SZ = 1ULL << 20;
char* buf = new char[BUF_SZ];
```

**Why one buffer for all iterations?** Allocating per-size would add heap overhead between iterations. A single 1 MB buffer (the size of the largest message) is reused for all sizes. For batched I/O, chunk sizes are capped at `BUF_SZ`, so the buffer is never overflowed.

### ACK

```cpp
uint64_t ack = 0;
if (send(client_fd, &ack, sizeof(ack), MSG_NOSIGNAL) < 0) {
    perror("send ack");
    // ... cleanup
}
```

**Why send zero?** The ACK value is never validated — it exists purely as a synchronization barrier. Sending a constant zero keeps the protocol structure without unnecessary arithmetic.

**Why `MSG_NOSIGNAL`?** Prevents `SIGPIPE` from killing the process if the client disconnects. On `SIGPIPE`, the process dies with exit code 141 — bypassing all error handling.

---

## client.cpp

### Shared definitions

The same `generate_sizes()`, `WARMUP_COUNTS[]`, and `MSG_COUNTS[]` arrays are duplicated in `client.cpp`. This is intentional: both files are standalone and self-consistent.

### compute_throughput()

```cpp
ThroughputResult compute_throughput(size_t msg_size, size_t msg_count,
                                    double elapsed_sec) {
    if (elapsed_sec <= 0.0) return {0.0, "bps"};
    double bits_per_sec =
        (static_cast<double>(msg_size) * msg_count * 8.0) / elapsed_sec;

    if      (bits_per_sec < 1000.0)        return {bits_per_sec, "bps"};
    else if (bits_per_sec < 1000000.0)     return {bits_per_sec / 1000.0, "Kbps"};
    else if (bits_per_sec < 1000000000.0)  return {bits_per_sec / 1000000.0, "Mbps"};
    else                                   return {bits_per_sec / 1000000000.0, "Gbps"};
}
```

**Why the zero guard?** In virtualized environments, `high_resolution_clock` can have coarse granularity. If a batch completes faster than the clock tick, `end - start` is 0.0. Dividing by zero produces `inf`. Returning 0 bps is a safe sentinel.

**Why cast to double before multiplication?** `msg_size * msg_count` is a `size_t` multiplication that can overflow. Casting to double forces floating-point arithmetic for the entire expression.

**Why multiply by 8?** Throughput is measured in bits per second (networking convention), but data is counted in bytes.

**Why SI prefixes (1000, not 1024)?** Networking uses decimal prefixes: 1 Kbps = 1000 bps, 1 Mbps = 1,000,000 bps.

### send_all() and recv_all()

Identical to the server's `recv_all()`, with the same `EINTR` handling and `MSG_NOSIGNAL` flag. `send_all()` loops until all bytes are sent; `recv_all()` loops until all bytes are received.

### Socket setup

```cpp
int flag = 1;
setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

int sndbuf = 16 * 1024 * 1024;
int rcvbuf = 16 * 1024 * 1024;
setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
```

**Why both SO_SNDBUF and SO_RCVBUF?** Large send buffer prevents `send()` from blocking during the timed batch. Large receive buffer prevents the ACK from being delayed by a full kernel buffer. Both are set to 16 MB.

### Warmup send — batched

```cpp
// Batched warmup send — saturate the TCP window in one pass
size_t remaining = WARMUP_COUNTS[i] * size;
while (remaining > 0) {
    size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
    if (send_all(client_fd, buf, chunk) < 0) { ... }
    remaining -= chunk;
}
```

**Why batched for warmup?** The goal is to saturate the TCP window as fast as possible — batched send pumps data into the kernel buffer at maximum rate.

### Timed send — per-message

```cpp
auto start = std::chrono::high_resolution_clock::now();

for (size_t j = 0; j < count; j++) {
    size_t total = 0;
    while (total < size) {
        ssize_t s = send(client_fd, buf + total, size - total, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            perror("send");
            // ... cleanup
        }
        total += static_cast<size_t>(s);
    }
}

uint64_t ack = 0;
if (recv_all(client_fd, &ack, sizeof(ack)) < 0) { ... }

auto end = std::chrono::high_resolution_clock::now();
```

**Why per-message send for the timed batch?** This matches the exercise's conceptual model: measuring the cost of sending individual messages of a given size. The inline send loop (no external function call) minimizes overhead while preserving the per-message structure.

**Why does the timer include the ACK wait?** This is intentional — it measures the network round-trip, not just the client's local send rate. The server only ACKs after receiving all timed bytes, so the timer captures the full send + network + server processing + ACK return path.

**Why `high_resolution_clock`?** Provides nanosecond resolution on modern Linux. For small messages, the timed batch can complete in microseconds — we need precision.

### Output format

```cpp
printf("%zu\t%.2f\t%s\n", size, result.value, result.unit.c_str());
```

**Why `%zu` for size?** Correct format specifier for `size_t` on all platforms.

**Why `\t` delimiters?** The exercise spec requires exactly three tab-delimited columns for the auto-tester script.

**Why `%.2f`?** Two decimal places is sufficient precision — more digits would imply false precision in inherently noisy network measurements.

---

## Protocol Summary

```
For each message size (1B ... 1MB):

  CLIENT                                  SERVER
    │                                       │
    │── batched warmup send ────────────────│  batched recv (discarded)
    │   (WARMUP_COUNTS[i] × size bytes)     │
    │                                       │
    │   start = now()                       │
    │                                       │
    │── per-message timed send ─────────────│  batched recv in 1MB chunks
    │   (MSG_COUNTS[i] msgs × size bytes)   │  (Opt A — minimal syscalls)
    │                                       │
    │                        ACK (8B) ←─────│  send(&ack=0)
    │                                       │
    │   end = now()                         │
    │   throughput = bits / elapsed         │
```

**Why this ordering prevents measurement error:**
- Warmup is fully drained before the timer starts — slow-start ramp-up is excluded.
- The server's batched timed recv minimizes syscall overhead inside the timer window.
- The ACK provides a precise batch delimiter — the client knows exactly when all timed bytes were received.

---

## Design Decisions Summary

| Decision | Rationale |
|----------|-----------|
| Standalone files (no common.h) | Maximum inlining, no link-time dependencies |
| Batched recv on server (1 MB chunks) | Reduces server syscalls by up to 1.3M per size (Opt A) |
| Per-message send on client (timed) | Matches exercise's per-message measurement model |
| Batched send on client (warmup) | Saturates TCP window as fast as possible |
| Single 1 MB buffer, all iterations | Zero heap allocation between sizes |
| SO_SNDBUF / SO_RCVBUF = 16 MB | Prevents send-side blocking during timed batch (Opt C) |
| TCP_NODELAY on both sides | Prevents Nagle from delaying data and ACKs |
| MSG_NOSIGNAL on all send() | Prevents SIGPIPE from killing the process |
| Timer spans send + ACK wait | Measures network round-trip, not just local send rate |
| ACK value = 0 (constant) | Value never validated — computing it is wasted work |
| WARMUP_COUNTS from warmup_probe | Empirically determined per-size warmup (variance < 1%) |
| MSG_COUNTS from convergence_detector | Empirically determined per-size message counts (variance < 1%) |
| C++17, -O3, -Wall -Wextra | Modern C++, max optimization, zero warnings |
