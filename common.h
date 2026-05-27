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
