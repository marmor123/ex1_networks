// Convergence detector — finds the minimum message count per size that
// produces stable throughput measurements (variance < 1% between consecutive
// doubled counts).
//
// Output (tab-separated, written to stdout):
//   size  count  throughput_Mbps  variance_pct
//
// The last line for each size is the converged value — that's the number
// to copy into MSG_COUNTS[].
//
// Usage:
//   ./conv_server &          (or on another machine)
//   ./conv_client <server-ip>  > convergence_results.txt

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

constexpr int DEFAULT_PORT  = 12346;
constexpr int WARMUP_MSGS   = 100;

constexpr double TARGET_VARIANCE = 0.01;   // 1 %
constexpr size_t START_COUNT     = 10;
constexpr size_t MAX_COUNT       = 10'000'000;

// ---- shared helpers ----

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

// ---- main ----

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        return 1;
    }
    const char* server_ip = argv[1];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int sndbuf = 16 * 1024 * 1024;
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);

    int pton_ret = inet_pton(AF_INET, server_ip, &addr.sin_addr);
    if (pton_ret == 0) {
        fprintf(stderr, "inet_pton: invalid address '%s'\n", server_ip);
        close(fd); return 1;
    }
    if (pton_ret < 0) {
        perror("inet_pton"); close(fd); return 1;
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }

    const size_t BUF_SZ = 1ULL << 20;
    char* buf = new char[BUF_SZ];

    auto sizes = generate_sizes();

    printf("# Convergence detector — %zu message sizes\n", sizes.size());
    printf("# Target variance: %.0f%%   Start count: %zu   Max count: %zu\n",
           TARGET_VARIANCE * 100, START_COUNT, MAX_COUNT);
    printf("# Columns: size\tcount\tthroughput_Mbps\tvariance_pct\n");
    fflush(stdout);

    for (auto size : sizes) {
        size_t count         = START_COUNT;
        double prev_throughput = 0.0;
        bool   converged       = false;

        while (!converged && count <= MAX_COUNT) {

            // --- send headers (size, count) ---
            uint64_t size_hdr  = static_cast<uint64_t>(size);
            uint64_t count_hdr = static_cast<uint64_t>(count);
            if (send_all(fd, &size_hdr,  sizeof(size_hdr))  < 0 ||
                send_all(fd, &count_hdr, sizeof(count_hdr)) < 0) {
                perror("send header");
                delete[] buf; close(fd); return 1;
            }

            // --- batched warmup send ---
            size_t remaining = static_cast<size_t>(WARMUP_MSGS) * size;
            while (remaining > 0) {
                size_t chunk = (remaining < BUF_SZ) ? remaining : BUF_SZ;
                if (send_all(fd, buf, chunk) < 0) {
                    perror("warmup send");
                    delete[] buf; close(fd); return 1;
                }
                remaining -= chunk;
            }

            // --- timed batch (per-message, matching benchmark behaviour) ---
            auto start = std::chrono::high_resolution_clock::now();

            for (size_t j = 0; j < count; j++) {
                size_t total = 0;
                while (total < size) {
                    ssize_t s = send(fd, buf + total, size - total, MSG_NOSIGNAL);
                    if (s < 0) {
                        if (errno == EINTR) continue;
                        perror("send");
                        delete[] buf; close(fd); return 1;
                    }
                    total += static_cast<size_t>(s);
                }
            }

            uint64_t ack = 0;
            if (recv_all(fd, &ack, sizeof(ack)) < 0) {
                perror("recv ack");
                delete[] buf; close(fd); return 1;
            }

            auto end = std::chrono::high_resolution_clock::now();
            // -------------------------------------

            double elapsed = std::chrono::duration<double>(end - start).count();
            double throughput_mbps =
                (static_cast<double>(size) * count * 8.0) / elapsed / 1'000'000.0;

            if (prev_throughput > 0.0) {
                double variance =
                    std::fabs(throughput_mbps - prev_throughput) / prev_throughput;

                printf("%zu\t%zu\t%.2f\t%.4f\n",
                       size, count, throughput_mbps, variance * 100.0);
                fflush(stdout);

                if (variance < TARGET_VARIANCE) {
                    converged = true;
                    break;
                }
            }

            prev_throughput = throughput_mbps;
            count *= 2;
        }

        if (!converged) {
            printf("%zu\t%zu\t(not converged — reached max count)\n",
                   size, count);
            fflush(stdout);
        }
    }

    // --- signal server to exit ---
    uint64_t zero = 0;
    send_all(fd, &zero, sizeof(zero));   // size  = 0
    send_all(fd, &zero, sizeof(zero));   // count = 0  → server exits loop

    delete[] buf;
    close(fd);

    printf("\n# Done. Copy the count column above into MSG_COUNTS[].\n");
    return 0;
}
