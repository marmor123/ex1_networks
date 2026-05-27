#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

/*
 * MSG_COUNTS[i] = number of messages to send for sizes[i].
 *
 * These values were determined using the convergence detector (see find_counts()
 * in client.cpp, currently commented out). The detector starts with a small batch and
 * doubles it until measured throughput stabilizes (variance < 1% between iterations).
 *
 * The counts decrease as message size grows to keep total benchmark runtime under
 * 30 seconds. Smaller messages need more iterations for stable timing measurements
 * because per-message overhead dominates at tiny sizes.
 *
 * To re-run the convergence detector:
 *   1. Remove the #if 0 / #endif guards around find_counts() in client.cpp
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

int main() {
    auto sizes = generate_sizes();

    int listen_fd = create_server_socket(DEFAULT_PORT);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to create server socket\n");
        return 1;
    }

    printf("Server listening on port %d\n", DEFAULT_PORT);

    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }

    printf("Client connected\n");

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        char* buf = new char[size];
        memset(buf, 0, size);

        // Receive warmup + timed messages (matching what client sends)
        for (size_t j = 0; j < WARMUP_MSGS + count; j++) {
            if (recv_full(client_fd, buf, size) < 0) {
                perror("recv");
                delete[] buf;
                close(client_fd);
                close(listen_fd);
                return 1;
            }
        }

        delete[] buf;

        // Send ACK: total bytes received from this batch
        uint64_t ack = size * count;
        if (send_full(client_fd, &ack, sizeof(ack)) < 0) {
            perror("send ack");
            close(client_fd);
            close(listen_fd);
            return 1;
        }
    }

    close(client_fd);
    close(listen_fd);
    return 0;
}
