// Warmup-probe client — one connection per size, all warmup levels for
// that size run on the same connection (no reconnect between levels).
//
// Per-size algorithm:
//   timed_count = MSG_COUNTS[i]  (fixed — already converged)
//   Connect once.
//   warmup = 2
//   loop:
//     send headers → send warmup → timed batch → ACK
//     if variance vs previous warmup level < 1 %: converged
//     else: warmup *= 2
//   Send count=0 to end connection.  Disconnect.  Next size.
//
// Output (tab-separated):
//   size  warmup_count  throughput_Mbps  variance_pct
//
// Usage:
//   ./warmup_server &
//   ./warmup_client 127.0.0.1  > warmup_results.txt

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>

constexpr int DEFAULT_PORT     = 12347;
constexpr double TARGET_VAR    = 0.01;
constexpr size_t WARMUP_START  = 2;
constexpr size_t WARMUP_MAX    = 32768;

// Fixed timed-message counts — from convergence detector results
inline const size_t MSG_COUNTS[] = {
    1310720, 81920,  655360, 163840, 327680, // 1B  2B  4B  8B  16B
    20480,   81920,  81920,  40960,  20480,   // 32B 64B 128B 256B 512B
    20480,   20480,  20480,  2560,   2560,    // 1KB 2KB 4KB 8KB 16KB
    2560,    640,    320,    160,    160,     // 32KB 64KB 128KB 256KB 512KB
    80                                          // 1MB
};

std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++)
        sizes.push_back(static_cast<size_t>(1ULL << i));
    return sizes;
}

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

// Connect a fresh socket for this size
static int connect_one(const char* server_ip) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int sndbuf = 16 * 1024 * 1024;
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "inet_pton: invalid address '%s'\n", server_ip);
        close(fd); return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

// Run one warmup-level cycle on the already-connected fd.
// Returns elapsed seconds.
static double run_one_cycle(int fd, size_t size, size_t warmup_cnt,
                            size_t timed_cnt, char* buf, size_t buf_sz,
                            bool* ok) {
    *ok = false;

    // --- send headers ---
    uint64_t hdr_sz = static_cast<uint64_t>(size);
    uint64_t hdr_w  = static_cast<uint64_t>(warmup_cnt);
    uint64_t hdr_t  = static_cast<uint64_t>(timed_cnt);
    if (send_all(fd, &hdr_sz, 8) < 0 ||
        send_all(fd, &hdr_w,  8) < 0 ||
        send_all(fd, &hdr_t,  8) < 0) {
        perror("send header"); return 0.0;
    }

    // --- batched warmup send ---
    size_t remaining = warmup_cnt * size;
    while (remaining > 0) {
        size_t chunk = (remaining < buf_sz) ? remaining : buf_sz;
        if (send_all(fd, buf, chunk) < 0) {
            perror("warmup send"); return 0.0;
        }
        remaining -= chunk;
    }

    // --- timed batch (per-message, matching benchmark behaviour) ---
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t j = 0; j < timed_cnt; j++) {
        size_t total = 0;
        while (total < size) {
            ssize_t s = send(fd, buf + total, size - total, MSG_NOSIGNAL);
            if (s < 0) {
                if (errno == EINTR) continue;
                perror("send"); return 0.0;
            }
            total += static_cast<size_t>(s);
        }
    }

    uint64_t ack = 0;
    if (recv_all(fd, &ack, sizeof(ack)) < 0) {
        perror("recv ack"); return 0.0;
    }

    auto end = std::chrono::high_resolution_clock::now();

    *ok = true;
    return std::chrono::duration<double>(end - start).count();
}

// ---- main ----

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        return 1;
    }
    const char* server_ip = argv[1];

    auto sizes = generate_sizes();
    const size_t BUF_SZ = 1ULL << 20;
    char* buf = new char[BUF_SZ];

    printf("# Warmup-probe (single connection per size)\n");
    printf("# Target variance: %.0f%%   Start: %zu   Max: %zu\n",
           TARGET_VAR * 100, WARMUP_START, WARMUP_MAX);
    printf("# Columns: size\twarmup_count\tthroughput_Mbps\tvariance_pct\n");
    fflush(stdout);

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size      = sizes[i];
        size_t timed_cnt = MSG_COUNTS[i];

        // Connect once for all warmup levels of this size
        int fd = connect_one(server_ip);
        if (fd < 0) { delete[] buf; return 1; }

        size_t warmup    = WARMUP_START;
        double prev_tp   = 0.0;
        bool   converged = false;

        while (!converged && warmup <= WARMUP_MAX) {
            bool ok = false;
            double elapsed = run_one_cycle(fd, size, warmup, timed_cnt,
                                           buf, BUF_SZ, &ok);
            if (!ok) { close(fd); delete[] buf; return 1; }

            double tp_mbps = (static_cast<double>(size) * timed_cnt * 8.0)
                             / elapsed / 1'000'000.0;

            if (prev_tp > 0.0) {
                double var = std::fabs(tp_mbps - prev_tp) / prev_tp;

                printf("%zu\t%zu\t%.2f\t%.4f\n",
                       size, warmup, tp_mbps, var * 100.0);
                fflush(stdout);

                if (var < TARGET_VAR) { converged = true; break; }
            }

            prev_tp = tp_mbps;
            warmup *= 2;
        }

        if (!converged) {
            printf("%zu\t%zu\t(not converged — reached max warmup)\n",
                   size, warmup);
            fflush(stdout);
        }

        // Signal end-of-connection, then close
        uint64_t zero = 0;
        send_all(fd, &zero, 8);   // size  = 0
        send_all(fd, &zero, 8);   // warmup = 0
        send_all(fd, &zero, 8);   // timed = 0 → server closes connection
        close(fd);
    }

    delete[] buf;
    printf("\n# Done. Copy the warmup_count column into WARMUP_MSGS[].\n");
    return 0;
}
