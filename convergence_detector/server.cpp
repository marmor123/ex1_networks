// Convergence-detector server — handles an unbounded number of
// (warmup + timed + ACK) cycles.  The client sends a size+count header
// before each cycle so the server knows how many bytes to drain.
//
// Protocol per cycle:
//   Client → Server:  uint64_t size
//   Client → Server:  uint64_t count   (0 = done, exit loop)
//   Client → Server:  WARMUP_MSGS * size  bytes  (warmup, batched)
//   Client → Server:  count   * size  bytes  (timed, batched)
//   Server → Client:  uint64_t ack = 0

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

constexpr int DEFAULT_PORT   = 12346;
constexpr int WARMUP_MSGS    = 100;
constexpr int LISTEN_BACKLOG = 5;

// ---- inline helpers ----

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

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DEFAULT_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    printf("Convergence server listening on port %d\n", DEFAULT_PORT);

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
        perror("accept"); close(server_fd); return 1;
    }
    printf("Convergence client connected\n");

    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    const size_t BUF_SZ = 1ULL << 20;   // 1 MB
    char* buf = new char[BUF_SZ];

    for (;;) {
        // --- read size header ---
        uint64_t size_raw = 0;
        if (recv_all(client_fd, &size_raw, sizeof(size_raw)) < 0) break;
        size_t size = static_cast<size_t>(size_raw);

        // --- read count header ---
        uint64_t count_raw = 0;
        if (recv_all(client_fd, &count_raw, sizeof(count_raw)) < 0) break;
        size_t count = static_cast<size_t>(count_raw);

        if (count == 0) break;   // client signalled done

        // --- batched warmup recv ---
        size_t remaining = static_cast<size_t>(WARMUP_MSGS) * size;
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
            if (recv_all(client_fd, buf, chunk) < 0) {
                perror("warmup recv"); goto cleanup;
            }
            remaining -= chunk;
        }

        // --- batched timed recv ---
        remaining = count * size;
        while (remaining > 0) {
            size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
            if (recv_all(client_fd, buf, chunk) < 0) {
                perror("timed recv"); goto cleanup;
            }
            remaining -= chunk;
        }

        // --- ACK ---
        uint64_t ack = 0;
        if (send(client_fd, &ack, sizeof(ack), MSG_NOSIGNAL) < 0) {
            perror("send ack"); goto cleanup;
        }
    }

cleanup:
    delete[] buf;
    close(client_fd);
    close(server_fd);
    return 0;
}
