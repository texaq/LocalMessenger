#pragma once
// ═══════════════════════════════════════════════════════════════════════
// MessageRepository.h — CRUD операции для таблицы messages
// ═══════════════════════════════════════════════════════════════════════

#include "Database.h"
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace lm::storage {

/// Направление сообщения
enum class MessageDirection : int {
    INCOMING = 0,
    OUTGOING = 1,
};

/// Статус доставки
enum class MessageStatus : int {
    SENT      = 0,
    DELIVERED = 1,
    READ      = 2,
};

/// Тип сообщения
enum class MessageType : int {
    TEXT  = 1,
    IMAGE = 2,
    FILE  = 3,
    VOICE = 4,
};

struct Message {
    int64_t           id         = 0;
    int64_t           contact_id = 0;
    MessageDirection  direction  = MessageDirection::OUTGOING;
    MessageType       msg_type   = MessageType::TEXT;
    std::string       content;
    std::vector<uint8_t> encrypted;
    MessageStatus     status     = MessageStatus::SENT;
    std::string       msg_uuid;
    std::string       created_at;
};

class MessageRepository {
public:
    explicit MessageRepository(Database& db);

    /// Сохранить новое сообщение
    int64_t insert(const Message& msg);

    /// Получить сообщения для контакта (с пагинацией)
    /// @param limit  — количество сообщений
    /// @param offset — смещение (0 = самые новые)
    std::vector<Message> get_by_contact(int64_t contact_id,
                                         int limit = 50,
                                         int offset = 0) const;

    /// Найти сообщение по UUID (для подтверждения доставки)
    std::optional<Message> find_by_uuid(const std::string& uuid) const;

    /// Обновить статус доставки
    void update_status(const std::string& msg_uuid, MessageStatus status);

    /// Полнотекстовый поиск по содержимому сообщений
    std::vector<Message> search(const std::string& query,
                                 int limit = 50) const;

    /// Количество непрочитанных сообщений для контакта
    int unread_count(int64_t contact_id) const;

    /// Пометить все входящие сообщения контакта как прочитанные
    void mark_all_read(int64_t contact_id);

    /// Получить последнее сообщение для контакта (для списка чатов)
    std::optional<Message> last_message(int64_t contact_id) const;

private:
    Message row_to_message(SQLite::Statement& stmt) const;

    Database& db_;
};

} // namespace lm::storage
