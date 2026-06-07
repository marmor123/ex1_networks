// Warmup-probe server — one connection per size, all warmup levels
// for that size run on the same connection.
//
// Protocol per cycle:
//   Client → Server:  uint64_t size
//   Client → Server:  uint64_t warmup_count
//   Client → Server:  uint64_t timed_count    (0 = done with this connection)
//   Client → Server:  warmup_count * size  bytes  (batched)
//   Client → Server:  timed_count  * size  bytes  (batched)
//   Server → Client:  uint64_t ack = 0
//
// Server loops: accept → handle-cycles-until-count=0 → close → accept again.

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

constexpr int DEFAULT_PORT   = 12347;
constexpr int LISTEN_BACKLOG = 5;

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

    printf("Warmup-probe server listening on port %d\n", DEFAULT_PORT);

    const size_t BUF_SZ = 1ULL << 20;
    char* buf = new char[BUF_SZ];

    for (;;) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) { perror("accept"); continue; }

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int rcvbuf = 16 * 1024 * 1024;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        uint64_t ack = 0;

        for (;;) {
            // --- read headers ---
            uint64_t size_hdr = 0, warmup_hdr = 0, timed_hdr = 0;
            if (recv_all(client_fd, &size_hdr,   sizeof(size_hdr))   < 0 ||
                recv_all(client_fd, &warmup_hdr, sizeof(warmup_hdr)) < 0 ||
                recv_all(client_fd, &timed_hdr,  sizeof(timed_hdr))  < 0) {
                break;
            }

            if (timed_hdr == 0) break;   // done with this connection

            size_t size       = static_cast<size_t>(size_hdr);
            size_t warmup_cnt = static_cast<size_t>(warmup_hdr);
            size_t timed_cnt  = static_cast<size_t>(timed_hdr);

            // --- batched warmup recv ---
            size_t remaining = warmup_cnt * size;
            while (remaining > 0) {
                size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
                if (recv_all(client_fd, buf, chunk) < 0) {
                    perror("warmup recv"); goto close_conn;
                }
                remaining -= chunk;
            }

            // --- batched timed recv ---
            remaining = timed_cnt * size;
            while (remaining > 0) {
                size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
                if (recv_all(client_fd, buf, chunk) < 0) {
                    perror("timed recv"); goto close_conn;
                }
                remaining -= chunk;
            }

            // --- ACK ---
            if (send(client_fd, &ack, sizeof(ack), MSG_NOSIGNAL) < 0) {
                perror("send ack"); goto close_conn;
            }
        }

    close_conn:
        close(client_fd);
    }

    delete[] buf;
    close(server_fd);
    return 0;
}
