# Code Walkthrough & Design Rationale

This guide explains every section of the TCP throughput benchmark — line by line, decision by decision. Use this to prepare for code interviews.

---

## Makefile

### Toolchain selection

```makefile
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
```

**Why g++?** The university lab machines run Linux with GCC. No cross-platform concerns — we stripped Windows support because it added dead weight.

**Why C++17?** We use two C++17 features: `inline` variables (for `MSG_COUNTS` in the header) and `std::size()` (for the bounds-check assertion). These avoid ODR violations and manual `sizeof` arithmetic respectively.

**Why `-Wall -Wextra`?** Catches implicit sign conversions, unused variables, and missing returns at compile time. Zero warnings is a hard requirement.

**Why `-O2`?** Throughput benchmarks must run at production speed. Measuring unoptimized code would produce meaningless numbers — the bottleneck would be the compiler's debug instrumentation, not the network.

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
constexpr size_t WARMUP_SIZE = 1048576;   // 1 MB
constexpr int WARMUP_COUNT = 8;
```

**Why hardcode the port?** The exercise spec requires it — the server takes no arguments, the client takes only the server IP. A single port simplifies the auto-tester script.

**Why backlog = 5?** We only ever accept one client. The backlog value is essentially unused, but POSIX requires *some* positive value. 5 is the traditional minimum.

**Why warmup with 1 MB messages?** TCP slow-start begins with a congestion window of ~10 segments (≈14 KB). After each successful RTT, the window doubles. To saturate a window large enough for our biggest test message (1 MB), we need the window to reach at least 1 MB. Sending 8 messages of 1 MB each forces the congestion window to open fully within the first few RTTs, regardless of the initial window size.

**Why 8 warmup messages?** Each 1 MB message consumes one window's worth of data at the point the window reaches 1 MB. By the 3rd or 4th message, the window is fully open. 8 messages provides margin for networks with larger initial windows or higher latency, without adding significant runtime (8 MB is negligible compared to the total benchmark data volume).

**Why one-time warmup instead of per-size warmup?** Earlier versions warmed up before every message size. This caused a measurement error: the server received warmup + timed messages as one batch and only ACKed after both. The client's timer, which started after warmup send, included the warmup transmission time in the elapsed measurement. Moving to one-time warmup at connection start eliminated this skew entirely. The TCP congestion window is per-connection, not per-message-size — once open, it stays open.

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

**How were these counts chosen?** Using the convergence detector (`find_counts()` in `client.cpp`, inside `#if 0`). It starts with a small count and doubles until measured throughput varies by less than 1% between iterations. The output tells us the minimum count needed for stable measurements at each size.

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

**Why on the accepted socket too?** The server sends 8-byte ACK messages after each batch. Without `TCP_NODELAY`, Nagle's algorithm could delay these ACKs by up to 200ms. This would inflate the client's measured elapsed time (since the timer runs until the ACK arrives), producing artificially low throughput. This is a recent fix — earlier versions missed this.

### One-time warmup receive

```cpp
char* warmup_buf = new char[WARMUP_SIZE];
for (int w = 0; w < WARMUP_COUNT; w++) {
    recv_full(client_fd, warmup_buf, WARMUP_SIZE);
}
delete[] warmup_buf;
```

**Why receive and discard?** The client sends 8 warmup messages of 1 MB each immediately after connecting. The server must consume this data from the TCP stream before the timed batches begin. The content doesn't matter — it's just there to open the congestion window. Using `new[]`/`delete[]` for the buffer because `WARMUP_SIZE` (1 MB) is too large for the stack.

### Per-size receive loop

```cpp
for (size_t j = 0; j < count; j++) {
    recv_full(client_fd, buf, size);
}
```

**Why allocate a new buffer each iteration?** Each message size is different (1B through 1MB). Allocating exactly `size` bytes is correct — it matches what the client sends. Using `new[]`/`delete[]` per iteration is acceptable because the outer loop has only 21 iterations.

### ACK calculation

```cpp
uint64_t ack = static_cast<uint64_t>(size) * count;
```

**Why cast before multiplication?** On 32-bit platforms, `size_t` is 32 bits. `size * count` could overflow before assignment to `uint64_t`. Casting `size` to `uint64_t` first forces 64-bit multiplication, which is safe for any realistic values.

**Why send an ACK at all?** The ACK serves two purposes: (1) it synchronizes the protocol — the server signals "I've received everything for this batch, proceed to the next," and (2) the client uses it to stop the timer, measuring the full round-trip including server processing time.

---

## client.cpp

### Convergence detector (`#if 0` block)

```cpp
#if 0
void find_counts(int fd) { ... }
#endif
```

**Why is this commented out?** This function is not part of the normal benchmark. It's a development tool used once to determine the optimal `MSG_COUNTS` values, then disabled. The `#if 0` / `#endif` preserves the code for future re-tuning without cluttering the compiled binary.

**How does it work?** For each message size, it starts with 10 messages and doubles until throughput variance between iterations falls below 1%. This finds the minimum count that produces stable measurements — balancing accuracy against runtime.

### Command-line parsing

```cpp
if (argc != 2) {
    fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
    return 1;
}
```

**Why only the server IP?** Per the exercise spec. The port is hardcoded. The auto-tester expects exactly this interface.

### One-time warmup send

```cpp
char* warmup_buf = new char[WARMUP_SIZE];
memset(warmup_buf, 0, WARMUP_SIZE);
for (int w = 0; w < WARMUP_COUNT; w++) {
    send_full(fd, warmup_buf, WARMUP_SIZE);
}
```

**Why zero the buffer?** The content of warmup messages is irrelevant — they exist only to fill the TCP pipe. Zeroing is the simplest initialization and costs ~1ms for 1 MB, negligible for 8 iterations.

**Why one-time rather than per-size?** Earlier versions warmed up before each message size. This introduced a measurement error because the server had to receive the warmup bytes before ACKing the timed batch. The timer included warmup transmission time in the elapsed measurement. Moving warmup to connection start eliminates this cross-contamination: once the TCP window is open, every timed batch measures only the data it's supposed to.

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

**Why does the timer include the ACK wait?** This is intentional. Stopping the timer after receiving the server's ACK measures the full network round-trip, not just the client's send rate. This captures the network path rather than just the local TCP stack's buffer-accept speed.

**Why is the ACK value never checked?** The ACK exists purely for synchronization — it tells the client "done with this batch." The value itself (`size * count`) is sent but not verified because both sides share the same `MSG_COUNTS` array (guaranteed by the `static_assert` and `assert` guards). Validating the value would catch memory corruption but not protocol errors.

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

## Design Decisions Summary

| Decision | Rationale |
|----------|-----------|
| C++17 over C11 | `inline` variables, `std::size`, cleaner standard library |
| POSIX sockets over Boost.Asio | No external dependencies — standard on Linux |
| One-time warmup over per-size | Eliminates timing skew; TCP window is per-connection |
| `MSG_COUNTS` in header as `inline const` | Single source of truth, no ODR duplication |
| `MSG_NOSIGNAL` on send | Prevents SIGPIPE from killing the process on disconnect |
| `TCP_NODELAY` on client AND server | Prevents Nagle from delaying both data and ACKs |
| Timer spans send + ACK wait | Measures network round-trip, not just local send rate |
| Custom CHECK macro over assert | Tests work regardless of NDEBUG |
| Compile-time + runtime bounds checks | Defense in depth — catches mismatches early |
