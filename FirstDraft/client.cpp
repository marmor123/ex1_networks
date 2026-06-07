#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>

// ---- shared definitions (duplicated — FirstDraft is standalone) ----

std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++)
        sizes.push_back(static_cast<size_t>(1ULL << i));
    return sizes;
}

constexpr int DEFAULT_PORT = 12345;
// Optimal warmup counts per size — from warmup_probe (variance < 1%)
inline const size_t WARMUP_COUNTS[] = {
    16, 4,  4,  32, 4,   // 1B  2B  4B  8B  16B
    4,  4,  4,  4,  4,   // 32B 64B 128B 256B 512B
    4,  4,  4,  4,  4,   // 1KB 2KB 4KB 8KB 16KB
    4,  4,  4,  4,  4,   // 32KB 64KB 128KB 256KB 512KB
    4                      // 1MB
};

// Converged via convergence_detector (variance < 1 % between doubled counts)
inline const size_t MSG_COUNTS[] = {
    1310720, 81920,  655360, 163840, 327680, // 1B  2B  4B  8B  16B
    20480,   81920,  81920,  40960,  20480,   // 32B 64B 128B 256B 512B
    20480,   20480,  20480,  2560,   2560,    // 1KB 2KB 4KB 8KB 16KB
    2560,    640,    320,    160,    160,     // 32KB 64KB 128KB 256KB 512KB
    80                                          // 1MB
};

struct ThroughputResult {
    double      value;
    std::string unit;
};

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

// ---- inline helpers ----

// Send exactly n bytes.  Handles partial writes and EINTR retry.
static inline ssize_t send_all(int fd, const void* buf, size_t n) {
    size_t total = 0;
    const char* ptr = static_cast<const char*>(buf);
    while (total < n) {
        ssize_t s = send(fd, ptr + total, n - total, MSG_NOSIGNAL);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(s);
    }
    return static_cast<ssize_t>(total);
}

// Receive exactly n bytes.  Handles partial reads, EINTR, and EOF.
static inline ssize_t recv_all(int fd, void* buf, size_t n) {
    size_t total = 0;
    char*  ptr   = static_cast<char*>(buf);
    while (total < n) {
        ssize_t r = recv(fd, ptr + total, n - total, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        total += static_cast<size_t>(r);
    }
    return static_cast<ssize_t>(total);
}

// ---- main ----

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        return 1;
    }
    const char* server_ip = argv[1];

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) { perror("socket"); return 1; }

    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Large socket buffers so send() rarely blocks during the timed loop
    int sndbuf = 16 * 1024 * 1024;
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(DEFAULT_PORT);

    int pton_ret = inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    if (pton_ret == 0) {
        fprintf(stderr, "inet_pton: invalid address '%s'\n", server_ip);
        close(client_fd); return 1;
    }
    if (pton_ret < 0) {
        perror("inet_pton"); close(client_fd); return 1;
    }

    if (connect(client_fd, reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        perror("connect"); close(client_fd); return 1;
    }

    // One buffer for all iterations — 1 MB ceiling
    const size_t BUF_SZ = 1ULL << 20;
    char* buf = new char[BUF_SZ];

    auto sizes = generate_sizes();
    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size  = sizes[i];
        size_t count = MSG_COUNTS[i];
        memset(buf, 0, size);

        // Batched warmup send — satuate the TCP window in one pass
        size_t remaining = WARMUP_COUNTS[i] * size;
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
            if (send_all(client_fd, buf, chunk) < 0) {
                perror("warmup send");
                delete[] buf; close(client_fd); return 1;
            }
            remaining -= chunk;
        }

        // ---- timed batch ----
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t j = 0; j < count; j++) {
            // Per-message send with partial-write handling baked in.
            // The inline loop adds ~1 check per message (the loop usually
            // runs once when send() completes in a single call).
            size_t total = 0;
            while (total < size) {
                ssize_t s = send(client_fd, buf + total, size - total,
                                 MSG_NOSIGNAL);
                if (s < 0) {
                    if (errno == EINTR) continue;
                    perror("send");
                    delete[] buf; close(client_fd); return 1;
                }
                total += static_cast<size_t>(s);
            }
        }

        uint64_t ack = 0;
        if (recv_all(client_fd, &ack, sizeof(ack)) < 0) {
            perror("recv ack");
            delete[] buf; close(client_fd); return 1;
        }

        auto end = std::chrono::high_resolution_clock::now();
        // ---------------------

        double elapsed = std::chrono::duration<double>(end - start).count();
        auto   result  = compute_throughput(size, count, elapsed);
        printf("%zu\t%.2f\t%s\n", size, result.value, result.unit.c_str());
    }

    delete[] buf;
    close(client_fd);
    return 0;
}
