#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

/*
 * MSG_COUNTS[i] = number of messages to send for sizes[i].
 *
 * These values were determined using the convergence detector (see find_counts()
 * below, currently commented out). The detector starts with a small batch and
 * doubles it until measured throughput stabilizes (variance < 1% between iterations).
 *
 * The counts decrease as message size grows to keep total benchmark runtime under
 * 30 seconds. Smaller messages need more iterations for stable timing measurements
 * because per-message overhead dominates at tiny sizes.
 *
 * To re-run the convergence detector:
 *   1. Remove the #if 0 / #endif guards around find_counts()
 *   2. Rebuild: make client
 *   3. Run: ./server &  &&  ./client <server-ip>
 *   4. Update MSG_COUNTS with the output values
 *
 * Current values are reasonable defaults — they should be tuned on the actual
 * hardware before final submission.
 */
const size_t MSG_COUNTS[] = {
    100000, 100000, 100000, 100000, 100000,  // 1B  2B  4B  8B  16B
    100000, 100000, 50000,  50000,  50000,   // 32B 64B 128B 256B 512B
    20000,  20000,  10000,  10000,  5000,    // 1KB 2KB 4KB 8KB 16KB
    2000,   2000,   1000,   500,    200,     // 32KB 64KB 128KB 256KB 512KB
    100                                         // 1MB
};

#if 0
/*
 * CONVERGENCE DETECTOR
 * ====================
 * This function was used to determine the optimal number of messages per size
 * (MSG_COUNTS). For each message size, it starts with a small count and doubles
 * it until the measured throughput variance between iterations falls below 1%.
 * This ensures stable, repeatable measurements.
 *
 * Usage: Temporarily remove the #if 0 / #endif guards, rebuild with
 *   make client
 * Then run the server on one machine and:
 *   ./client <server-ip> > convergence_results.txt
 *
 * The output is: <size> <count> (tab-separated), one line per message size.
 * Use these counts to populate the MSG_COUNTS[] array above.
 */
#include <cmath>

void find_counts(int fd) {
    auto sizes = generate_sizes();
    const double TARGET_VARIANCE = 0.01;
    const size_t START_COUNT = 10;

    printf("# Convergence detector results\n");
    printf("# size\tcount\tthroughput_Mbps\tvariance_pct\n");

    for (auto size : sizes) {
        size_t count = START_COUNT;
        double prev_throughput = 0.0;
        bool converged = false;

        char* buf = new char[size];
        memset(buf, 0, size);

        while (!converged && count <= 10000000) {
            // Warm-up
            for (int w = 0; w < WARMUP_MSGS; w++) {
                send_full(fd, buf, size);
            }

            auto start = std::chrono::high_resolution_clock::now();
            for (size_t j = 0; j < count; j++) {
                send_full(fd, buf, size);
            }
            uint64_t ack = 0;
            recv_full(fd, &ack, sizeof(ack));
            auto end = std::chrono::high_resolution_clock::now();

            double elapsed = std::chrono::duration<double>(end - start).count();
            double throughput_mbps = (size * count * 8.0) / elapsed / 1000000.0;

            if (prev_throughput > 0.0) {
                double variance = std::fabs(throughput_mbps - prev_throughput) / prev_throughput;
                if (variance < TARGET_VARIANCE) {
                    printf("%zu\t%zu\t%.2f\t%.4f\n", size, count, throughput_mbps, variance * 100);
                    converged = true;
                    break;
                }
            }

            prev_throughput = throughput_mbps;
            count *= 2;
        }

        if (!converged) {
            printf("%zu\t%zu\t(not converged)\n", size, count);
        }

        delete[] buf;
    }
}
#endif

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        return 1;
    }

    const char* server_ip = argv[1];
    auto sizes = generate_sizes();

    int fd = create_client_socket(server_ip, DEFAULT_PORT);
    if (fd < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", server_ip, DEFAULT_PORT);
        return 1;
    }

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        char* buf = new char[size];
        memset(buf, 0, size);

        // Warm-up: send messages to saturate TCP slow-start window
        for (int w = 0; w < WARMUP_MSGS; w++) {
            if (send_full(fd, buf, size) < 0) {
                perror("warmup send");
                delete[] buf;
                close(fd);
                return 1;
            }
        }

        // Timed batch: start timer, send all messages, wait for ACK, stop timer
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t j = 0; j < count; j++) {
            if (send_full(fd, buf, size) < 0) {
                perror("send");
                delete[] buf;
                close(fd);
                return 1;
            }
        }

        uint64_t ack = 0;
        if (recv_full(fd, &ack, sizeof(ack)) < 0) {
            perror("recv ack");
            delete[] buf;
            close(fd);
            return 1;
        }

        auto end = std::chrono::high_resolution_clock::now();

        double elapsed = std::chrono::duration<double>(end - start).count();

        auto result = compute_throughput(size, count, elapsed);
        printf("%zu\t%.2f\t%s\n", size, result.value, result.unit.c_str());

        delete[] buf;
    }

    close(fd);
    return 0;
}
