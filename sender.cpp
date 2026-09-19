#include "protocol.h"
#include "utils.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct ChunkState {
    bool sent = false;
    bool acked = false;
    uint64_t last_sent_ms = 0;
    int retry_count = 0;
};

bool setSocketTimeout(int sockfd, uint32_t timeout_ms) {
    struct timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        perror("setsockopt SO_RCVTIMEO");
        return false;
    }
    return true;
}

bool sendDatagram(int sockfd, const sockaddr_in &dest, const std::vector<uint8_t> &buf) {
    if (buf.empty())
        return false;
    ssize_t sent = sendto(sockfd, buf.data(), static_cast<size_t>(buf.size()), 0,
                          reinterpret_cast<const sockaddr *>(&dest), sizeof(dest));
    return sent == static_cast<ssize_t>(buf.size());
}

bool receivePacket(int sockfd, std::vector<uint8_t> &buf, sockaddr_in &src, socklen_t &srclen) {
    buf.assign(MAX_PACKET_SIZE, 0);
    ssize_t n = recvfrom(sockfd, buf.data(), buf.size(), 0,
                         reinterpret_cast<sockaddr *>(&src), &srclen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        perror("recvfrom");
        return false;
    }
    if (n == 0) {
        return false;
    }
    buf.resize(static_cast<size_t>(n));
    return true;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "usage: sender <receiver_ip> <port> <file_path>\n";
        return 1;
    }

    const std::string receiver_ip = argv[1];
    const int port = std::stoi(argv[2]);
    const std::string filepath = argv[3];

    std::ifstream input(filepath, std::ios::binary);
    if (!input) {
        std::cerr << "failed to open file: " << filepath << "\n";
        return 1;
    }

    auto chunks = readFileChunks(filepath, CHUNK_SIZE);
    const uint32_t total_chunks = static_cast<uint32_t>(chunks.size());
    std::string filename = filepath.substr(filepath.find_last_of("/\\") + 1);
    uint32_t file_crc = 0xFFFFFFFF;
    for (const auto &chunk : chunks) {
        file_crc = crc32Update(file_crc, chunk);
    }
    file_crc ^= 0xFFFFFFFF;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    if (!setSocketTimeout(sockfd, TIMEOUT_MS_DEFAULT)) {
        close(sockfd);
        return 1;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, receiver_ip.c_str(), &dest.sin_addr) != 1) {
        std::cerr << "invalid receiver IP: " << receiver_ip << "\n";
        close(sockfd);
        return 1;
    }

    bool hello_acked = false;
    for (int attempt = 0; attempt < MAX_RETRIES && !hello_acked; ++attempt) {
        Packet hello = makeHelloPacket(filename, total_chunks, file_crc);
        auto hello_buf = serializePacket(hello);
        if (hello_buf.empty()) {
            std::cerr << "failed to serialize HELLO\n";
            close(sockfd);
            return 1;
        }
        if (!sendDatagram(sockfd, dest, hello_buf)) {
            std::cerr << "sendto HELLO failed\n";
            close(sockfd);
            return 1;
        }
        std::cout << "HELLO sent: file=" << filename << ", chunks=" << total_chunks << "\n";

        std::vector<uint8_t> buf;
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        if (receivePacket(sockfd, buf, src, srclen)) {
            Packet pkt;
            if (deserializePacket(buf, pkt) && pkt.header.type == static_cast<uint8_t>(PacketType::HELLO_ACK)) {
                hello_acked = true;
                std::cout << "HELLO_ACK received\n";
            }
        }
    }

    if (!hello_acked) {
        std::cerr << "Failed to receive HELLO_ACK\n";
        close(sockfd);
        return 1;
    }

    std::vector<ChunkState> states(total_chunks, ChunkState{});
    bool transfer_complete = false;
    while (!transfer_complete) {
        uint64_t now = currentTimeMs();
        uint32_t in_flight = 0;
        for (uint32_t i = 0; i < total_chunks; ++i) {
            if (states[i].acked) {
                continue;
            }
            if (!states[i].sent) {
                if (in_flight >= DEFAULT_WINDOW) {
                    break;
                }
                const auto &payload = chunks[i];
                Packet pkt = makeDataPacket(i, total_chunks, payload, crc32(payload));
                auto packet_buf = serializePacket(pkt);
                if (packet_buf.empty()) {
                    std::cerr << "failed to serialize DATA packet for seq " << i << "\n";
                    close(sockfd);
                    return 1;
                }
                if (!sendDatagram(sockfd, dest, packet_buf)) {
                    std::cerr << "sendto DATA failed for seq " << i << "\n";
                    close(sockfd);
                    return 1;
                }
                states[i].sent = true;
                states[i].last_sent_ms = now;
                ++in_flight;
                std::cout << "sent chunk " << i << "\n";
                continue;
            }

            if (now - states[i].last_sent_ms >= TIMEOUT_MS_DEFAULT) {
                if (states[i].retry_count >= MAX_RETRIES) {
                    std::cerr << "DATA retransmission limit exceeded for chunk " << i << "\n";
                    close(sockfd);
                    return 1;
                }
                const auto &payload = chunks[i];
                Packet pkt = makeDataPacket(i, total_chunks, payload, crc32(payload));
                auto packet_buf = serializePacket(pkt);
                if (packet_buf.empty()) {
                    std::cerr << "failed to serialize retransmission for seq " << i << "\n";
                    close(sockfd);
                    return 1;
                }
                if (!sendDatagram(sockfd, dest, packet_buf)) {
                    std::cerr << "sendto retransmission failed for seq " << i << "\n";
                    close(sockfd);
                    return 1;
                }
                states[i].last_sent_ms = now;
                states[i].retry_count += 1;
                ++in_flight;
                std::cout << "retransmitted chunk " << i << " (attempt " << states[i].retry_count << ")\n";
            }
        }

        std::vector<uint8_t> ack_buf;
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        if (receivePacket(sockfd, ack_buf, src, srclen)) {
            Packet pkt;
            if (deserializePacket(ack_buf, pkt) && pkt.header.type == static_cast<uint8_t>(PacketType::ACK)) {
                const uint32_t seq = pkt.header.seq;
                if (seq < total_chunks && !states[seq].acked) {
                    states[seq].acked = true;
                    std::cout << "ACK received for chunk " << seq << "\n";
                }
            }
        }

        bool all_acked = true;
        for (uint32_t i = 0; i < total_chunks; ++i) {
            if (!states[i].acked) {
                all_acked = false;
                break;
            }
        }
        if (all_acked) {
            transfer_complete = true;
        }
    }

    bool fin_acked = false;
    for (int attempt = 0; attempt < MAX_RETRIES && !fin_acked; ++attempt) {
        Packet fin = makeFinPacket(file_crc);
        auto fin_buf = serializePacket(fin);
        if (fin_buf.empty()) {
            std::cerr << "failed to serialize FIN\n";
            close(sockfd);
            return 1;
        }
        if (!sendDatagram(sockfd, dest, fin_buf)) {
            std::cerr << "sendto FIN failed\n";
            close(sockfd);
            return 1;
        }
        std::cout << "FIN sent\n";

        std::vector<uint8_t> buf;
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        if (receivePacket(sockfd, buf, src, srclen)) {
            Packet pkt;
            if (deserializePacket(buf, pkt) && pkt.header.type == static_cast<uint8_t>(PacketType::FIN_ACK)) {
                fin_acked = true;
                std::cout << "FIN_ACK received\n";
            }
        }
    }

    if (!fin_acked) {
        std::cerr << "Failed to receive FIN_ACK\n";
    }

    close(sockfd);
    return fin_acked ? 0 : 1;
}