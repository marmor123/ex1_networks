# Optimization Opportunities — Timed Measurement Path

This document analyzes possible performance gains **strictly inside the timer window**
(`start` → `end` in `client.cpp`). Everything outside this window is irrelevant to the
throughput number.

## What the timer actually measures

```
CLIENT                                  SERVER
───────────────────────────────────     ───────────────────────────────────
auto start = now();

for j in 0..count:
    send_full(fd, buf, size) ──TCP──►   for j in 0..count:
                                            recv_full(client_fd, buf, size)
                                                ↑ N recv() syscalls

recv_full(fd, &ack, 8)      ◄──TCP──   send_full(client_fd, &ack, 8)

auto end = now();
```

The clock runs until the ACK arrives. That means **the server's timed-recv work is
directly on the clock** — every cycle the server spends in `recv_full()` inflates the
client's measured time and lowers the reported throughput.

---

## Optimization A: Batched timed-recv on the server (high impact)

### Current code (`server.cpp:77-85`)

```cpp
// Receive timed messages — one recv_full() call per message
for (size_t j = 0; j < count; j++) {
    if (recv_full(client_fd, buf, size) < 0) {
        perror("recv");
        delete[] buf;
        close(client_fd);
        close(listen_fd);
        return 1;
    }
}
```

For a 1-byte message size this is **100,000 `recv()` syscalls** inside the timer window.
Each syscall costs ~1–3 μs on localhost, ~10–50 μs on real hardware.

### Why per-message recv is unnecessary

The server doesn't need message boundaries. It never inspects the data nor counts
individual messages — it only needs to know when *all* timed bytes for this batch
have arrived. TCP is a byte stream: receiving `count × size` bytes in one gulp is
functionally identical to receiving `size` bytes `count` times.

### Proposed change

```cpp
// Drain all timed bytes in one kernel-managed call.
// MSG_WAITALL tells the kernel: "don't return until n bytes arrive or error."
// The loop handles the rare case where MSG_WAITALL returns early (signal).
size_t remaining = count * size;
while (remaining > 0) {
    ssize_t n = recv(client_fd, buf, remaining, MSG_WAITALL);
    remaining -= static_cast<size_t>(n);
}
```

Requires a buffer large enough for `count × size` bytes (or a smaller buffer with
multiple drain iterations — still far fewer syscalls than per-message).

### Expected impact

| Message size | Before (syscalls) | After (syscalls) | Reduction |
|-------------|-------------------|-------------------|-----------|
| 1 B         | 100,000           | ~1                | 99,999×   |
| 1 KB        | 20,000            | ~1                | 20,000×   |
| 1 MB        | 100               | ~1                | 100×      |

The effect is largest for small messages where per-syscall overhead dominates the
throughput calculation. For large messages (1 MB), the 100→1 reduction matters less
because the data transfer time dwarfs the syscall cost.

### Risks

- **Buffer size:** the current per-iteration `new char[size]` allocates only enough
  for one message. A batched recv needs a larger buffer (either `count × size` allocated
  per iteration, or one max-size allocation reused across all iterations).
- **Correctness:** batched recv is safe because TCP preserves byte ordering and the
  warmup phase is fully drained before the timed recv begins (separate server loops
  guarantee this).
- **Error handling:** the current code checks every `recv_full()` return value. The
  batched version above doesn't check for errors (`n < 0`). A production version should
  add a check, though it adds a branch in the hot path.

---

## Optimization B: Inline minimal send loop on the client (low impact)

### Current code (`client.cpp:71-78`)

```cpp
for (size_t j = 0; j < count; j++) {
    if (send_full(fd, buf, size) < 0) {
        perror("send");
        delete[] buf;
        close(fd);
        return 1;
    }
}
```

`send_full()` is defined in `common.cpp` (different translation unit). Even with `-O3`,
g++ cannot inline it without Link-Time Optimization (`-flto`). Each call pays:

| Overhead item | Est. cost |
|--------------|-----------|
| Function call (push args, jump, return) | ~5–10 cycles |
| `while (total < n)` check and branch | ~2 cycles |
| `if (sent < 0)` — EINTR check | ~2 cycles |
| `if (sent == 0)` — disconnect check | ~2 cycles |
| **Total per-message overhead** | **~11–16 cycles (~3–5 ns)** |

The `send()` syscall itself costs ~1000–3000 cycles. So the overhead is ~0.3–1.6% —
small but measurable across 100,000 iterations (~300–500 μs total).

### Proposed change

Move a minimal inline version into `client.cpp` so the compiler can inline it:

```cpp
// Inline — no function call, no error branches, just the minimum loop
// to handle partial writes (which are rare with large socket buffers).
static inline void timed_send(int fd, const void* buf, size_t n) {
    size_t total = 0;
    const char* ptr = static_cast<const char*>(buf);
    while (total < n) {
        total += static_cast<size_t>(
            send(fd, ptr + total, n - total, MSG_NOSIGNAL)
        );
    }
}
```

Timed loop becomes:
```cpp
for (size_t j = 0; j < count; j++) {
    timed_send(fd, buf, size);
}
```

Or, if partial writes are considered impossible (large SO_SNDBUF, localhost):

```cpp
for (size_t j = 0; j < count; j++) {
    send(fd, buf, size, MSG_NOSIGNAL);  // single syscall, no loop at all
}
```

### Expected impact

~0.3–1.6% improvement in measured throughput for small messages. Negligible for large
messages (where count is small and data transfer dominates).

### Risks

- **Partial writes:** if `send()` transfers fewer bytes than requested (possible when
  the socket buffer is full), the raw version silently undercounts bytes → throughput
  is overstated. The inline-loop version handles this correctly.
- **Error blindness:** neither version checks for errors. A connection break during the
  timed batch means the client keeps calling `send()` on a dead socket, producing garbage
  throughput numbers. The current code catches this and exits cleanly.

---

## Optimization C: Large socket buffers (medium impact)

### Current state

The kernel's default send/receive buffer is typically 208 KB (`net.core.wmem_default`).
When the client sends faster than the server receives, the client's `send()` blocks
until buffer space frees up — and the timer is running during that block.

### Proposed change

Increase both buffers to 16 MB (kernel caps to `net.core.wmem_max`/`rmem_max`):

```cpp
int buf = 16 * 1024 * 1024;
setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));  // client
setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));  // server (accepted fd)
```

### Expected impact

- **Small messages:** negligible — the send rate is limited by syscall overhead,
  not buffer space.
- **Large messages:** significant — 1 MB messages fill a 208 KB buffer in ~5 sends,
  after which the client stalls waiting for the server to drain. A 16 MB buffer
  absorbs the entire 100-message batch without blocking.

### Risks

- Kernel enforces an upper bound (`/proc/sys/net/core/wmem_max`). The `setsockopt`
  call succeeds but the actual buffer is silently capped to the kernel max.
- Large buffers consume non-swappable kernel memory (16 MB per socket direction).
  Not an issue for a single-connection benchmark.

---

## Optimization D: Remove error branches from client timed loop (very low impact)

### Current code

```cpp
for (size_t j = 0; j < count; j++) {
    if (send_full(fd, buf, size) < 0) {
        perror("send");
        delete[] buf;
        close(fd);
        return 1;
    }
}
```

The `if (send_full(...) < 0)` check, the `perror`, and the cleanup block are all inside
the timed loop. The compiler may place the cold error-handling code off the hot path,
but the branch-predictor still pays for the conditional jump.

### Proposed change

Hoist the error check out of the loop — check once after all sends complete:

```cpp
bool ok = true;
for (size_t j = 0; j < count; j++) {
    if (send_full(fd, buf, size) < 0) { ok = false; break; }
}
if (!ok) { /* handle error */ }
```

Or, since the user specified willingness to remove error checks entirely:

```cpp
for (size_t j = 0; j < count; j++) {
    send_full(fd, buf, size);  // return value ignored
}
```

### Expected impact

Negligible (~1 branch per message saved). The branch predictor already handles the
rarely-taken error path efficiently. This is a micro-optimization at best.

---

## Optimization E: Buffer pre-allocation (zero impact on timer, code quality only)

### Current code

Each iteration: `new char[size]` → use → `delete[] buf` (plus `memset`).

### Note

This happens **outside the timer** (before `start` / after `end`). It has zero effect
on measured throughput. The only reason to change it is code cleanliness: one allocation
of max_size (1 MB) reused across all iterations, eliminating 21 heap operations and
the `memset` (data content is irrelevant to throughput).

---

## Summary

| Opt | What | Where | Inside timer? | Estimated gain |
|-----|------|-------|:---:|---------------|
| **A** | Batched `recv()` with MSG_WAITALL | server | ✓ | **High** — up to 100K fewer syscalls per size |
| **B** | Inline send loop, no function call | client | ✓ | Low — ~1% per message |
| **C** | Large socket buffers (16 MB) | both | ✓ | Medium — reduces send-side blocking for large messages |
| **D** | Remove error branches from timed loop | client | ✓ | Very low — 1 branch per message |
| **E** | Buffer pre-allocation + no memset | both | ✗ | **Zero** — not on the clock |

**Recommendation:** Implement A and C. Skip B and D (complexity not worth the gain).
Skip E or do it as a cleanup pass unrelated to performance.

---

## Dirty-but-fast variant

If error handling is entirely sacrificed in the timed path (the user explicitly asked
about this), the client timed section becomes:

```cpp
auto start = std::chrono::high_resolution_clock::now();

for (size_t j = 0; j < count; j++) {
    send(fd, buf, size, MSG_NOSIGNAL);
}

uint64_t ack;
recv(fd, &ack, sizeof(ack), 0);

auto end = std::chrono::high_resolution_clock::now();
```

And the server timed section:

```cpp
recv(client_fd, buf, count * size, MSG_WAITALL);
uint64_t ack = 0;
send(client_fd, &ack, sizeof(ack), MSG_NOSIGNAL);
```

This is the absolute minimum: two syscalls per size on the client (the send loop +
ACK recv), two on the server (one bulk recv + ACK send). No error checks, no loops
in userspace, no function calls. The throughput numbers will be as high as physically
possible — but a single network hiccup produces garbage output instead of an error
message.

### What breaks without error checks

- **Partial sends:** if `send(buf, 1MB)` only transfers 500 KB, the remaining 500 KB
  is never sent. The server's `MSG_WAITALL` blocks forever waiting for bytes that
  will never arrive → **hang.**
- **Connection drop:** if the peer disconnects, `send()` eventually raises SIGPIPE
  or returns EPIPE (ignored). The client keeps calling `send()` forever →
  **infinite loop** (or until the kernel kills it).
- **Server crash:** client calls `recv()` for the ACK, gets 0 (connection closed),
  interprets it as a valid ACK, stops timer early → **wildly inflated throughput.**
