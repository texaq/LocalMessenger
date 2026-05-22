#pragma once
// ═══════════════════════════════════════════════════════════════════════
// ChatMessage.h — Модель текстового сообщения
//
// Объединяет данные сообщения: UUID, текст, время, статус, шифрование.
// Используется ChatManager-ом для отправки/получения и сохранения в БД.
// ═══════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace lm::chat {

/// Направление сообщения
enum class Direction { INCOMING, OUTGOING };

/// Статус доставки
enum class Status { SENT, DELIVERED, READ };

/// Тип контента
enum class ContentType {
    TEXT,       // обычный текст (markdown-subset)
    IMAGE,      // изображение (ссылка на медиа-файл)
    FILE,       // файл
    VOICE,      // голосовое сообщение
    REACTION,   // реакция (emoji) на другое сообщение
};

/// Реакция на сообщение
struct Reaction {
    std::string emoji;       // символ реакции (напр. "👍")
    std::string sender_id;   // public_key отправителя реакции
};

/// Полная модель сообщения чата
struct ChatMessage {
    std::string   uuid;            // UUID сообщения (для подтверждений)
    std::string   sender_id;       // public_key отправителя
    std::string   receiver_id;     // public_key получателя (или group_id)
    Direction     direction = Direction::OUTGOING;
    ContentType   content_type = ContentType::TEXT;
    std::string   text;            // текст (или путь к файлу)
    Status        status    = Status::SENT;
    int64_t       timestamp = 0;   // unix timestamp (ms)
    std::string   reply_to_uuid;   // UUID сообщения, на которое отвечаем
    std::vector<Reaction> reactions;

    // Для групповых чатов
    std::string   group_id;        // пусто для 1-на-1
    std::string   sender_name;     // имя отправителя (для отображения)

    /// Текущее время в миллисекундах
    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }
};

} // namespace lm::chat
