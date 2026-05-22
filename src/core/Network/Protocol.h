#pragma once
// ═══════════════════════════════════════════════════════════════════════
// Protocol.h — Бинарный протокол обмена сообщениями
//
// Формат пакета:
//   [magic:4][type:1][length:4][payload:N]
//
// magic  = 0x4C4D5347 ("LMSG")
// type   = PacketType enum
// length = размер payload в байтах (big-endian)
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace lm::net {

// Магическое число заголовка — "LMSG" в ASCII
constexpr uint32_t PROTOCOL_MAGIC = 0x4C4D5347;

// Размер фиксированного заголовка: magic(4) + type(1) + length(4) = 9
constexpr size_t   HEADER_SIZE    = 9;

// Максимальный размер payload (16 MiB — защита от переполнения)
constexpr uint32_t MAX_PAYLOAD    = 16 * 1024 * 1024;

// ── Типы пакетов ──────────────────────────────────────────────────────
enum class PacketType : uint8_t {
    // Текстовый чат
    MSG_TEXT       = 0x01,  // текстовое сообщение
    MSG_MEDIA      = 0x02,  // медиа-сообщение (изображение/голосовое)
    MSG_RECEIPT    = 0x03,  // подтверждение доставки / прочтения
    MSG_TYPING     = 0x04,  // индикатор набора текста
    MSG_REACTION   = 0x05,  // реакция на сообщение (emoji)

    // Звонки — сигнализация
    CALL_OFFER     = 0x10,  // SDP offer
    CALL_ANSWER    = 0x11,  // SDP answer
    CALL_ICE       = 0x12,  // ICE candidate
    CALL_HANGUP    = 0x13,  // завершение звонка
    CALL_DECLINE   = 0x14,  // отклонение звонка

    // Передача файлов
    FILE_META      = 0x20,  // метаданные файла
    FILE_CHUNK     = 0x21,  // блок данных файла
    FILE_ACK       = 0x22,  // подтверждение блока
    FILE_CANCEL    = 0x23,  // отмена передачи
    FILE_PAUSE     = 0x24,  // пауза передачи

    // Демонстрация экрана
    SCREEN_OFFER   = 0x30,  // предложение демо экрана
    SCREEN_FRAME   = 0x31,  // кадр экрана
    SCREEN_INPUT   = 0x32,  // события ввода (remote control)

    // Служебные
    PING           = 0xF0,  // проверка связи
    PONG           = 0xF1,  // ответ на ping
    PRESENCE       = 0xF2,  // присутствие (UDP broadcast)
    KEY_EXCHANGE   = 0xF3,  // обмен публичными ключами X25519
    HANDSHAKE      = 0xF4,  // рукопожатие при установке TCP-соединения
};

// ── Статусы доставки ──────────────────────────────────────────────────
enum class DeliveryStatus : uint8_t {
    SENT      = 0,  // отправлено
    DELIVERED = 1,  // доставлено
    READ      = 2,  // прочитано
};

// ── Заголовок пакета ──────────────────────────────────────────────────
struct PacketHeader {
    uint32_t   magic  = PROTOCOL_MAGIC;
    PacketType type   = PacketType::PING;
    uint32_t   length = 0;  // размер payload
};

// ── Полный пакет (заголовок + данные) ─────────────────────────────────
struct Packet {
    PacketHeader          header;
    std::vector<uint8_t>  payload;
};

// ── Функции сериализации / десериализации ──────────────────────────────

/// Сериализовать пакет в байтовый буфер (заголовок + payload)
std::vector<uint8_t> serialize(const Packet& packet);

/// Десериализовать заголовок из буфера (ровно HEADER_SIZE байт)
/// Возвращает nullopt при неверном magic
std::optional<PacketHeader> deserialize_header(const uint8_t* data, size_t size);

/// Создать пакет заданного типа с произвольным payload
Packet make_packet(PacketType type, const std::vector<uint8_t>& payload);

/// Создать пакет с текстовым payload (UTF-8)
Packet make_packet(PacketType type, const std::string& text);

/// Создать PING/PONG без payload
Packet make_ping();
Packet make_pong();

} // namespace lm::net
