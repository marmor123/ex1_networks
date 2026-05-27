#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

static_assert(sizeof(MSG_COUNTS) / sizeof(MSG_COUNTS[0]) == 21,
              "MSG_COUNTS must have 21 entries (one per message size)");

#if 0
/*
 * CONVERGENCE DETECTOR
 * ====================
 * This function was used to determine the optimal number of messages per size
 * (MSG_COUNTS). For each message size, it starts with a small count and doubles
 * it until the measured throughput variance between iterations falls below 1%.
 *
 * IMPORTANT: This function uses a per-size convergence loop that is NOT
 * compatible with the standard server protocol. Before using, the server must
 * be modified to handle variable message counts per iteration, or the client
 * must reconnect for each tested count.
 *
 * Output: <size> <count> (tab-separated), one line per message size.
 * Use these counts to populate MSG_COUNTS[] in common.cpp.
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
            auto start = std::chrono::high_resolution_clock::now();
            for (size_t j = 0; j < count; j++) {
                if (send_full(fd, buf, size) < 0) {
                    perror("send");
                    delete[] buf;
                    return;
                }
            }
            uint64_t ack = 0;
            if (recv_full(fd, &ack, sizeof(ack)) < 0) {
                perror("recv ack");
                delete[] buf;
                return;
            }
            auto end = std::chrono::high_resolution_clock::now();

            double elapsed = std::chrono::duration<double>(end - start).count();
            double throughput_mbps = (static_cast<double>(size) * count * 8.0)
                                     / elapsed / 1000000.0;

            if (prev_throughput > 0.0) {
                double variance = std::fabs(throughput_mbps - prev_throughput)
                                  / prev_throughput;
                if (variance < TARGET_VARIANCE) {
                    printf("%zu\t%zu\t%.2f\t%.4f\n",
                           size, count, throughput_mbps, variance * 100);
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

    // One-time warmup: send large messages to saturate TCP slow-start window.
    // The server receives and discards these before the timed batches begin.
    char* warmup_buf = new char[WARMUP_SIZE];
    memset(warmup_buf, 0, WARMUP_SIZE);
    for (int w = 0; w < WARMUP_COUNT; w++) {
        if (send_full(fd, warmup_buf, WARMUP_SIZE) < 0) {
            perror("warmup send");
            delete[] warmup_buf;
            close(fd);
            return 1;
        }
    }
    delete[] warmup_buf;

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        char* buf = new char[size];
        memset(buf, 0, size);

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
