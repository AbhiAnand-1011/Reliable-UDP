#include "protocol.h"
#include "utils.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

enum class ReceiveState {
    WAIT_FOR_HELLO,
    TRANSFER_IN_PROGRESS,
    ALL_DATA_RECEIVED,
    FILE_ASSEMBLED_AND_VERIFIED,
    WAIT_FOR_FIN,
    DONE
};

bool validateHelloPacket(
    const Packet &pkt,
    uint32_t &expected_total,
    std::string &safe_filename,
    uint32_t &expected_crc
) {
    if (pkt.header.type != static_cast<uint8_t>(PacketType::HELLO))
        return false;

    if (pkt.header.seq != 0)
        return false;

    if (pkt.header.payload_len == 0)
        return false;

    std::string candidate;
    uint32_t total_chunks = 0;
    uint32_t calculated_crc = 0;

    if (!parseHelloPayload(
            pkt.payload,
            candidate,
            total_chunks,
            calculated_crc
        )) {
        return false;
    }

    if (candidate.empty())
        return false;

    std::string sanitized = sanitizeFilename(candidate);

    if (sanitized.empty())
        return false;

    if (pkt.header.total_chunks != total_chunks)
        return false;

    expected_total = total_chunks;
    expected_crc = calculated_crc;
    safe_filename = sanitized;

    return true;
}

bool packetIsValidData(
    const Packet &pkt,
    uint32_t expected_total_chunks
) {
    if (pkt.header.type != static_cast<uint8_t>(PacketType::DATA))
        return false;

    if (pkt.header.total_chunks != expected_total_chunks)
        return false;

    if (pkt.header.seq >= expected_total_chunks)
        return false;

    if (pkt.header.payload_len > CHUNK_SIZE)
        return false;

    if (pkt.payload.size() != pkt.header.payload_len)
        return false;

    if (pkt.payload.empty())
        return false;

    return true;
}

bool computeAssembledFileCrc(
    const std::string &path,
    uint32_t &file_crc
) {
    std::ifstream in(path, std::ios::binary);

    if (!in)
        return false;

    uint32_t crc = 0xFFFFFFFF;
    std::vector<uint8_t> buffer(4096, 0);

    while (in) {
        in.read(
            reinterpret_cast<char *>(buffer.data()),
            static_cast<std::streamsize>(buffer.size())
        );

        std::streamsize read_count = in.gcount();

        if (read_count <= 0)
            break;

        crc = crc32Update(
            crc,
            buffer.data(),
            static_cast<size_t>(read_count)
        );
    }

    file_crc = crc ^ 0xFFFFFFFF;

    return true;
}

bool verifyTransferCompletion(
    const std::string &outdir,
    const std::string &filename,
    uint32_t total_chunks,
    const std::unordered_set<uint32_t> &received,
    uint32_t expected_crc,
    std::string &assembled_path
) {
    if (total_chunks == 0) {
        assembled_path = outdir + "/" + filename;

        uint32_t crc = 0;

        if (!computeAssembledFileCrc(
                assembled_path,
                crc
            )) {
            return false;
        }

        return crc == expected_crc;
    }

    for (uint32_t i = 0; i < total_chunks; ++i) {
        if (received.find(i) == received.end())
            return false;
    }

    std::string final_path =
        outdir + "/" + filename;

    if (!assembleChunksFromDir(
            outdir,
            final_path,
            total_chunks
        )) {
        return false;
    }

    uint32_t crc = 0;

    if (!computeAssembledFileCrc(
            final_path,
            crc
        )) {
        return false;
    }

    assembled_path = final_path;

    return crc == expected_crc;
}

bool sendAck(
    int sockfd,
    const sockaddr_in &dest,
    socklen_t dest_len,
    uint32_t seq
) {
    Packet ack = makeAckPacket(seq, {});
    auto out = serializePacket(ack);

    if (out.empty())
        return false;

    ssize_t sent = sendto(
        sockfd,
        out.data(),
        out.size(),
        0,
        reinterpret_cast<const sockaddr *>(&dest),
        dest_len
    );

    return sent == static_cast<ssize_t>(out.size());
}

bool sendFinAck(
    int sockfd,
    const sockaddr_in &dest,
    socklen_t dest_len
) {
    Packet fin_ack = makeFinAckPacket();
    auto out = serializePacket(fin_ack);

    if (out.empty())
        return false;

    ssize_t sent = sendto(
        sockfd,
        out.data(),
        out.size(),
        0,
        reinterpret_cast<const sockaddr *>(&dest),
        dest_len
    );

    return sent == static_cast<ssize_t>(out.size());
}

}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr
            << "usage: receiver <port> <outdir>\n";
        return 1;
    }

    int port = std::stoi(argv[1]);
    std::string outdir = argv[2];

    if (!ensureDir(outdir)) {
        std::cerr
            << "failed to create output directory: "
            << outdir
            << "\n";
        return 1;
    }

    int sockfd =
        socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct timeval tv{};

    tv.tv_sec = 0;
    tv.tv_usec =
        static_cast<suseconds_t>(
            TIMEOUT_MS_DEFAULT * 1000
        );

    if (setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &tv,
            sizeof(tv)
        ) != 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sockfd);
        return 1;
    }

    int enable = 1;

    if (setsockopt(
            sockfd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enable,
            sizeof(enable)
        ) != 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return 1;
    }

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port =
        htons(static_cast<uint16_t>(port));

    if (bind(
            sockfd,
            reinterpret_cast<sockaddr *>(&addr),
            sizeof(addr)
        ) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    std::cout
        << "receiver listening on port "
        << port
        << std::endl;

    ReceiveState state =
        ReceiveState::WAIT_FOR_HELLO;

    std::string filename;
    std::string final_path;

    uint32_t total_chunks = 0;
    uint32_t expected_crc = 0;

    std::unordered_set<uint32_t> received;

    uint32_t received_count = 0;

    bool transfer_verified = false;

    uint64_t fin_deadline_ms = 0;

    while (true) {
        std::vector<uint8_t> buf(
            MAX_PACKET_SIZE,
            0
        );

        sockaddr_in src{};
        socklen_t srclen =
            sizeof(src);

        ssize_t n = recvfrom(
            sockfd,
            buf.data(),
            buf.size(),
            0,
            reinterpret_cast<sockaddr *>(&src),
            &srclen
        );

        if (n < 0) {
            if (
                errno == EAGAIN ||
                errno == EWOULDBLOCK
            ) {
                if (
                    state == ReceiveState::DONE &&
                    currentTimeMs() >= fin_deadline_ms
                ) {
                    break;
                }

                continue;
            }

            perror("recvfrom");
            continue;
        }

        if (n == 0)
            continue;

        buf.resize(
            static_cast<size_t>(n)
        );

        Packet pkt;

        if (!deserializePacket(
                buf,
                pkt
            )) {
            continue;
        }

        if (
            pkt.header.version !=
            PROTO_VERSION
        ) {
            continue;
        }

        const PacketType type =
            static_cast<PacketType>(
                pkt.header.type
            );

        if (
            state ==
            ReceiveState::WAIT_FOR_HELLO
        ) {
            if (type != PacketType::HELLO)
                continue;

            uint32_t negotiated_total = 0;
            std::string safe_filename;
            uint32_t hello_crc = 0;

            if (!validateHelloPacket(
                    pkt,
                    negotiated_total,
                    safe_filename,
                    hello_crc
                )) {
                std::cout
                    << "rejected malformed HELLO\n";
                continue;
            }

            filename = safe_filename;
            total_chunks = negotiated_total;
            expected_crc = hello_crc;

            received.clear();
            received_count = 0;
            transfer_verified = false;

            state =
                total_chunks == 0
                    ? ReceiveState::FILE_ASSEMBLED_AND_VERIFIED
                    : ReceiveState::TRANSFER_IN_PROGRESS;

            std::cout
                << "HELLO received: file="
                << filename
                << ", chunks="
                << total_chunks
                << std::endl;

            Packet ack =
                makeHelloAckPacket({});

            auto out =
                serializePacket(ack);

            if (!out.empty()) {
                sendto(
                    sockfd,
                    out.data(),
                    out.size(),
                    0,
                    reinterpret_cast<const sockaddr *>(&src),
                    srclen
                );
            }

            if (total_chunks == 0) {
    final_path = outdir + "/" + filename;

    std::ofstream empty_file(
        final_path,
        std::ios::binary | std::ios::trunc
    );

    if (!empty_file) {
        std::cerr
            << "failed to create empty output file\n";
        close(sockfd);
        return 1;
    }

    std::cout
        << "empty file transfer: waiting for FIN\n";

    transfer_verified = true;
    state =
        ReceiveState::WAIT_FOR_FIN;
}
            continue;
        }

        if (
            state ==
            ReceiveState::TRANSFER_IN_PROGRESS
        ) {
            if (type == PacketType::DATA) {
                if (!packetIsValidData(
                        pkt,
                        total_chunks
                    )) {
                    continue;
                }

                const uint32_t seq =
                    pkt.header.seq;

                if (
                    received.find(seq) ==
                    received.end()
                ) {
                    bool ok =
                        writeChunkToTemp(
                            outdir,
                            seq,
                            pkt.payload
                        );

                    if (!ok) {
                        std::cerr
                            << "failed to write chunk "
                            << seq
                            << " to disk\n";
                        continue;
                    }

                    received.insert(seq);
                    ++received_count;

                    std::cout
                        << "received chunk "
                        << seq
                        << std::endl;
                }

                sendAck(
                    sockfd,
                    src,
                    srclen,
                    seq
                );

                if (
                    received_count ==
                    total_chunks
                ) {
                    if (
                        verifyTransferCompletion(
                            outdir,
                            filename,
                            total_chunks,
                            received,
                            expected_crc,
                            final_path
                        )
                    ) {
                        std::cout
                            << "all chunks received and file CRC verified\n";

                        transfer_verified = true;

                        state =
                            ReceiveState::WAIT_FOR_FIN;
                    } else {
                        std::cerr
                            << "transfer failed: downloaded file did not validate\n";
                    }
                }

                continue;
            }

            if (type == PacketType::FIN) {
                if (!transfer_verified)
                    continue;

                if (pkt.payload.size() != 4) {
                    std::cout
                        << "ignored malformed FIN\n";
                    continue;
                }

                uint32_t fin_crc = 0;

                std::memcpy(
                    &fin_crc,
                    pkt.payload.data(),
                    4
                );

                fin_crc = ntohl(fin_crc);

                if (
                    fin_crc !=
                    expected_crc
                ) {
                    std::cout
                        << "ignored FIN with mismatched CRC\n";
                    continue;
                }

                sendFinAck(
                    sockfd,
                    src,
                    srclen
                );

                std::cout
                    << "FIN received, sent FIN_ACK\n";

                state =
                    ReceiveState::DONE;

                fin_deadline_ms =
                    currentTimeMs() +
                    (3U * TIMEOUT_MS_DEFAULT);

                continue;
            }

            if (type == PacketType::HELLO)
                continue;
        }

        if (
            state ==
            ReceiveState::WAIT_FOR_FIN
        ) {
            if (type == PacketType::DATA) {
                if (!packetIsValidData(
                        pkt,
                        total_chunks
                    )) {
                    continue;
                }

                sendAck(
                    sockfd,
                    src,
                    srclen,
                    pkt.header.seq
                );

                continue;
            }

            if (type == PacketType::FIN) {
                uint32_t fin_crc = 0;

                if (pkt.payload.size() == 4) {
                    std::memcpy(
                        &fin_crc,
                        pkt.payload.data(),
                        4
                    );

                    fin_crc =
                        ntohl(fin_crc);
                }

                if (
                    fin_crc == expected_crc ||
                    total_chunks == 0
                ) {
                    sendFinAck(
                        sockfd,
                        src,
                        srclen
                    );

                    std::cout
                        << "FIN received, sent FIN_ACK\n";

                    state =
                        ReceiveState::DONE;

                    fin_deadline_ms =
                        currentTimeMs() +
                        (3U * TIMEOUT_MS_DEFAULT);
                }

                continue;
            }
        }

        if (
            state ==
            ReceiveState::DONE
        ) {
            if (type == PacketType::DATA) {
                if (
                    packetIsValidData(
                        pkt,
                        total_chunks
                    )
                ) {
                    sendAck(
                        sockfd,
                        src,
                        srclen,
                        pkt.header.seq
                    );
                }

                continue;
            }

            if (type == PacketType::FIN) {
                sendFinAck(
                    sockfd,
                    src,
                    srclen
                );

                fin_deadline_ms =
                    currentTimeMs() +
                    (3U * TIMEOUT_MS_DEFAULT);

                continue;
            }

            if (
                currentTimeMs() >=
                fin_deadline_ms
            ) {
                break;
            }
        }
    }

    close(sockfd);

    return 0;
}clear
