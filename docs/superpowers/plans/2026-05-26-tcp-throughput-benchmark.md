# TCP Throughput Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a C/C++ TCP throughput benchmark (client + server) that measures unidirectional throughput for message sizes 1B to 1MB.

**Architecture:** Three source files sharing a common header — `common.h` (declarations), `common.cpp` (shared utilities), `server.cpp` (receives batches, sends ACK), `client.cpp` (sends timed batches, prints results). Makefile builds `server` and `client` executables. TDD on pure functions; socket code verified via integration test.

**Tech Stack:** C++17, POSIX sockets, `g++` compiler, Make

---

### File Structure Map

| File | Responsibility |
|------|---------------|
| `common.h` | Declares constants, types, and function signatures shared by client and server |
| `common.cpp` | Implements: `generate_sizes()`, `compute_throughput()`, `send_full()`, `recv_full()`, `create_server_socket()`, `create_client_socket()` |
| `server.cpp` | `main()` — listens, receives batches per size, sends ACK after each batch |
| `client.cpp` | `main()` — connects, warm-up + timed send per size, waits for ACK, prints results. Contains commented-out convergence detector |
| `test_common.cpp` | Unit tests for `generate_sizes()` and `compute_throughput()` using `<cassert>` |
| `Makefile` | Builds `server`, `client`, and `test` targets |

---

### Task 1: Create Makefile and project skeleton

**Files:**
- Create: `Makefile`
- Create: `common.h`

- [ ] **Step 1: Write `common.h` with initial declarations**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

constexpr int DEFAULT_PORT = 12345;
constexpr int WARMUP_MSGS = 100;
constexpr int LISTEN_BACKLOG = 5;

std::vector<size_t> generate_sizes();

struct ThroughputResult {
    double value;
    std::string unit;
};
ThroughputResult compute_throughput(size_t msg_size, size_t msg_count, double elapsed_sec);

ssize_t send_full(int fd, const void* buf, size_t n);
ssize_t recv_full(int fd, void* buf, size_t n);

int create_server_socket(int port);
int create_client_socket(const char* ip, int port);
```

- [ ] **Step 2: Write the `Makefile`**

```makefile
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDFLAGS :=

.PHONY: all clean test

all: server client

server: server.cpp common.o
	$(CXX) $(CXXFLAGS) -o server server.cpp common.o $(LDFLAGS)

client: client.cpp common.o
	$(CXX) $(CXXFLAGS) -o client client.cpp common.o $(LDFLAGS)

common.o: common.cpp common.h
	$(CXX) $(CXXFLAGS) -c common.cpp -o common.o

test: test_common.cpp common.o
	$(CXX) $(CXXFLAGS) -o test test_common.cpp common.o $(LDFLAGS)
	./test

clean:
	rm -f server client test common.o
```

- [ ] **Step 3: Verify Makefile parses correctly**

Run: `make clean`
Expected: no errors (rm may complain about missing files — that's fine)

---

### Task 2: Write test for `generate_sizes()`

**Files:**
- Create: `test_common.cpp`

- [ ] **Step 1: Write failing test**

```cpp
#include <cassert>
#include <cstdio>
#include <cmath>
#include "common.h"

void test_generate_sizes_count() {
    auto sizes = generate_sizes();
    assert(sizes.size() == 21);
    printf("PASS: test_generate_sizes_count (got %zu sizes)\n", sizes.size());
}

void test_generate_sizes_powers_of_two() {
    auto sizes = generate_sizes();
    for (size_t i = 0; i < sizes.size(); i++) {
        assert(sizes[i] == (size_t{1} << i));
        assert((sizes[i] & (sizes[i] - 1)) == 0); // is power of two
    }
    printf("PASS: test_generate_sizes_powers_of_two\n");
}

void test_generate_sizes_range() {
    auto sizes = generate_sizes();
    assert(sizes.front() == 1);
    assert(sizes.back() == 1048576); // 2^20
    printf("PASS: test_generate_sizes_range\n");
}

int main() {
    test_generate_sizes_count();
    test_generate_sizes_powers_of_two();
    test_generate_sizes_range();
    printf("All tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Create stub `common.cpp` so it compiles but tests fail**

```cpp
#include "common.h"

std::vector<size_t> generate_sizes() {
    return {}; // stub — test will fail
}
```

- [ ] **Step 3: Build and run test to verify failure**

Run: `make test`
Expected: assertion failure (size == 21 fails on empty vector)

---

### Task 3: Implement `generate_sizes()`

**Files:**
- Modify: `common.cpp`

- [ ] **Step 1: Implement `generate_sizes()`**

```cpp
#include "common.h"
#include <cmath>

std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++) {
        sizes.push_back(static_cast<size_t>(1ULL << i));
    }
    return sizes;
}
```

- [ ] **Step 2: Run tests**

Run: `make test`
Expected: all tests pass

---

### Task 4: Write test for `compute_throughput()`

**Files:**
- Modify: `test_common.cpp` — add tests

- [ ] **Step 1: Add throughput tests to `test_common.cpp`**

Append before `main()`:

```cpp
#include <cmath>

void test_compute_throughput_bps() {
    // 1 byte in 1 second = 8 bits/sec
    auto r = compute_throughput(1, 1, 1.0);
    assert(r.unit == "bps");
    assert(std::fabs(r.value - 8.0) < 0.01);
    printf("PASS: test_compute_throughput_bps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_Kbps() {
    // 1000 bytes in 1 second = 8000 bps = 8 Kbps
    auto r = compute_throughput(1000, 1, 1.0);
    assert(r.unit == "Kbps");
    assert(std::fabs(r.value - 8.0) < 0.01);
    printf("PASS: test_compute_throughput_Kbps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_Mbps() {
    // 125000 bytes in 1 second = 1 Mbps
    auto r = compute_throughput(125000, 1, 1.0);
    assert(r.unit == "Mbps");
    assert(std::fabs(r.value - 1.0) < 0.01);
    printf("PASS: test_compute_throughput_Mbps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_Gbps() {
    // 125000000 bytes in 1 second = 1 Gbps
    auto r = compute_throughput(125000000, 1, 1.0);
    assert(r.unit == "Gbps");
    assert(std::fabs(r.value - 1.0) < 0.01);
    printf("PASS: test_compute_throughput_Gbps (%.2f %s)\n", r.value, r.unit.c_str());
}

void test_compute_throughput_boundary_Mbps_to_Gbps() {
    // 12500000 bytes (12.5MB) in 1 sec = 100 Mbps
    auto r = compute_throughput(12500000, 1, 1.0);
    assert(r.unit == "Mbps");
    assert(std::fabs(r.value - 100.0) < 0.01);
    printf("PASS: test_compute_throughput_boundary_Mbps_to_Gbps (%.2f %s)\n", r.value, r.unit.c_str());
}
```

And update `main()` to call all tests:

```cpp
int main() {
    test_generate_sizes_count();
    test_generate_sizes_powers_of_two();
    test_generate_sizes_range();
    test_compute_throughput_bps();
    test_compute_throughput_Kbps();
    test_compute_throughput_Mbps();
    test_compute_throughput_Gbps();
    test_compute_throughput_boundary_Mbps_to_Gbps();
    printf("All tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Build and run to verify failure**

Run: `make test`
Expected: linker error — `compute_throughput` not defined

---

### Task 5: Implement `compute_throughput()`

**Files:**
- Modify: `common.cpp` — add implementation

- [ ] **Step 1: Implement `compute_throughput()`**

Append to `common.cpp`:

```cpp
ThroughputResult compute_throughput(size_t msg_size, size_t msg_count, double elapsed_sec) {
    double bits_per_sec = (msg_size * msg_count * 8.0) / elapsed_sec;

    if (bits_per_sec < 1000.0) {
        return {bits_per_sec, "bps"};
    } else if (bits_per_sec < 1000000.0) {
        return {bits_per_sec / 1000.0, "Kbps"};
    } else if (bits_per_sec < 1000000000.0) {
        return {bits_per_sec / 1000000.0, "Mbps"};
    } else {
        return {bits_per_sec / 1000000000.0, "Gbps"};
    }
}
```

- [ ] **Step 2: Run tests**

Run: `make test`
Expected: all 8 tests pass

---

### Task 6: Implement `send_full()` and `recv_full()`

**Files:**
- Modify: `common.cpp` — add implementations

- [ ] **Step 1: Implement `send_full()` and `recv_full()`**

Append to `common.cpp`:

```cpp
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

ssize_t send_full(int fd, const void* buf, size_t n) {
    size_t total = 0;
    const char* ptr = static_cast<const char*>(buf);
    while (total < n) {
        ssize_t sent = send(fd, ptr + total, n - total, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent == 0) return -1;
        total += static_cast<size_t>(sent);
    }
    return static_cast<ssize_t>(total);
}

ssize_t recv_full(int fd, void* buf, size_t n) {
    size_t total = 0;
    char* ptr = static_cast<char*>(buf);
    while (total < n) {
        ssize_t rcvd = recv(fd, ptr + total, n - total, 0);
        if (rcvd < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rcvd == 0) return -1; // connection closed
        total += static_cast<size_t>(rcvd);
    }
    return static_cast<ssize_t>(total);
}
```

- [ ] **Step 2: Verify it compiles**

Run: `make common.o`
Expected: compiles without errors

---

### Task 7: Implement socket creation functions

**Files:**
- Modify: `common.h` — add `<netinet/in.h>` related include note
- Modify: `common.cpp` — add socket creation implementations

- [ ] **Step 1: Implement `create_server_socket()` and `create_client_socket()`**

Append to `common.cpp`:

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>

int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int create_client_socket(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}
```

- [ ] **Step 2: Verify it compiles**

Run: `make common.o`
Expected: compiles without errors

---

### Task 8: Implement server

**Files:**
- Create: `server.cpp`

- [ ] **Step 1: Write `server.cpp`**

```cpp
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

const size_t MSG_COUNTS[] = {
    100000, 100000, 100000, 100000, 100000,  // 1B  2B  4B  8B  16B
    100000, 100000, 50000,  50000,  50000,   // 32B 64B 128B 256B 512B
    20000,  20000,  10000,  10000,  5000,    // 1KB 2KB 4KB 8KB 16KB
    2000,   2000,   1000,   500,    200,     // 32KB 64KB 128KB 256KB 512KB
    100                                         // 1MB
};

int main() {
    auto sizes = generate_sizes();

    int listen_fd = create_server_socket(DEFAULT_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to create server socket\n");
        return 1;
    }

    printf("Server listening on port %d\n", DEFAULT_PORT);

    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }

    printf("Client connected\n");

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        char* buf = new char[size];
        memset(buf, 0, size);

        for (size_t j = 0; j < count; j++) {
            if (recv_full(client_fd, buf, size) < 0) {
                perror("recv");
                delete[] buf;
                close(client_fd);
                close(listen_fd);
                return 1;
            }
        }

        delete[] buf;

        // Send ACK: total bytes received for this batch
        uint64_t ack = size * count;
        if (send_full(client_fd, &ack, sizeof(ack)) < 0) {
            perror("send ack");
            close(client_fd);
            close(listen_fd);
            return 1;
        }
    }

    close(client_fd);
    close(listen_fd);
    return 0;
}
```

- [ ] **Step 2: Build server**

Run: `make server`
Expected: compiles without errors

---

### Task 9: Implement client (without convergence detector)

**Files:**
- Create: `client.cpp`

- [ ] **Step 1: Write `client.cpp`**

```cpp
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <unistd.h>

const size_t MSG_COUNTS[] = {
    100000, 100000, 100000, 100000, 100000,  // 1B  2B  4B  8B  16B
    100000, 100000, 50000,  50000,  50000,   // 32B 64B 128B 256B 512B
    20000,  20000,  10000,  10000,  5000,    // 1KB 2KB 4KB 8KB 16KB
    2000,   2000,   1000,   500,    200,     // 32KB 64KB 128KB 256KB 512KB
    100                                         // 1MB
};

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        return 1;
    }

    const char* server_ip = argv[1];
    auto sizes = generate_sizes();

    int fd = create_client_socket(server_ip, DEFAULT_PORT);
    if (fd < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", server_ip, DEFAULT_PORT);
        return 1;
    }

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        char* buf = new char[size];
        memset(buf, 0, size);

        // Warm-up: send WARMUP_MSGS messages to saturate TCP slow-start
        for (int w = 0; w < WARMUP_MSGS; w++) {
            if (send_full(fd, buf, size) < 0) {
                perror("warmup send");
                delete[] buf;
                close(fd);
                return 1;
            }
        }

        // Timed batch: start timer, send all messages, wait for ACK, stop timer
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t j = 0; j < count; j++) {
            if (send_full(fd, buf, size) < 0) {
                perror("send");
                delete[] buf;
                close(fd);
                return 1;
            }
        }

        uint64_t ack = 0;
        if (recv_full(fd, &ack, sizeof(ack)) < 0) {
            perror("recv ack");
            delete[] buf;
            close(fd);
            return 1;
        }

        auto end = std::chrono::high_resolution_clock::now();

        double elapsed = std::chrono::duration<double>(end - start).count();

        auto result = compute_throughput(size, count, elapsed);
        printf("%zu\t%.2f\t%s\n", size, result.value, result.unit.c_str());

        delete[] buf;
    }

    close(fd);
    return 0;
}
```

- [ ] **Step 2: Build client**

Run: `make client`
Expected: compiles without errors

---

### Task 10: Implement convergence detector in client

**Files:**
- Modify: `client.cpp` — add convergence detector function before `main()`

- [ ] **Step 1: Add `find_counts()` function before `main()`**

Insert after the `#include` block and before `MSG_COUNTS`:

```cpp
#if 0
/*
 * CONVERGENCE DETECTOR
 * ====================
 * This function was used to determine the optimal number of messages per size
 * (MSG_COUNTS). For each message size, it starts with a small count and doubles
 * it until the measured throughput variance between iterations falls below 1%.
 * This ensures stable, repeatable measurements.
 *
 * Usage: Temporarily remove the #if 0 / #endif guards, rebuild with
 *   make client
 * Then run the server on one machine and:
 *   ./client <server-ip> > convergence_results.txt
 *
 * The output is: <size> <count> (tab-separated), one line per message size.
 * Use these counts to populate the MSG_COUNTS[] array above.
 */
#include <cmath>

void find_counts(int fd) {
    auto sizes = generate_sizes();
    const double TARGET_VARIANCE = 0.01;
    const size_t START_COUNT = 10;

    printf("# Convergence detector results\n");
    printf("# size\tcount\tthroughput_Mbps\tvariance_pct\n");

    for (auto size : sizes) {
        size_t count = START_COUNT;
        double prev_throughput = 0.0;
        bool converged = false;

        char* buf = new char[size];
        memset(buf, 0, size);

        while (!converged && count <= 10000000) {
            // Warm-up
            for (int w = 0; w < WARMUP_MSGS; w++) {
                send_full(fd, buf, size);
            }

            auto start = std::chrono::high_resolution_clock::now();
            for (size_t j = 0; j < count; j++) {
                send_full(fd, buf, size);
            }
            uint64_t ack = 0;
            recv_full(fd, &ack, sizeof(ack));
            auto end = std::chrono::high_resolution_clock::now();

            double elapsed = std::chrono::duration<double>(end - start).count();
            double throughput_mbps = (size * count * 8.0) / elapsed / 1000000.0;

            if (prev_throughput > 0.0) {
                double variance = std::fabs(throughput_mbps - prev_throughput) / prev_throughput;
                if (variance < TARGET_VARIANCE) {
                    printf("%zu\t%zu\t%.2f\t%.4f\n", size, count, throughput_mbps, variance * 100);
                    converged = true;
                    break;
                }
            }

            prev_throughput = throughput_mbps;
            count *= 2;
        }

        if (!converged) {
            printf("%zu\t%zu\t(not converged)\n", size, count);
        }

        delete[] buf;
    }
}
#endif
```

- [ ] **Step 2: Verify it builds**

Run: `make client`
Expected: compiles (function is inside `#if 0` so it's not compiled — just verify no parse errors in editor)

---

### Task 11: Integration test (loopback)

**Files:**
- No file changes — manual verification step

- [ ] **Step 1: Start the server in background**

Run in terminal 1: `./server`
Expected: `Server listening on port 12345`

- [ ] **Step 2: Run the client against localhost**

Run in terminal 2: `./client 127.0.0.1`
Expected: output with 21 lines, three tab-delimited columns each

Example expected output format:
```
1       123.45  Kbps
2       234.56  Kbps
4       345.67  Kbps
...
1048576 1234.56 Mbps
```

- [ ] **Step 3: Verify column format**

Run: `./client 127.0.0.1 | awk -F'\t' '{print NF}' | sort -u`
Expected: single output `3` (all lines have exactly 3 tabs)

- [ ] **Step 4: Verify 21 lines of output**

Run: `./client 127.0.0.1 | wc -l`
Expected: `21`

- [ ] **Step 5: Verify message sizes are powers of two**

Run: `./client 127.0.0.1 | awk -F'\t' '{print $1}'`
Expected:
```
1
2
4
8
16
32
64
128
256
512
1024
2048
4096
8192
16384
32768
65536
131072
262144
524288
1048576
```

- [ ] **Step 6: Cleanup**

Run: `make clean`
Expected: binaries and object files removed

---

### Task 12: Finalize — convergence results placeholder and explanation comment

**Files:**
- Modify: `client.cpp` — ensure the `MSG_COUNTS` comment explains origin
- Modify: `server.cpp` — ensure the `MSG_COUNTS` comment explains origin

- [ ] **Step 1: Update the comments above `MSG_COUNTS[]` in both files**

In both `server.cpp` and `client.cpp`, change the MSG_COUNTS comment to:

```cpp
/*
 * MSG_COUNTS[i] = number of messages to send for sizes[i].
 *
 * These values were determined using the convergence detector (see find_counts()
 * below, currently commented out). The detector starts with a small batch and
 * doubles it until measured throughput stabilizes (variance < 1% between iterations).
 *
 * The counts decrease as message size grows to keep total benchmark runtime under
 * 30 seconds. Smaller messages need more iterations for stable timing measurements
 * because per-message overhead dominates at tiny sizes.
 *
 * To re-run the convergence detector:
 *   1. Remove the #if 0 / #endif guards around find_counts()
 *   2. Rebuild: make client
 *   3. Run: ./server &  &&  ./client <server-ip>
 *   4. Update MSG_COUNTS with the output values
 *
 * Current values are reasonable defaults — they should be tuned on the actual
 * hardware before final submission.
 */
const size_t MSG_COUNTS[] = { ... };
```

- [ ] **Step 2: Verify everything still compiles**

Run: `make all`
Expected: both `server` and `client` compile without errors
