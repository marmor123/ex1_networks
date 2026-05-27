#include "common.h"
#include <cmath>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#endif

std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++) {
        sizes.push_back(static_cast<size_t>(1ULL << i));
    }
    return sizes;
}

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

#ifndef _WIN32
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
        if (rcvd == 0) return -1;
        total += static_cast<size_t>(rcvd);
    }
    return static_cast<ssize_t>(total);
}

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
#endif
