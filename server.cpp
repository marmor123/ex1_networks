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

    // One-time warmup: receive and discard to match client warmup
    char* warmup_buf = new char[WARMUP_SIZE];
    memset(warmup_buf, 0, WARMUP_SIZE);
    for (int w = 0; w < WARMUP_COUNT; w++) {
        if (recv_full(client_fd, warmup_buf, WARMUP_SIZE) < 0) {
            perror("warmup recv");
            delete[] warmup_buf;
            close(client_fd);
            close(listen_fd);
            return 1;
        }
    }
    delete[] warmup_buf;

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        char* buf = new char[size];
        memset(buf, 0, size);

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

        uint64_t ack = static_cast<uint64_t>(size) * count;
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
