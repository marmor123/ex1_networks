#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

// ---- shared definitions (duplicated — FirstDraft is standalone) ----

std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++)
        sizes.push_back(static_cast<size_t>(1ULL << i));
    return sizes;
}

constexpr int DEFAULT_PORT   = 12345;
constexpr int WARMUP_MSGS    = 100;
constexpr int LISTEN_BACKLOG = 5;

// Converged via convergence_detector (variance < 1 % between doubled counts)
inline const size_t MSG_COUNTS[] = {
    1310720, 81920,  655360, 163840, 327680, // 1B  2B  4B  8B  16B
    20480,   81920,  81920,  40960,  20480,   // 32B 64B 128B 256B 512B
    20480,   20480,  20480,  2560,   2560,    // 1KB 2KB 4KB 8KB 16KB
    2560,    640,    320,    160,    160,     // 32KB 64KB 128KB 256KB 512KB
    80                                          // 1MB
};

// ---- inline helpers ----

// Drain exactly n bytes from fd into buf.
// Handles partial reads, EINTR retry, and EOF → -1.
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

// ---- main ----

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(DEFAULT_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr),
             sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    printf("Server listening on port %d\n", DEFAULT_PORT);

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
        perror("accept"); close(server_fd); return 1;
    }
    printf("Client connected\n");

    // Disable Nagle + max out receive buffer for the measurement path
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // One buffer for all iterations — 1 MB ceiling
    const size_t BUF_SZ = 1ULL << 20;
    char* buf = new char[BUF_SZ];

    auto sizes = generate_sizes();
    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size  = sizes[i];
        size_t count = MSG_COUNTS[i];

        // Batched warmup recv — drain 100 messages in a single pass
        size_t remaining = static_cast<size_t>(WARMUP_MSGS) * size;
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
            if (recv_all(client_fd, buf, chunk) < 0) {
                perror("warmup recv");
                delete[] buf; close(client_fd); close(server_fd);
                return 1;
            }
            remaining -= chunk;
        }

        // Batched timed recv — drain ALL timed bytes in big gulps (Opt A)
        // Each chunk ≤ BUF_SZ, so the 1 MB buffer is never overflowed.
        // On localhost the kernel buffer is already full → 1–2 recv calls.
        remaining = count * size;
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
            if (recv_all(client_fd, buf, chunk) < 0) {
                perror("recv");
                delete[] buf; close(client_fd); close(server_fd);
                return 1;
            }
            remaining -= chunk;
        }

        // ACK — 8 bytes, always fits in one segment with TCP_NODELAY
        uint64_t ack = 0;
        if (send(client_fd, &ack, sizeof(ack), MSG_NOSIGNAL) < 0) {
            perror("send ack");
            delete[] buf; close(client_fd); close(server_fd);
            return 1;
        }
    }

    delete[] buf;
    close(client_fd);
    close(server_fd);
    return 0;
}
