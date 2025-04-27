#include <iostream>
#include <cstring>
#include <vector>
#include <random>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"
#define MAX_ARRAY_SIZE 100000

void error(const char* msg) {
    std::cerr << msg << ": " << strerror(errno) << std::endl;
    exit(EXIT_FAILURE);
}

double network_to_double(const uint8_t* buffer) {
    uint64_t temp = 0;
    for (int i = 0; i < 8; ++i) {
        temp |= static_cast<uint64_t>(buffer[i]) << (56 - i * 8);
    }
    double value;
    std::memcpy(&value, &temp, sizeof(double));
    return value;
}

ssize_t receive_exact(int socket, void* buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        ssize_t bytes = recv(socket, (char*)buffer + received, length - received, 0);
        if (bytes <= 0) return bytes;
        received += bytes;
    }
    return received;
}

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) error("Socket creation failed");

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0)
        error("Invalid address");

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        error("Connection failed");

    uint32_t num_arrays;
    std::cout << "Enter number of arrays to send: ";
    std::cin >> num_arrays;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int32_t> dist(1, 100);

    for (uint32_t arr_idx = 0; arr_idx < num_arrays; ++arr_idx) {
        uint32_t array_size, num_threads;
        std::cout << "Enter array size (max " << MAX_ARRAY_SIZE << ") for array " << arr_idx + 1 << ": ";
        std::cin >> array_size;
        if (array_size > MAX_ARRAY_SIZE || array_size == 0) {
            std::cerr << "Invalid array size" << std::endl;
            close(sock);
            return 1;
        }

        std::cout << "Enter number of threads for array " << arr_idx + 1 << ": ";
        std::cin >> num_threads;

        std::vector<int32_t> array(array_size);
        std::cout << "Generated " << array_size << " random integers for array " << arr_idx + 1 << ":" << std::endl;
        for (uint32_t i = 0; i < array_size; ++i) {
            array[i] = dist(gen);
            std::cout << array[i] << " ";
        }
        std::cout << std::endl;

        uint8_t msg_type = 0x01;
        if (send(sock, &msg_type, sizeof(msg_type), 0) < 0)
            error("Failed to send message type");

        uint32_t net_array_size = htonl(array_size);
        uint32_t net_num_threads = htonl(num_threads);
        if (send(sock, &net_array_size, sizeof(net_array_size), 0) < 0)
            error("Failed to send array size");
        if (send(sock, &net_num_threads, sizeof(net_num_threads), 0) < 0)
            error("Failed to send num threads");

        for (uint32_t i = 0; i < array_size; ++i) {
            array[i] = htonl(array[i]);
        }
        if (send(sock, array.data(), array_size * sizeof(int32_t), 0) < 0)
            error("Failed to send array");
    }

    uint8_t msg_type = 0x02;
    if (send(sock, &msg_type, sizeof(msg_type), 0) < 0)
        error("Failed to send compute command");

    uint8_t status;
    if (receive_exact(sock, &status, sizeof(status)) <= 0)
        error("Failed to receive compute status");

    if (status != 0x00) {
        std::cerr << "Computation failed on server" << std::endl;
        close(sock);
        return 1;
    }

    msg_type = 0x03;
    if (send(sock, &msg_type, sizeof(msg_type), 0) < 0)
        error("Failed to send status request");

    if (receive_exact(sock, &status, sizeof(status)) <= 0)
        error("Failed to receive status");

    if (status == 0x00) {
        uint32_t num_results;
        if (receive_exact(sock, &num_results, sizeof(num_results)) <= 0)
            error("Failed to receive number of results");
        num_results = ntohl(num_results);

        for (uint32_t i = 0; i < num_results; ++i) {
            uint32_t mode_size;
            if (receive_exact(sock, &mode_size, sizeof(mode_size)) <= 0)
                error("Failed to receive mode size");
            mode_size = ntohl(mode_size);

            std::vector<int32_t> mode(mode_size);
            size_t mode_bytes = mode_size * sizeof(int32_t);
            if (receive_exact(sock, mode.data(), mode_bytes) <= 0)
                error("Failed to receive mode");
            for (uint32_t j = 0; j < mode_size; ++j) {
                mode[j] = ntohl(mode[j]);
            }

            uint8_t median_buffer[8];
            if (receive_exact(sock, median_buffer, sizeof(median_buffer)) <= 0)
                error("Failed to receive median");
            double median = network_to_double(median_buffer);

            std::cout << "Results for array " << i + 1 << ":" << std::endl;
            std::cout << "Mode: ";
            for (int32_t val : mode) {
                std::cout << val << " ";
            }
            std::cout << std::endl;
            std::cout << "Median: " << median << std::endl;
        }
    } else if (status == 0x01) {
        std::cout << "Computation in progress" << std::endl;
    } else {
        std::cerr << "Error on server" << std::endl;
    }

    close(sock);
    return 0;
}