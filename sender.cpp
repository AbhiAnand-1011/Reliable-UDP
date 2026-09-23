#include "protocol.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
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
    bool retransmitted = false;

    uint64_t last_sent_us = 0;
    int retry_count = 0;
};

struct TransferMetrics {
    uint64_t transfer_start_us = 0;
    uint64_t transfer_end_us = 0;

    uint64_t initial_data_sends = 0;
    uint64_t retransmissions = 0;
    uint64_t ack_received = 0;
    uint64_t payload_bytes = 0;

    std::vector<uint64_t> rtt_samples_us;
};

uint64_t currentTimeUs() {
    using namespace std::chrono;

    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()
        ).count()
    );
}

uint64_t percentileUs(
    std::vector<uint64_t> samples,
    double percentile
) {
    if (samples.empty()) {
        return 0;
    }

    if (percentile < 0.0 || percentile > 100.0) {
        return 0;
    }

    std::sort(samples.begin(), samples.end());

    const double position =
        (percentile / 100.0) *
        static_cast<double>(samples.size() - 1);

    const size_t index =
        static_cast<size_t>(position);

    const double fraction =
        position - static_cast<double>(index);

    if (index + 1 >= samples.size()) {
        return samples[index];
    }

    return static_cast<uint64_t>(
        samples[index] +
        fraction *
            static_cast<double>(
                samples[index + 1] - samples[index]
            )
    );
}

bool setSocketTimeout(
    int sockfd,
    uint32_t timeout_ms
) {
    struct timeval tv{};

    tv.tv_sec =
        static_cast<time_t>(timeout_ms / 1000);

    tv.tv_usec =
        static_cast<suseconds_t>(
            (timeout_ms % 1000) * 1000
        );

    if (setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &tv,
            sizeof(tv)
        ) != 0) {
        perror("setsockopt SO_RCVTIMEO");
        return false;
    }

    return true;
}

bool sendDatagram(
    int sockfd,
    const sockaddr_in &dest,
    const std::vector<uint8_t> &buf
) {
    if (buf.empty()) {
        return false;
    }

    ssize_t sent = sendto(
        sockfd,
        buf.data(),
        static_cast<size_t>(buf.size()),
        0,
        reinterpret_cast<const sockaddr *>(&dest),
        sizeof(dest)
    );

    return sent == static_cast<ssize_t>(buf.size());
}

bool receivePacket(
    int sockfd,
    std::vector<uint8_t> &buf,
    sockaddr_in &src,
    socklen_t &srclen
) {
    buf.assign(MAX_PACKET_SIZE, 0);

    ssize_t n = recvfrom(
        sockfd,
        buf.data(),
        buf.size(),
        0,
        reinterpret_cast<sockaddr *>(&src),
        &srclen
    );

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

bool receivePacketNonBlocking(
    int sockfd,
    std::vector<uint8_t> &buf,
    sockaddr_in &src,
    socklen_t &srclen
) {
    buf.assign(MAX_PACKET_SIZE, 0);
    srclen = sizeof(src);

    ssize_t n = recvfrom(
        sockfd,
        buf.data(),
        buf.size(),
        MSG_DONTWAIT,
        reinterpret_cast<sockaddr *>(&src),
        &srclen
    );

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

} 

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr
            << "usage: sender <receiver_ip> <port> <file_path>\n";
        return 1;
    }

    const std::string receiver_ip = argv[1];
    const int port = std::stoi(argv[2]);
    const std::string filepath = argv[3];

    std::ifstream input(
        filepath,
        std::ios::binary
    );

    if (!input) {
        std::cerr
            << "failed to open file: "
            << filepath
            << "\n";
        return 1;
    }

    auto chunks =
        readFileChunks(filepath, CHUNK_SIZE);

    const uint32_t total_chunks =
        static_cast<uint32_t>(chunks.size());

    std::string filename =
        filepath.substr(
            filepath.find_last_of("/\\") + 1
        );

    uint32_t file_crc = 0xFFFFFFFF;

    for (const auto &chunk : chunks) {
        file_crc =
            crc32Update(
                file_crc,
                chunk
            );
    }

    file_crc ^= 0xFFFFFFFF;

    int sockfd =
        socket(
            AF_INET,
            SOCK_DGRAM,
            0
        );

    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    if (!setSocketTimeout(
            sockfd,
            TIMEOUT_MS_DEFAULT
        )) {
        close(sockfd);
        return 1;
    }

    sockaddr_in dest{};

    dest.sin_family =
        AF_INET;

    dest.sin_port =
        htons(
            static_cast<uint16_t>(port)
        );

    if (inet_pton(
            AF_INET,
            receiver_ip.c_str(),
            &dest.sin_addr
        ) != 1) {
        std::cerr
            << "invalid receiver IP: "
            << receiver_ip
            << "\n";

        close(sockfd);
        return 1;
    }

    bool hello_acked = false;

    for (
        int attempt = 0;
        attempt < MAX_RETRIES && !hello_acked;
        ++attempt
    ) {
        Packet hello =
            makeHelloPacket(
                filename,
                total_chunks,
                file_crc
            );

        auto hello_buf =
            serializePacket(hello);

        if (hello_buf.empty()) {
            std::cerr
                << "failed to serialize HELLO\n";

            close(sockfd);
            return 1;
        }

        if (!sendDatagram(
                sockfd,
                dest,
                hello_buf
            )) {
            std::cerr
                << "sendto HELLO failed\n";

            close(sockfd);
            return 1;
        }

        std::cout
            << "HELLO sent: file="
            << filename
            << ", chunks="
            << total_chunks
            << "\n";

        std::vector<uint8_t> buf;
        sockaddr_in src{};
        socklen_t srclen =
            sizeof(src);

        if (receivePacket(
                sockfd,
                buf,
                src,
                srclen
            )) {
            Packet pkt;

            if (
                deserializePacket(buf, pkt) &&
                pkt.header.type ==
                    static_cast<uint8_t>(
                        PacketType::HELLO_ACK
                    )
            ) {
                hello_acked = true;

                std::cout
                    << "HELLO_ACK received\n";
            }
        }
    }

    if (!hello_acked) {
        std::cerr
            << "Failed to receive HELLO_ACK\n";

        close(sockfd);
        return 1;
    }

    std::vector<ChunkState> states(
        total_chunks,
        ChunkState{}
    );

    TransferMetrics metrics{};

    for (const auto &chunk : chunks) {
        metrics.payload_bytes += chunk.size();
    }

    metrics.transfer_start_us =
        currentTimeUs();

    uint32_t next_seq = 0;
    uint32_t in_flight = 0;

    bool transfer_complete =
        (total_chunks == 0);

    while (!transfer_complete) {
        while (
            next_seq < total_chunks &&
            in_flight < DEFAULT_WINDOW
        ) {
            const uint32_t seq = next_seq;
            const auto &payload = chunks[seq];

            Packet pkt =
                makeDataPacket(
                    seq,
                    total_chunks,
                    payload,
                    crc32(payload)
                );

            auto packet_buf =
                serializePacket(pkt);

            if (packet_buf.empty()) {
                std::cerr
                    << "failed to serialize DATA packet for seq "
                    << seq
                    << "\n";

                close(sockfd);
                return 1;
            }

            if (!sendDatagram(
                    sockfd,
                    dest,
                    packet_buf
                )) {
                std::cerr
                    << "sendto DATA failed for seq "
                    << seq
                    << "\n";

                close(sockfd);
                return 1;
            }

            states[seq].sent = true;
            states[seq].acked = false;
            states[seq].retransmitted = false;
            states[seq].last_sent_us =
                currentTimeUs();
            states[seq].retry_count = 0;

            ++metrics.initial_data_sends;
            ++in_flight;
            ++next_seq;

            std::cout
                << "sent chunk "
                << seq
                << "\n";
        }

        const uint64_t now_us =
            currentTimeUs();

        for (uint32_t i = 0; i < next_seq; ++i) {
            if (
                !states[i].sent ||
                states[i].acked
            ) {
                continue;
            }

            if (
                now_us -
                    states[i].last_sent_us
                <
                static_cast<uint64_t>(
                    TIMEOUT_MS_DEFAULT
                ) *
                    1000ULL
            ) {
                continue;
            }

            if (
                states[i].retry_count >=
                MAX_RETRIES
            ) {
                std::cerr
                    << "DATA retransmission limit exceeded for chunk "
                    << i
                    << "\n";

                close(sockfd);
                return 1;
            }

            const auto &payload =
                chunks[i];

            Packet pkt =
                makeDataPacket(
                    i,
                    total_chunks,
                    payload,
                    crc32(payload)
                );

            auto packet_buf =
                serializePacket(pkt);

            if (packet_buf.empty()) {
                std::cerr
                    << "failed to serialize retransmission for seq "
                    << i
                    << "\n";

                close(sockfd);
                return 1;
            }

            if (!sendDatagram(
                    sockfd,
                    dest,
                    packet_buf
                )) {
                std::cerr
                    << "sendto retransmission failed for seq "
                    << i
                    << "\n";

                close(sockfd);
                return 1;
            }

            states[i].last_sent_us =
                currentTimeUs();

            states[i].retransmitted =
                true;

            ++states[i].retry_count;
            ++metrics.retransmissions;

            std::cout
                << "retransmitted chunk "
                << i
                << " (attempt "
                << states[i].retry_count
                << ")\n";
        }

        auto processAck =
            [&](const std::vector<uint8_t> &ack_buf) {
                Packet pkt;

                if (!deserializePacket(
                        ack_buf,
                        pkt
                    )) {
                    return;
                }

                if (
                    pkt.header.type !=
                    static_cast<uint8_t>(
                        PacketType::ACK
                    )
                ) {
                    return;
                }

                const uint32_t seq =
                    pkt.header.seq;

                if (seq >= total_chunks) {
                    return;
                }

                if (
                    !states[seq].sent ||
                    states[seq].acked
                ) {
                    return;
                }

                states[seq].acked = true;

                if (in_flight > 0) {
                    --in_flight;
                }

                ++metrics.ack_received;

                const uint64_t ack_time_us =
                    currentTimeUs();

                if (
                    !states[seq].retransmitted
                ) {
                    metrics.rtt_samples_us.push_back(
                        ack_time_us -
                        states[seq].last_sent_us
                    );
                }

                std::cout
                    << "ACK received for chunk "
                    << seq
                    << "\n";
            };

        std::vector<uint8_t> ack_buf;
        sockaddr_in src{};
        socklen_t srclen =
            sizeof(src);

        if (receivePacket(
                sockfd,
                ack_buf,
                src,
                srclen
            )) {
            processAck(ack_buf);

            while (
                receivePacketNonBlocking(
                    sockfd,
                    ack_buf,
                    src,
                    srclen
                )
            ) {
                processAck(ack_buf);
            }
        }

        if (
            next_seq >= total_chunks &&
            in_flight == 0
        ) {
            transfer_complete = true;
        }
    }

    metrics.transfer_end_us =
        currentTimeUs();

    const uint64_t transfer_time_us =
        metrics.transfer_end_us -
        metrics.transfer_start_us;

    const double transfer_time_s =
        static_cast<double>(
            transfer_time_us
        ) /
        1'000'000.0;

    const double goodput_mbps =
        transfer_time_s > 0.0
            ? (
                static_cast<double>(
                    metrics.payload_bytes
                ) *
                8.0
            ) /
                transfer_time_s /
                1'000'000.0
            : 0.0;

    const double retransmission_rate =
        metrics.initial_data_sends > 0
            ? static_cast<double>(
                  metrics.retransmissions
              ) /
              static_cast<double>(
                  metrics.initial_data_sends
              )
            : 0.0;

    std::cout
        << "\n=== Transfer Statistics ===\n";

    std::cout
        << "payload_bytes="
        << metrics.payload_bytes
        << "\n";

    std::cout
        << "transfer_time_ms="
        << (
            static_cast<double>(
                transfer_time_us
            ) /
            1000.0
        )
        << "\n";

    std::cout
        << "goodput_mbps="
        << std::fixed
        << std::setprecision(3)
        << goodput_mbps
        << "\n";

    std::cout
        << "initial_data_sends="
        << metrics.initial_data_sends
        << "\n";

    std::cout
        << "retransmissions="
        << metrics.retransmissions
        << "\n";

    std::cout
        << "retransmission_rate="
        << retransmission_rate
        << "\n";

    std::cout
        << "acks_received="
        << metrics.ack_received
        << "\n";

    std::cout
        << "rtt_samples="
        << metrics.rtt_samples_us.size()
        << "\n";

    std::cout
        << "rtt_p50_us="
        << percentileUs(
               metrics.rtt_samples_us,
               50.0
           )
        << "\n";

    std::cout
        << "rtt_p95_us="
        << percentileUs(
               metrics.rtt_samples_us,
               95.0
           )
        << "\n";

    std::cout
        << "rtt_p99_us="
        << percentileUs(
               metrics.rtt_samples_us,
               99.0
           )
        << "\n";

    bool fin_acked = false;

    for (
        int attempt = 0;
        attempt < MAX_RETRIES && !fin_acked;
        ++attempt
    ) {
        Packet fin =
            makeFinPacket(file_crc);

        auto fin_buf =
            serializePacket(fin);

        if (fin_buf.empty()) {
            std::cerr
                << "failed to serialize FIN\n";

            close(sockfd);
            return 1;
        }

        if (!sendDatagram(
                sockfd,
                dest,
                fin_buf
            )) {
            std::cerr
                << "sendto FIN failed\n";

            close(sockfd);
            return 1;
        }

        std::cout
            << "FIN sent\n";

        std::vector<uint8_t> buf;
        sockaddr_in src{};
        socklen_t srclen =
            sizeof(src);

        if (receivePacket(
                sockfd,
                buf,
                src,
                srclen
            )) {
            Packet pkt;

            if (
                deserializePacket(buf, pkt) &&
                pkt.header.type ==
                    static_cast<uint8_t>(
                        PacketType::FIN_ACK
                    )
            ) {
                fin_acked = true;

                std::cout
                    << "FIN_ACK received\n";
            }
        }
    }

    if (!fin_acked) {
        std::cerr
            << "Failed to receive FIN_ACK\n";
    }

    close(sockfd);

    return fin_acked ? 0 : 1;
}