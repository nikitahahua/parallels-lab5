#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <map>
#include <algorithm>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#define PORT 8080
#define MAX_ARRAY_SIZE 100000
#define BUFFER_SIZE 1024

std::atomic<bool> running(true);


void error(const char* msg) {
    std::cerr << msg << ": " << strerror(errno) << std::endl;
    exit(EXIT_FAILURE);
}

void double_to_network(double value, uint8_t* buffer) {
    uint64_t temp;
    std::memcpy(&temp, &value, sizeof(double));
    for (int i = 0; i < 8; ++i) {
        buffer[i] = (temp >> (56 - i * 8)) & 0xFF;
    }
}

std::map<int32_t, int32_t> countFrequenciesInRange(const std::vector<int32_t>& numbers, size_t start, size_t end) {
    std::map<int32_t, int32_t> frequencyMap;
    for (size_t i = start; i < end && i < numbers.size(); ++i) {
        frequencyMap[numbers[i]]++;
    }
    return frequencyMap;
}

std::vector<int32_t> findMode(const std::map<int32_t, int32_t>& frequencyMap) {
    int32_t maxFrequency = 0;
    for (const auto& pair : frequencyMap) {
        maxFrequency = std::max(maxFrequency, pair.second);
    }

    std::vector<int32_t> mode;
    for (const auto& pair : frequencyMap) {
        if (pair.second == maxFrequency) {
            mode.push_back(pair.first);
        }
    }
    return mode;
}

double findMedian(const std::map<int32_t, int32_t>& frequencyMap, size_t size) {
    std::vector<int32_t> sortedNumbers;
    for (const auto& pair : frequencyMap) {
        for (int32_t i = 0; i < pair.second; ++i) {
            sortedNumbers.push_back(pair.first);
        }
    }
    std::sort(sortedNumbers.begin(), sortedNumbers.end());

    if (size % 2 == 1) {
        return static_cast<double>(sortedNumbers[size / 2]);
    } else {
        return (static_cast<double>(sortedNumbers[size / 2]) + sortedNumbers[size / 2 - 1]) / 2.0;
    }
}

struct Result {
    std::vector<int32_t> mode;
    double median;
};

Result processArray(const std::vector<int32_t>& numbers, uint32_t threadCount) {
    std::vector<std::thread> threads;
    std::vector<std::map<int32_t, int32_t> > frequencyMaps(threadCount);

    size_t segmentSize = numbers.size() / threadCount;

    for (uint32_t i = 0; i < threadCount; ++i) {
        size_t start = i * segmentSize;
        size_t end = (i == threadCount - 1) ? numbers.size() : start + segmentSize;
        threads.emplace_back([&frequencyMaps, &numbers, start, end, i]() {
            frequencyMaps[i] = countFrequenciesInRange(numbers, start, end);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    std::map<int32_t, int32_t> frequencyMap;
    for (const auto& freq : frequencyMaps) {
        for (const auto& pair : freq) {
            frequencyMap[pair.first] += pair.second;
        }
    }

    Result result;
    result.mode = findMode(frequencyMap);
    result.median = findMedian(frequencyMap, numbers.size());

    return result;
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

void handleClient(int client_socket, sockaddr_in client_addr) {
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "Connected client: " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

    std::vector<std::vector<int32_t> > arrays;
    std::vector<uint32_t> threadCounts;
    bool data_received = false;
    std::vector<Result> results;
    bool computation_done = false;

    while (running) {
        uint8_t msg_type;
        if (receive_exact(client_socket, &msg_type, sizeof(msg_type)) <= 0) {
            std::cerr << "Client " << client_ip << ":" << ntohs(client_addr.sin_port) << " disconnected" << std::endl;
            break;
        }

        if (msg_type == 0x01) {
            uint32_t array_size, num_threads;
            if (receive_exact(client_socket, &array_size, sizeof(array_size)) <= 0)
                error("Failed to receive array size");
            if (receive_exact(client_socket, &num_threads, sizeof(num_threads)) <= 0)
                error("Failed to receive num threads");

            array_size = ntohl(array_size);
            num_threads = ntohl(num_threads);

            if (array_size > MAX_ARRAY_SIZE || array_size == 0) {
                std::cerr << "Invalid array size from client " << client_ip << std::endl;
                break;
            }

            std::vector<int32_t> array(array_size);
            size_t array_bytes = array_size * sizeof(int32_t);
            if (receive_exact(client_socket, array.data(), array_bytes) <= 0)
                error("Failed to receive array");

            for (uint32_t i = 0; i < array_size; ++i) {
                array[i] = ntohl(array[i]);
            }

            arrays.push_back(array);
            threadCounts.push_back(num_threads);
            data_received = true;
            std::cout << "Received array of size " << array_size
                      << ", threads: " << num_threads << " from client " << client_ip << std::endl;
        }
        else if (msg_type == 0x02) {
            uint8_t status = data_received ? 0x00 : 0x01;
            if (send(client_socket, &status, sizeof(status), 0) < 0)
                error("Failed to send status");

            if (data_received) {
                results.clear();
                for (size_t i = 0; i < arrays.size(); ++i) {
                    results.push_back(processArray(arrays[i], threadCounts[i]));
                }
                computation_done = true;
                std::cout << "Computed results for " << arrays.size() << " arrays for client " << client_ip << std::endl;
            }
        }
        else if (msg_type == 0x03) {
            uint8_t status = computation_done ? 0x00 : (data_received ? 0x01 : 0x02);
            if (send(client_socket, &status, sizeof(status), 0) < 0)
                error("Failed to send status");

            if (status == 0x00) {
                uint32_t num_results = htonl(results.size());
                if (send(client_socket, &num_results, sizeof(num_results), 0) < 0)
                    error("Failed to send number of results");

                for (const auto& result : results) {
                    uint32_t mode_size = htonl(result.mode.size());
                    if (send(client_socket, &mode_size, sizeof(mode_size), 0) < 0)
                        error("Failed to send mode size");

                    std::vector<int32_t> net_mode = result.mode;
                    for (auto& val : net_mode) {
                        val = htonl(val);
                    }
                    if (send(client_socket, net_mode.data(), net_mode.size() * sizeof(int32_t), 0) < 0)
                        error("Failed to send mode");

                    uint8_t median_buffer[8];
                    double_to_network(result.median, median_buffer);
                    if (send(client_socket, median_buffer, sizeof(median_buffer), 0) < 0)
                        error("Failed to send median");
                }
            }
        }
        else {
            std::cerr << "Unknown message type: " << (int)msg_type << " from client " << client_ip << std::endl;
            break;
        }
    }

    close(client_socket);
    std::cout << "Closed connection with client " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;
}

void console_thread() {
    while (running) {
        char input = std::cin.get();
        if (input == 'q' || input == 'Q') {
            std::cout << "Received shutdown command, shutting down server..." << std::endl;
            running = false;
            break;
        }
    }
}

int main() {

    std::thread console(console_thread);
    console.detach();

    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    std::vector<std::thread> client_threads;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) error("Socket creation failed");

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
        error("Bind failed");

    if (listen(server_fd, 5) < 0) error("Listen failed");
    std::cout << "Server listening on port " << PORT << "..." << std::endl;
    std::cout << "Press 'q' to shutdown the server" << std::endl;

    while (running) {
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket < 0) {
            if (!running) break;
            std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            continue;
        }

        client_threads.emplace_back([client_socket, client_addr]() {
            handleClient(client_socket, client_addr);
        });
        client_threads.back().detach();
    }

    close(server_fd);
    return 0;
}