#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

constexpr int DEFAULT_PORT = 12345;
constexpr int LISTEN_BACKLOG = 5;
constexpr size_t WARMUP_SIZE = 1048576;   // 1 MB — one-time warmup message size
constexpr int WARMUP_COUNT = 8;           // warmup messages to saturate TCP slow-start

std::vector<size_t> generate_sizes();

inline const size_t MSG_COUNTS[] = {
    100000, 100000, 100000, 100000, 100000,  // 1B  2B  4B  8B  16B
    100000, 100000, 50000,  50000,  50000,   // 32B 64B 128B 256B 512B
    20000,  20000,  10000,  10000,  5000,    // 1KB 2KB 4KB 8KB 16KB
    2000,   2000,   1000,   500,    200,     // 32KB 64KB 128KB 256KB 512KB
    100                                         // 1MB
};

struct ThroughputResult {
    double value;
    std::string unit;
};
ThroughputResult compute_throughput(size_t msg_size, size_t msg_count, double elapsed_sec);

ssize_t send_full(int fd, const void* buf, size_t n);
ssize_t recv_full(int fd, void* buf, size_t n);

int create_server_socket(int port);
int create_client_socket(const char* ip, int port);
