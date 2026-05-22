// ═══════════════════════════════════════════════════════════════════════
// Protocol.cpp — Реализация бинарного протокола
// ═══════════════════════════════════════════════════════════════════════

#include "Protocol.h"
#include <cstring>
#include <stdexcept>

namespace lm::net {

// ── Вспомогательные: запись/чтение числа в big-endian ──────────────────

static void write_u32_be(uint8_t* dst, uint32_t val) {
    dst[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((val >>  8) & 0xFF);
    dst[3] = static_cast<uint8_t>((val      ) & 0xFF);
}

static uint32_t read_u32_be(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24)
         | (static_cast<uint32_t>(src[1]) << 16)
         | (static_cast<uint32_t>(src[2]) <<  8)
         | (static_cast<uint32_t>(src[3])      );
}

// ── Сериализация пакета в буфер ───────────────────────────────────────
std::vector<uint8_t> serialize(const Packet& packet) {
    std::vector<uint8_t> buf(HEADER_SIZE + packet.payload.size());

    // magic (4 байта, big-endian)
    write_u32_be(buf.data(), packet.header.magic);

    // type (1 байт)
    buf[4] = static_cast<uint8_t>(packet.header.type);

    // length (4 байта, big-endian) — размер payload
    write_u32_be(buf.data() + 5, static_cast<uint32_t>(packet.payload.size()));

    // payload
    if (!packet.payload.empty()) {
        std::memcpy(buf.data() + HEADER_SIZE,
                    packet.payload.data(),
                    packet.payload.size());
    }

    return buf;
}

// ── Десериализация заголовка ───────────────────────────────────────────
std::optional<PacketHeader> deserialize_header(const uint8_t* data, size_t size) {
    if (size < HEADER_SIZE) {
        return std::nullopt;
    }

    PacketHeader hdr;
    hdr.magic  = read_u32_be(data);
    hdr.type   = static_cast<PacketType>(data[4]);
    hdr.length = read_u32_be(data + 5);

    // Проверяем magic — защита от мусорных данных
    if (hdr.magic != PROTOCOL_MAGIC) {
        return std::nullopt;
    }

    // Проверяем размер payload — защита от DoS
    if (hdr.length > MAX_PAYLOAD) {
        return std::nullopt;
    }

    return hdr;
}

// ── Фабричные функции ─────────────────────────────────────────────────

Packet make_packet(PacketType type, const std::vector<uint8_t>& payload) {
    Packet pkt;
    pkt.header.type   = type;
    pkt.header.length = static_cast<uint32_t>(payload.size());
    pkt.payload       = payload;
    return pkt;
}

Packet make_packet(PacketType type, const std::string& text) {
    std::vector<uint8_t> payload(text.begin(), text.end());
    return make_packet(type, payload);
}

Packet make_ping() {
    return make_packet(PacketType::PING, std::vector<uint8_t>{});
}

Packet make_pong() {
    return make_packet(PacketType::PONG, std::vector<uint8_t>{});
}

} // namespace lm::net
