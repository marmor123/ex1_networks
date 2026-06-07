#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <vector>
#include <netinet/tcp.h>


struct ThroughputResult {
    double value;
    std::string unit;
};

std::vector<size_t> generate_sizes() {
    std::vector<size_t> sizes;
    for (int i = 0; i <= 20; i++) {
        sizes.push_back(static_cast<size_t>(1ULL << i));
    }
    return sizes;
}

constexpr int WARMUP_MSGS = 100;          // per-size warmup messages to saturate TCP slow-start

inline const size_t MSG_COUNTS[] = {

    100000, 100000, 100000, 100000, 100000,  // 1B  2B  4B  8B  16B
    100000, 100000, 50000,  50000,  50000,   // 32B 64B 128B 256B 512B
    20000,  20000,  10000,  10000,  5000,    // 1KB 2KB 4KB 8KB 16KB
    2000,   2000,   1000,   500,    200,     // 32KB 64KB 128KB 256KB 512KB
    100,                                     // 1MB
};


ssize_t read_all(int fd, char* buf, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        ssize_t bytes = recv(fd, buf + total_read, size - total_read, 0);
        if (bytes < 0) return -1;  // שגיאה
        if (bytes == 0) return 0;  // הלקוח התנתק
        total_read += bytes;
    }
    return total_read;
}


int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (server_fd < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed" << std::endl;
        return 1;
    }

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed" << std::endl;
        return 1;
    }

    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

    if (client_fd < 0) {
        std::cerr << "Accept failed" << std::endl;
        return 1;
    }

    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    
    auto sizes = generate_sizes();
    char* buf = new char[1ULL << 20];
    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        // Receive warmup messages and discard (they exist only to open the TCP
        // congestion window before the timed batch)
        memset(buf, 0, size);
        for (int w = 0; w < WARMUP_MSGS; w++) {
            if (read_all(client_fd, buf, size) < 0) {
                perror("warmup recv");
                delete[] buf;
                close(client_fd);
                close(server_fd);
                return 1;
            }
        }

        // Receive timed messages
        if (read_all(client_fd, buf, size*count) < 0) {
            perror("recv");
            delete[] buf;
            close(client_fd);
            close(server_fd);
            return 1;
        }


        // ACK signals that all timed messages for this size have been received.
        // The value is irrelevant — the client uses it purely as a sync barrier.
        uint64_t ack = 0;
        if (send(client_fd, &ack, sizeof(ack), 0) < 0) {
            perror("send ack");
            close(client_fd);
            close(server_fd);
            return 1;
        }
    }
    delete[] buf;
  
    close(client_fd);
    close(server_fd);
    return 0;
}       