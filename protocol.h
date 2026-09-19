#ifndef SIUU_RUDP_PROTOCOL_H
#define SIUU_RUDP_PROTOCOL_H

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

static constexpr uint8_t  PROTO_VERSION      = 1;
static constexpr size_t   CHUNK_SIZE         = 1200;
static constexpr size_t   MAX_PACKET_SIZE    = 1400;
static constexpr uint32_t DEFAULT_WINDOW     = 32;
static constexpr uint32_t TIMEOUT_MS_DEFAULT = 500;
static constexpr int      MAX_RETRIES        = 10;

enum class PacketType : uint8_t {
    HELLO     = 1,
    HELLO_ACK = 2,
    DATA      = 3,
    ACK       = 4,
    NACK      = 5,
    FIN       = 6,
    FIN_ACK   = 7
};

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t  version;
    uint8_t  type;
    uint32_t seq;
    uint32_t total_chunks;
    uint16_t payload_len;
    uint32_t checksum;
};
#pragma pack(pop)

struct Packet {
    PacketHeader header{};
    std::vector<uint8_t> payload;
};

using SeqRange = std::pair<uint32_t, uint32_t>;

std::vector<uint8_t> serializePacket(const Packet &pkt);
bool deserializePacket(const std::vector<uint8_t> &buf, Packet &pkt);

Packet makeHelloPacket(const std::string &filename, uint32_t total_chunks, uint32_t file_crc32);
Packet makeHelloAckPacket(const std::vector<uint8_t> &optional_bitmap);
Packet makeDataPacket(uint32_t seq, uint32_t total_chunks, const std::vector<uint8_t> &payload, uint32_t payload_crc32);
Packet makeAckPacket(uint32_t base_seq, const std::vector<uint8_t> &bitmap);
Packet makeNackPacket(const std::vector<SeqRange> &missing_ranges);
Packet makeFinPacket(uint32_t file_crc32);
Packet makeFinAckPacket();

std::vector<uint8_t> encodeBitmap(const std::vector<bool> &received, size_t window_size);
std::vector<bool> decodeBitmap(const std::vector<uint8_t> &bitmap, size_t window_size);

uint32_t crc32(const uint8_t *buf, size_t len);
uint32_t crc32(const std::vector<uint8_t> &v);
uint32_t crc32Update(uint32_t crc, const uint8_t *buf, size_t len);
uint32_t crc32Update(uint32_t crc, const std::vector<uint8_t> &v);

bool parseHelloPayload(const std::vector<uint8_t> &payload, std::string &filename, uint32_t &total_chunks, uint32_t &file_crc32);
std::vector<uint8_t> buildHelloPayload(const std::string &filename, uint32_t total_chunks, uint32_t file_crc32);

#endif