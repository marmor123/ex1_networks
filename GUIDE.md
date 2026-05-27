# Code Walkthrough & Design Rationale

This guide explains every section of the TCP throughput benchmark — line by line, decision by decision. Use this to prepare for code interviews.

---

## Makefile

### Toolchain selection

```makefile
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3
```

**Why g++?** The university lab machines run Linux with GCC. No cross-platform concerns — we stripped Windows support because it added dead weight.

**Why C++17?** We use two C++17 features: `inline` variables (for `MSG_COUNTS` in the header) and `std::size()` (for the bounds-check assertion). These avoid ODR violations and manual `sizeof` arithmetic respectively.

**Why `-Wall -Wextra`?** Catches implicit sign conversions, unused variables, and missing returns at compile time. Zero warnings is a hard requirement.

**Why `-O3`?** A throughput benchmark must run at maximum speed. `-O3` enables aggressive function inlining, loop vectorization, and inter-procedural optimizations beyond `-O2`. We are measuring the network, not the compiler's ability to generate safe debug code. Any cycle spent in suboptimal code inflates the measurement.

### Build targets

```makefile
server: server.cpp common.o
client: client.cpp common.o
common.o: common.cpp common.h
```

**Why a shared object file?** `common.cpp` is compiled once into `common.o` and linked into both executables. This is correct because the code is genuinely shared, not duplicated. (Earlier versions had `MSG_COUNTS` duplicated in both executables — that was a bug we fixed.)

**Why does `test` auto-run?** `make test` both compiles and executes. TDD workflow: one command to verify everything still works after any change.

---

## common.h — Shared Header

### Include strategy

```cpp
#include <sys/types.h>
```

**Why `<sys/types.h>`?** Provides `ssize_t` on POSIX systems. We previously had an `#ifdef _WIN32` shim using `BaseTsd.h` and `typedef SSIZE_T ssize_t`, but removed it since the target is Linux-only. Including the POSIX header directly is simpler and correct for our platform.

### Constants

```cpp
constexpr int DEFAULT_PORT = 12345;
constexpr int LISTEN_BACKLOG = 5;
constexpr int WARMUP_MSGS = 100;  // per-size warmup messages to saturate TCP slow-start
```

**Why hardcode the port?** The exercise spec requires it — the server takes no arguments, the client takes only the server IP. A single port simplifies the auto-tester script.

**Why backlog = 5?** We only ever accept one client. The backlog value is essentially unused, but POSIX requires *some* positive value. 5 is the traditional minimum.

**Why 100 warmup messages per size?** TCP slow-start begins with a congestion window of ~10 segments (≈14 KB). After each successful RTT, the window doubles. Per-size warmup ensures the window is fully open before each timed batch. This is important because the lecturer noted there may be TCP-level dynamics we don't fully understand that benefit from per-size warmup. 100 messages is enough to saturate the window for all sizes — for 1B messages that's only 100 bytes of warmup, while for 1MB messages it provides 100MB of warmup to fully open a large window.

**Why per-size instead of one-time warmup?** Earlier versions used a single warmup phase at connection start (8×1MB messages). The lecturer advised that per-size warmup is more robust — there may be TCP dynamics beyond simple congestion-window opening that affect different message sizes differently. The key fix: the server now receives warmup and timed messages in **separate loops**, only ACKing after the timed batch. This prevents warmup transmission time from contaminating the throughput measurement.

### Message size generation

```cpp
std::vector<size_t> generate_sizes();
```

**Why a vector and not a hardcoded array?** The series is mathematically defined (2^0 through 2^20). Generating it programmatically is self-documenting and can't have transcription errors. An interviewer can see at a glance that the series is correct.

### MSG_COUNTS — single source of truth

```cpp
inline const size_t MSG_COUNTS[] = { ... };
```

**Why `inline const` in the header?** The array is needed by both `server.cpp` and `client.cpp`. In C++17, `inline` variables allow a single definition in a header without ODR (One Definition Rule) violations — each translation unit sees the same array at the same address. Before this fix, the array was duplicated in both executables, creating a risk that editing one copy without the other would silently desynchronize the protocol.

**Why do the counts decrease as message size increases?** Smaller messages have higher per-system-call overhead: for 1-byte messages, the `send()` syscall cost dominates, so we need many iterations (~100K) to get a stable timing measurement. For 1 MB messages, a single `send()` already transfers substantial data, so fewer iterations (~100) suffice. This keeps total runtime under 30 seconds.

**How were these counts chosen?** Using the convergence detector (`find_counts()` in `client.cpp`, inside `#if 0`). For each message size, it starts with a small count and doubles until measured throughput varies by less than 1% between consecutive counts. The output tells us the minimum count needed for stable measurements at each size. The convergence checks variance between **different counts** (not repeated runs of the same count), because we're finding the point where increasing the count no longer meaningfully changes the measured throughput.

### ThroughputResult

```cpp
struct ThroughputResult {
    double value;
    std::string unit;
};
```

**Why separate value and unit?** The unit auto-scales (bps → Kbps → Mbps → Gbps). Keeping them separate avoids ambiguity — the value is always relative to its unit. Using a `double` for the value allows fractional results (e.g., 1.23 Mbps).

### Socket utility declarations

```cpp
ssize_t send_full(int fd, const void* buf, size_t n);
ssize_t recv_full(int fd, void* buf, size_t n);
```

**Why wrap `send()`/`recv()`?** POSIX `send()` and `recv()` are not guaranteed to transfer all requested bytes in one call. They may perform partial transfers (e.g., send 500 of 1000 bytes). For a protocol that exchanges fixed-size messages, partial transfers break correctness — the server would interpret half a message as a complete one. Our wrappers loop until all bytes are transferred or an error occurs.

**Why return `ssize_t`?** Matches the POSIX convention: non-negative = bytes transferred, negative = error. Callers check `< 0` to detect failure, which is intuitive for anyone familiar with POSIX.

---

## common.cpp — Shared Implementation

### generate_sizes()

```cpp
for (int i = 0; i <= 20; i++) {
    sizes.push_back(static_cast<size_t>(1ULL << i));
}
```

**Why `1ULL`?** The `ULL` suffix guarantees 64-bit unsigned arithmetic for the shift. Without it, `1 << 20` would be an `int` (32-bit signed on most platforms), and `1 << 31` would overflow into the sign bit — undefined behavior. `ULL` prevents this entirely.

**Why `static_cast<size_t>`?** `size_t` is the natural type for buffer sizes in C++. The cast narrows from `unsigned long long` to `size_t`, which on 64-bit Linux is the same width — a no-op.

### compute_throughput()

```cpp
if (elapsed_sec <= 0.0) return {0.0, "bps"};
```

**Why the zero guard?** In virtualized environments (common on lab machines), `high_resolution_clock` can have coarse granularity (1ms+). If a batch completes faster than the clock tick, `end - start` is 0.0. Dividing by zero produces `inf` in IEEE 754, which would propagate through the unit-selection logic and print as garbage. Returning 0 bps is a safe sentinel — the condition is essentially "too fast to measure."

```cpp
double bits_per_sec = (static_cast<double>(msg_size) * msg_count * 8.0) / elapsed_sec;
```

**Why cast to double before multiplication?** `msg_size * msg_count` is a `size_t` multiplication. If both are large on a 32-bit platform, the product overflows before the `* 8.0` promotes to double. Casting `msg_size` to double first forces floating-point arithmetic for the entire expression, which has a range far exceeding any realistic byte count.

**Why multiply by 8?** Throughput is measured in bits per second (networking convention), but our data is counted in bytes.

**Why the cascading if-else chain?** Auto-scaling to the most readable unit:
- < 1,000 bps → display as bps
- < 1,000,000 bps → display as Kbps
- < 1,000,000,000 bps → display as Mbps
- Otherwise → display as Gbps

The thresholds follow SI prefixes (powers of 1000, not 1024 — networking uses decimal prefixes).

### send_full()

```cpp
ssize_t sent = send(fd, ptr + total, n - total, MSG_NOSIGNAL);
```

**Why `MSG_NOSIGNAL`?** On Linux, writing to a socket whose peer has closed triggers `SIGPIPE`, which kills the process by default. This bypasses all our error handling — the process just dies with exit code 141. `MSG_NOSIGNAL` suppresses the signal and makes `send()` return `EPIPE` as a normal error, which our code handles gracefully. This was a critical bug in earlier versions.

```cpp
if (sent < 0) {
    if (errno == EINTR) continue;
    return -1;
}
```

**Why handle `EINTR`?** POSIX signals can interrupt blocking system calls. If a signal arrives during `send()`, the call returns -1 with `errno = EINTR`. This is not a real error — no data was lost, the call was just interrupted. We `continue` to retry from the same position.

```cpp
if (sent == 0) return -1;
```

**Why treat `send()` returning 0 as an error?** On a blocking stream socket with a non-zero buffer, `send()` should never return 0 under POSIX. If it does, it indicates an unusual condition (e.g., the connection was shut down for writing). Treating it as an error prevents an infinite loop.

### recv_full()

```cpp
if (rcvd == 0) return -1;
```

**Why does `recv()` returning 0 mean connection closed?** In TCP, `recv()` returning 0 means the peer performed an orderly shutdown (`close()` or `shutdown(SHUT_WR)`). There is no more data to receive. Our protocol expects exactly `n` bytes, so a premature close is an error.

### create_server_socket()

```cpp
int opt = 1;
if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt SO_REUSEADDR");
}
```

**Why `SO_REUSEADDR`?** When a server process exits, the TCP port enters `TIME_WAIT` state for ~60 seconds. Without `SO_REUSEADDR`, restarting the server during this window fails with `EADDRINUSE`. This is essential during development and testing where the server is restarted frequently.

**Why check the return value but not abort?** `SO_REUSEADDR` is a convenience, not a correctness requirement. If it fails (unlikely, but possible on restricted kernels), the server might fail later at `bind()` with a clear error. We log the failure for diagnostics but continue.

```cpp
addr.sin_addr.s_addr = INADDR_ANY;
```

**Why `INADDR_ANY`?** Binds to all available network interfaces. The server doesn't need to know which interface the client will connect through — it accepts connections on any.

```cpp
addr.sin_port = htons(static_cast<uint16_t>(port));
```

**Why `htons()`?** Network byte order is big-endian; host byte order may be little-endian (x86). `htons()` ("host to network short") converts the port number to the correct byte order for the wire.

### create_client_socket()

```cpp
if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
    perror("setsockopt TCP_NODELAY");
}
```

**Why `TCP_NODELAY`?** Disables Nagle's algorithm. Without this, the kernel delays small sends (up to 200ms) hoping to coalesce them into larger segments. For a throughput benchmark measuring per-message timing, this delay would dominate the measurement for small messages, producing artificially low and inconsistent throughput numbers. Nagle must be off so we measure the network, not the kernel's buffering policy.

```cpp
int pton_ret = inet_pton(AF_INET, ip, &addr.sin_addr);
if (pton_ret == 0) {
    fprintf(stderr, "inet_pton: invalid address '%s'\n", ip);
```

**Why separate `return 0` from `return -1`?** `inet_pton()` returns 0 when the input string is not a valid IP address (e.g., "not_an_ip") and -1 on system errors. But `errno` is only set on -1. In earlier versions, we used a combined `<= 0` check with `perror()`, which printed "Success" or a stale error message when given an invalid address. Separating the two paths gives the user a clear, accurate error message.

---

## server.cpp

### Compile-time bounds check

```cpp
static_assert(sizeof(MSG_COUNTS) / sizeof(MSG_COUNTS[0]) == 21, ...);
```

**Why a compile-time check?** If `MSG_COUNTS` is accidentally edited to have more or fewer than 21 entries (e.g., a comma error), the build fails with a clear message instead of producing a binary that reads out of bounds at runtime.

### Runtime bounds check

```cpp
auto sizes = generate_sizes();
assert(sizes.size() == std::size(MSG_COUNTS));
```

**Why both compile-time and runtime checks?** The `static_assert` verifies `MSG_COUNTS` has 21 entries. The `assert` verifies `sizes` also has 21 entries. They could diverge if `generate_sizes()` is modified. Together they guarantee the loop below never accesses `MSG_COUNTS[i]` out of bounds.

### TCP_NODELAY on accepted socket

```cpp
int nodelay = 1;
setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
```

**Why on the accepted socket too?** The server sends 8-byte ACK messages after each batch. Without `TCP_NODELAY`, Nagle's algorithm could delay these ACKs by up to 200ms. This would inflate the client's measured elapsed time (since the timer runs until the ACK arrives), producing artificially low throughput.

### Per-size warmup receive

```cpp
for (int w = 0; w < WARMUP_MSGS; w++) {
    recv_full(client_fd, buf, size);
}
```

**Why receive and discard?** The client sends 100 warmup messages of the current test size before each timed batch. The server must consume this data from the TCP stream before the timed batch begins. By receiving warmup in a **separate loop** from timed messages, the server ensures all warmup bytes are drained before the timed data is processed. This prevents warmup transmission time from being included in the client's timer measurement.

### Per-size timed receive

```cpp
for (size_t j = 0; j < count; j++) {
    recv_full(client_fd, buf, size);
}
```

**Why a separate loop from warmup?** This is the critical fix for measurement accuracy. The client's timer starts after warmup send and stops after ACK. If the server mixed warmup and timed messages in one loop (as earlier versions did), the ACK would only be sent after *both* were received — meaning the timer would include warmup transmission time. By separating the loops, the server only ACKs the timed batch, and the warmup bytes are fully consumed before the timed data arrives at the server.

### ACK

```cpp
uint64_t ack = 0;
send_full(client_fd, &ack, sizeof(ack));
```

**Why send zero instead of computing `size * count`?** The ACK value is never validated by the client — it exists purely as a synchronization barrier to tell the client "I've received everything for this batch." Computing the total bytes was wasted work. Sending a constant zero keeps the protocol structure (the client blocks on `recv_full` for 8 bytes) without the unnecessary arithmetic.

**Why keep 8 bytes instead of reducing to 1?** Protocol consistency. The `uint64_t` ACK is a fixed-size field. Changing to a single byte would save 7 bytes per batch (147 bytes total across 21 batches) — negligible.

---

## client.cpp

### Convergence detector (`#if 0` block)

```cpp
#if 0
void find_counts(int fd) { ... }
#endif
```

**Why is this commented out?** This function is not part of the normal benchmark. It's a development tool used once to determine the optimal `MSG_COUNTS` values, then disabled. The `#if 0` / `#endif` preserves the code for future re-tuning without cluttering the compiled binary.

**How does it work?** For each message size, it starts with 10 messages and doubles until throughput variance between consecutive counts falls below 1%. This finds the minimum count that produces stable measurements — balancing accuracy against runtime. The variance is measured between **different counts** (current vs previous, which was half the size), not between repeated runs of the same count. The idea: if throughput at count=100 is nearly identical to throughput at count=200, then count=100 is already stable — increasing it further won't change the result.

**Why does it include its own warmup?** The convergence detector runs as a standalone function replacing `main()`. It needs to handle its own warmup to get accurate per-count measurements.

### Command-line parsing

```cpp
if (argc != 2) {
    fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
    return 1;
}
```

**Why only the server IP?** Per the exercise spec. The port is hardcoded. The auto-tester expects exactly this interface.

### Per-size warmup send

```cpp
for (int w = 0; w < WARMUP_MSGS; w++) {
    send_full(fd, buf, size);
}
```

**Why per-size warmup?** The lecturer advised that there may be TCP-level dynamics beyond simple congestion-window opening that affect different message sizes differently. Per-size warmup ensures the path is fully saturated before each batch, regardless of message size. For small messages, 100×1B = 100 bytes of warmup; for large messages, 100×1MB = 100MB of warmup.

**Why before the timer?** The warmup exists to saturate the TCP window. If we included it in the timer, the measurement would capture the slow-start ramp-up rather than steady-state throughput. By sending warmup before `start = now()`, we measure only the steady-state portion.

### Timed batch

```cpp
auto start = std::chrono::high_resolution_clock::now();

for (size_t j = 0; j < count; j++) {
    send_full(fd, buf, size);
}

uint64_t ack = 0;
recv_full(fd, &ack, sizeof(ack));

auto end = std::chrono::high_resolution_clock::now();
```

**Why `high_resolution_clock`?** Provides the best available timer resolution (typically nanoseconds on modern Linux). For small messages, the timed batch can complete in microseconds — we need precision to get meaningful measurements.

**Why does the timer include the ACK wait?** This is intentional. Stopping the timer after receiving the server's ACK measures the full network round-trip, not just the client's send rate. The server only sends the ACK after receiving all timed messages (the warmup was already drained in the separate warmup loop). This captures the network path rather than just the local TCP stack's buffer-accept speed.

**Why is the ACK value never checked?** The ACK exists purely for synchronization — it tells the client "done with this batch." The value is irrelevant. Both sides share the same `MSG_COUNTS` array (guaranteed by `static_assert` and `assert` guards), so message counts can't diverge.

### Output format

```cpp
printf("%zu\t%.2f\t%s\n", size, result.value, result.unit.c_str());
```

**Why `%zu` for size?** `size_t` is the correct format specifier for `size_t` on all platforms. Using `%lu` would break on platforms where `size_t` is a different width than `unsigned long`.

**Why `\t` (tab) delimiters?** The exercise spec requires exactly three tab-delimited columns. A provided auto-tester script parses this format.

**Why `%.2f`?** Two decimal places is sufficient precision for throughput measurements. More digits would imply false precision (network measurements are inherently noisy).

---

## test_common.cpp

### Custom CHECK macro

```cpp
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        return; \
    } \
} while (0)
```

**Why not `assert()`?** `assert()` is a no-op when compiled with `-DNDEBUG` (common in release builds). If someone adds `-DNDEBUG` to `CXXFLAGS`, every test silently passes without running any checks. Our `CHECK` macro always executes the condition, regardless of build configuration.

**Why `do { ... } while (0)`?** This is the standard C macro idiom that makes `CHECK(x)` behave like a single statement. Without it, `if (something) CHECK(x); else ...` would parse incorrectly.

**Why `return` on failure instead of `exit`?** `return` exits only the current test function, allowing subsequent tests to run. `exit` would terminate the entire test suite at the first failure. Running all tests gives a complete picture of what's broken.

### Test coverage

**Why test `generate_sizes()` three ways?** The count test verifies the right number of sizes. The powers-of-two test verifies each individual value. The range test verifies the endpoints. Together they catch off-by-one errors, shift errors, and incorrect final values.

**Why test `compute_throughput()` at every unit boundary?** The auto-scaling logic has thresholds at 1e3, 1e6, and 1e9 bps. Testing at each scale (bps, Kbps, Mbps, Gbps) plus a boundary case (100 Mbps — near the Mbps→Gbps transition) verifies the scaling logic is correct for all possible throughput values.

**Why test zero elapsed time?** This is a defensive edge case. In virtualized environments, the clock can tick slowly enough that a fast batch measures 0.0 seconds. The test verifies our guard returns 0 bps instead of `inf`.

---

## Key Protocol: Warmup → Timed → ACK Pipeline

For each message size, the protocol executes:

```
CLIENT                              SERVER
  |                                   |
  |--- warmup msg 1 (size) ---------->|  } 
  |--- warmup msg 2 (size) ---------->|  } WARMUP_MSGS messages
  |        ...                        |  } received and discarded
  |--- warmup msg 100 (size) -------->|  }
  |                                   |
  |  start = now()                    |
  |                                   |
  |--- timed msg 1 (size) ----------->|  }
  |--- timed msg 2 (size) ----------->|  } MSG_COUNTS[i] messages
  |        ...                        |  } received in timed loop
  |--- timed msg N (size) ----------->|  }
  |                                   |
  |                        ACK <------|  send_full(&ack=0)
  |                                   |
  |  end = now()                      |
  |  throughput = bytes / elapsed     |
```

**Why this ordering prevents measurement error:** The server receives all warmup messages in its own loop *before* entering the timed receive loop. By the time the server starts receiving timed messages, the warmup bytes have already been consumed. The server only sends the ACK after the timed batch is fully received. The client's timer spans only the timed send + server processing + ACK return — warmup transmission time is excluded.

**Why one ACK per size:** The ACK serves as a batch delimiter. After each size's timed batch, the server signals "ready for next size" by sending the ACK. Without it, the client would have no way to know when the server has finished processing the current batch, and the protocol would desynchronize.

---

## Design Decisions Summary

| Decision | Rationale |
|----------|-----------|
| C++17 over C11 | `inline` variables, `std::size`, cleaner standard library |
| `-O3` over `-O2` | Max optimization — measuring network, not compiler |
| POSIX sockets over Boost.Asio | No external dependencies — standard on Linux |
| Per-size warmup (100 msgs) | Lecturer guidance; handles unknown TCP dynamics per size |
| Separate warmup/timed loops on server | Eliminates timing skew — timer never includes warmup |
| `MSG_COUNTS` in header as `inline const` | Single source of truth, no ODR duplication |
| `MSG_NOSIGNAL` on send | Prevents SIGPIPE from killing the process on disconnect |
| `TCP_NODELAY` on client AND server | Prevents Nagle from delaying both data and ACKs |
| Timer spans send + ACK wait | Measures network round-trip, not just local send rate |
| ACK value = 0 (constant) | Value is never validated — computing it is wasted work |
| Custom CHECK macro over assert | Tests work regardless of NDEBUG |
| Compile-time + runtime bounds checks | Defense in depth — catches mismatches early |
