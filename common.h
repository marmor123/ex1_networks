#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <sys/types.h>

constexpr int DEFAULT_PORT = 12345;
constexpr int LISTEN_BACKLOG = 5;
// Optimal warmup counts per size — from warmup_probe (variance < 1%)
inline const size_t WARMUP_COUNTS[] = {
    16, 4,  4,  32, 4,   // 1B  2B  4B  8B  16B
    4,  4,  4,  4,  4,   // 32B 64B 128B 256B 512B
    4,  4,  4,  4,  4,   // 1KB 2KB 4KB 8KB 16KB
    4,  4,  4,  4,  4,   // 32KB 64KB 128KB 256KB 512KB
    4                      // 1MB
};

std::vector<size_t> generate_sizes();

// Converged via convergence_detector (variance < 1 % between doubled counts)
inline const size_t MSG_COUNTS[] = {
    1310720, 81920,  655360, 163840, 327680, // 1B  2B  4B  8B  16B
    20480,   81920,  81920,  40960,  20480,   // 32B 64B 128B 256B 512B
    20480,   20480,  20480,  2560,   2560,    // 1KB 2KB 4KB 8KB 16KB
    2560,    640,    320,    160,    160,     // 32KB 64KB 128KB 256KB 512KB
    80                                          // 1MB
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
