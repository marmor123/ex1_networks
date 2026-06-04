#include <iostream>       // עבור הדפסות (std::cout, std::cerr)
#include <sys/socket.h>   // עבור פונקציות הסוקטים (socket, connect)
#include <netinet/in.h>   // עבור מבנה הכתובת sockaddr_in
#include <arpa/inet.h>    // עבור הפונקציה inet_pton (המרת מחרוזת ה-IP)
#include <netinet/tcp.h>
#include <unistd.h>       // עבור פונקציית close
#include <cstring>
#include <chrono>
#include <vector>

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

ThroughputResult compute_throughput(size_t msg_size, size_t msg_count, double elapsed_sec) {
    if (elapsed_sec <= 0.0) return {0.0, "bps"};
    double bits_per_sec = (static_cast<double>(msg_size) * msg_count * 8.0) / elapsed_sec;

    if (bits_per_sec < 1000.0) {
        return {bits_per_sec, "bps"};
    } else if (bits_per_sec < 1000000.0) {
        return {bits_per_sec / 1000.0, "Kbps"};
    } else if (bits_per_sec < 1000000000.0) {
        return {bits_per_sec / 1000000.0, "Mbps"};
    } else {
        return {bits_per_sec / 1000000000.0, "Gbps"};
    }
}




int main (){
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    int flag = 1;
    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        perror("setsockopt TCP_NODELAY");
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address" << std::endl;
        return 1;
    }

    if (connect(client_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Connection failed" << std::endl;
        return 1;
    }

    auto sizes = generate_sizes();
    char* buf = new char[1ULL << 20];

    for (size_t i = 0; i < sizes.size(); i++) {
        size_t size = sizes[i];
        size_t count = MSG_COUNTS[i];

        memset(buf, 0, size);

        // Warm-up: send messages to saturate TCP slow-start window.
        // The server receives and discards these before the timed batch.
        for (int w = 0; w < WARMUP_MSGS; w++) {
            if (send(client_fd, buf, size, 0) < 0) {
                perror("warmup send");
                delete[] buf;
                close(client_fd);
                return 1;
            }
        }

        // Timed batch: start timer, send all messages, wait for ACK, stop timer.
        // The timer spans the send + server processing + ACK return to measure
        // the network round-trip, not just the local send rate.
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t j = 0; j < count; j++) {
            if (send(client_fd, buf, size, 0) < 0) {
                perror("send");
                delete[] buf;
                close(client_fd);
                return 1;
            }
        }

        uint64_t ack = 0;
        if (recv(client_fd, &ack, sizeof(ack), 0) < 0) {
            perror("recv ack");
            delete[] buf;
            close(client_fd);
            return 1;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        
        double elapsed = std::chrono::duration<double>(end - start).count();
        
        auto result = compute_throughput(size, count, elapsed);
        printf("%zu\t%.2f\t%s\n", size, result.value, result.unit.c_str());
        
        
    }
    // send(client_fd, message, strlen(message), 0);
    
    // recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    

    close(client_fd);
}