#include "protocol.h"

#include <cstring>
#include <arpa/inet.h>

namespace {

void appendU16BE(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU32BE(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

uint32_t computeLogicalChecksum(const PacketHeader &header, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> canonical;
    canonical.reserve(1 + 1 + 4 + 4 + 2 + payload.size() + 4);
    canonical.push_back(header.version);
    canonical.push_back(header.type);
    appendU32BE(canonical, header.seq);
    appendU32BE(canonical, header.total_chunks);
    appendU16BE(canonical, header.payload_len);
    canonical.insert(canonical.end(), payload.begin(), payload.end());
    return crc32(canonical);
}

} // namespace

static uint32_t crc_table[256];
static bool crc_init = false;

static void init_crc() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_init = true;
}

uint32_t crc32(const uint8_t *buf, size_t len) {
    if (!crc_init)
        init_crc();
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

uint32_t crc32(const std::vector<uint8_t> &v) {
    if (v.empty())
        return 0;
    return crc32(v.data(), v.size());
}

uint32_t crc32Update(uint32_t crc, const uint8_t *buf, size_t len) {
    if (!crc_init)
        init_crc();
    uint32_t c = crc;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c;
}

uint32_t crc32Update(uint32_t crc, const std::vector<uint8_t> &v) {
    if (v.empty())
        return crc;
    return crc32Update(crc, v.data(), v.size());
}

std::vector<uint8_t> serializePacket(const Packet &pkt) {
    if (pkt.payload.size() > MAX_PACKET_SIZE - sizeof(PacketHeader))
        return {};

    Packet normalized = pkt;
    normalized.header.checksum = computeLogicalChecksum(normalized.header, normalized.payload);

    std::vector<uint8_t> buf(sizeof(PacketHeader) + normalized.payload.size(), 0);
    size_t off = 0;
    buf[off++] = normalized.header.version;
    buf[off++] = normalized.header.type;
    uint32_t seq = htonl(normalized.header.seq);
    std::memcpy(&buf[off], &seq, sizeof(seq));
    off += sizeof(seq);
    uint32_t total = htonl(normalized.header.total_chunks);
    std::memcpy(&buf[off], &total, sizeof(total));
    off += sizeof(total);
    uint16_t payload_len = htons(normalized.header.payload_len);
    std::memcpy(&buf[off], &payload_len, sizeof(payload_len));
    off += sizeof(payload_len);
    uint32_t checksum = htonl(normalized.header.checksum);
    std::memcpy(&buf[off], &checksum, sizeof(checksum));
    off += sizeof(checksum);
    if (!normalized.payload.empty())
        std::memcpy(buf.data() + off, normalized.payload.data(), normalized.payload.size());
    return buf;
}

bool deserializePacket(const std::vector<uint8_t> &buf, Packet &pkt) {
    if (buf.size() < sizeof(PacketHeader))
        return false;

    size_t off = 0;
    pkt = Packet{};
    pkt.header.version = buf[off++];
    pkt.header.type = buf[off++];
    if (pkt.header.version != PROTO_VERSION)
        return false;

    uint32_t seq = 0;
    uint32_t total = 0;
    uint16_t payload_len = 0;
    std::memcpy(&seq, buf.data() + off, sizeof(seq));
    off += sizeof(seq);
    std::memcpy(&total, buf.data() + off, sizeof(total));
    off += sizeof(total);
    std::memcpy(&payload_len, buf.data() + off, sizeof(payload_len));
    off += sizeof(payload_len);
    std::memcpy(&pkt.header.checksum, buf.data() + off, sizeof(pkt.header.checksum));
    off += sizeof(pkt.header.checksum);

    pkt.header.seq = ntohl(seq);
    pkt.header.total_chunks = ntohl(total);
    pkt.header.payload_len = ntohs(payload_len);
    pkt.header.checksum = ntohl(pkt.header.checksum);

    if (pkt.header.payload_len > MAX_PACKET_SIZE - sizeof(PacketHeader))
        return false;
    if (buf.size() < sizeof(PacketHeader) + pkt.header.payload_len)
        return false;

    pkt.payload.resize(pkt.header.payload_len);
    if (pkt.header.payload_len > 0) {
        std::memcpy(pkt.payload.data(), buf.data() + sizeof(PacketHeader), pkt.header.payload_len);
    }

    PacketHeader expected = pkt.header;
    expected.checksum = 0;
    uint32_t actual = computeLogicalChecksum(expected, pkt.payload);
    if (actual != pkt.header.checksum)
        return false;

    return true;
}

Packet makeHelloPacket(const std::string &filename, uint32_t total_chunks, uint32_t file_crc32) {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::HELLO);
    pkt.header.seq = 0;
    pkt.header.total_chunks = total_chunks;
    pkt.payload = buildHelloPayload(filename, total_chunks, file_crc32);
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    pkt.header.checksum = 0;
    pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    return pkt;
}

Packet makeHelloAckPacket(const std::vector<uint8_t> &optional_bitmap) {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::HELLO_ACK);
    pkt.header.seq = 0;
    pkt.header.total_chunks = 0;
    pkt.payload = optional_bitmap;
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    pkt.header.checksum = 0;
    pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    return pkt;
}

Packet makeDataPacket(uint32_t seq, uint32_t total_chunks, const std::vector<uint8_t> &payload, uint32_t payload_crc32) {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::DATA);
    pkt.header.seq = seq;
    pkt.header.total_chunks = total_chunks;
    pkt.payload = payload;
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    pkt.header.checksum = payload_crc32;
    if (payload_crc32 == 0 && !pkt.payload.empty()) {
        pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    } else if (pkt.payload.empty()) {
        pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    }
    return pkt;
}

Packet makeAckPacket(uint32_t base_seq, const std::vector<uint8_t> &bitmap) {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::ACK);
    pkt.header.seq = base_seq;
    pkt.header.total_chunks = 0;
    pkt.payload = bitmap;
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    pkt.header.checksum = 0;
    pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    return pkt;
}

Packet makeNackPacket(const std::vector<SeqRange> &missing_ranges) {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::NACK);
    pkt.header.seq = 0;
    pkt.header.total_chunks = 0;

    pkt.payload.clear();
    for (const auto &r : missing_ranges) {
        uint32_t s = htonl(r.first);
        uint32_t l = htonl(r.second);
        pkt.payload.insert(pkt.payload.end(), reinterpret_cast<uint8_t *>(&s), reinterpret_cast<uint8_t *>(&s) + 4);
        pkt.payload.insert(pkt.payload.end(), reinterpret_cast<uint8_t *>(&l), reinterpret_cast<uint8_t *>(&l) + 4);
    }

    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
    pkt.header.checksum = 0;
    pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    return pkt;
}

Packet makeFinPacket(uint32_t file_crc32) {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::FIN);
    pkt.header.seq = 0;
    pkt.header.total_chunks = 0;
    pkt.payload.resize(4);
    uint32_t c = htonl(file_crc32);
    std::memcpy(pkt.payload.data(), &c, 4);
    pkt.header.payload_len = 4;
    pkt.header.checksum = 0;
    pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    return pkt;
}

Packet makeFinAckPacket() {
    Packet pkt{};
    pkt.header.version = PROTO_VERSION;
    pkt.header.type = static_cast<uint8_t>(PacketType::FIN_ACK);
    pkt.header.seq = 0;
    pkt.header.total_chunks = 0;
    pkt.payload.clear();
    pkt.header.payload_len = 0;
    pkt.header.checksum = 0;
    pkt.header.checksum = computeLogicalChecksum(pkt.header, pkt.payload);
    return pkt;
}

std::vector<uint8_t> encodeBitmap(const std::vector<bool> &received, size_t window_size) {
    size_t bytes = (window_size + 7) / 8;
    std::vector<uint8_t> bitmap(bytes, 0);
    for (size_t i = 0; i < window_size && i < received.size(); i++) {
        if (received[i])
            bitmap[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
    }
    return bitmap;
}

std::vector<bool> decodeBitmap(const std::vector<uint8_t> &bitmap, size_t window_size) {
    std::vector<bool> received(window_size, false);
    if (window_size == 0)
        return received;

    size_t bytes_needed = (window_size + 7) / 8;
    size_t limit = std::min(bitmap.size(), bytes_needed);
    for (size_t i = 0; i < window_size; ++i) {
        size_t byte_index = i / 8;
        if (byte_index >= limit)
            break;
        if ((bitmap[byte_index] & static_cast<uint8_t>(1U << (i % 8))) != 0)
            received[i] = true;
    }
    return received;
}

std::vector<uint8_t> buildHelloPayload(const std::string &filename, uint32_t total_chunks, uint32_t file_crc32) {
    std::vector<uint8_t> payload;
    if (filename.size() > 65535)
        return payload;

    uint16_t name_len = static_cast<uint16_t>(filename.size());
    uint32_t tc = htonl(total_chunks);
    uint32_t fc = htonl(file_crc32);

    payload.reserve(2 + filename.size() + 4 + 4);
    payload.push_back(static_cast<uint8_t>((name_len >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(name_len & 0xFF));
    payload.insert(payload.end(), filename.begin(), filename.end());
    payload.insert(payload.end(), reinterpret_cast<const uint8_t *>(&tc), reinterpret_cast<const uint8_t *>(&tc) + 4);
    payload.insert(payload.end(), reinterpret_cast<const uint8_t *>(&fc), reinterpret_cast<const uint8_t *>(&fc) + 4);
    return payload;
}

bool parseHelloPayload(const std::vector<uint8_t> &payload, std::string &filename, uint32_t &total_chunks, uint32_t &file_crc32) {
    if (payload.size() < 10)
        return false;

    uint16_t name_len = static_cast<uint16_t>((static_cast<uint16_t>(payload[0]) << 8) | static_cast<uint16_t>(payload[1]));
    size_t total_size = 2 + static_cast<size_t>(name_len) + 8;
    if (payload.size() != total_size)
        return false;

    filename.assign(reinterpret_cast<const char *>(payload.data() + 2), name_len);

    uint32_t tc = 0;
    uint32_t fc = 0;
    std::memcpy(&tc, payload.data() + 2 + name_len, 4);
    std::memcpy(&fc, payload.data() + 6 + name_len, 4);
    total_chunks = ntohl(tc);
    file_crc32 = ntohl(fc);
    return true;
}