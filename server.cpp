#include "common.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

static_assert(sizeof(MSG_COUNTS) / sizeof(MSG_COUNTS[0]) == 21,
              "MSG_COUNTS must have 21 entries (one per message size)");

int main() {
    auto sizes = generate_sizes();
    assert(sizes.size() == std::size(MSG_COUNTS));

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

    // Disable Nagle on accepted socket for timely ACK delivery
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        // Receive warmup messages and discard (they exist only to open the TCP
        // congestion window before the timed batch)
        char* buf = new char[size];
        memset(buf, 0, size);
        for (int w = 0; w < WARMUP_MSGS; w++) {
            if (recv_full(client_fd, buf, size) < 0) {
                perror("warmup recv");
                delete[] buf;
                close(client_fd);
                close(listen_fd);
                return 1;
            }
        }

        // Receive timed messages
        for (size_t j = 0; j < count; j++) {
            if (recv_full(client_fd, buf, size) < 0) {
                perror("recv");
                delete[] buf;
                close(client_fd);
                close(listen_fd);
                return 1;
            }
        }

        delete[] buf;

        // ACK signals that all timed messages for this size have been received.
        // The value is irrelevant — the client uses it purely as a sync barrier.
        uint64_t ack = 0;
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
