// ═══════════════════════════════════════════════════════════════════════
// MessageRepository.cpp — Реализация CRUD для сообщений
// ═══════════════════════════════════════════════════════════════════════

#include "MessageRepository.h"
#include <spdlog/spdlog.h>

namespace lm::storage {

MessageRepository::MessageRepository(Database& db) : db_(db) {}

int64_t MessageRepository::insert(const Message& msg) {
    SQLite::Statement stmt(db_.db(), R"SQL(
        INSERT INTO messages (contact_id, direction, msg_type, content,
                              encrypted, status, msg_uuid)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )SQL");

    stmt.bind(1, msg.contact_id);
    stmt.bind(2, static_cast<int>(msg.direction));
    stmt.bind(3, static_cast<int>(msg.msg_type));
    stmt.bind(4, msg.content);

    if (!msg.encrypted.empty()) {
        stmt.bind(5, msg.encrypted.data(),
                  static_cast<int>(msg.encrypted.size()));
    } else {
        stmt.bind(5);  // NULL
    }

    stmt.bind(6, static_cast<int>(msg.status));
    stmt.bind(7, msg.msg_uuid);
    stmt.exec();

    return db_.db().getLastInsertRowid();
}

std::vector<Message> MessageRepository::get_by_contact(
    int64_t contact_id, int limit, int offset) const {

    // Сортируем по времени (новые в конце), но с пагинацией
    SQLite::Statement stmt(db_.db(), R"SQL(
        SELECT * FROM messages
        WHERE contact_id = ?
        ORDER BY created_at DESC
        LIMIT ? OFFSET ?
    )SQL");

    stmt.bind(1, contact_id);
    stmt.bind(2, limit);
    stmt.bind(3, offset);

    std::vector<Message> result;
    while (stmt.executeStep()) {
        result.push_back(row_to_message(stmt));
    }

    // Разворачиваем, чтобы старые были первыми (для отображения в чате)
    std::reverse(result.begin(), result.end());
    return result;
}

std::optional<Message> MessageRepository::find_by_uuid(
    const std::string& uuid) const {

    SQLite::Statement stmt(db_.db(),
        "SELECT * FROM messages WHERE msg_uuid = ?");
    stmt.bind(1, uuid);

    if (stmt.executeStep()) {
        return row_to_message(stmt);
    }
    return std::nullopt;
}

void MessageRepository::update_status(const std::string& msg_uuid,
                                       MessageStatus status) {
    SQLite::Statement stmt(db_.db(), R"SQL(
        UPDATE messages SET status = ? WHERE msg_uuid = ?
    )SQL");
    stmt.bind(1, static_cast<int>(status));
    stmt.bind(2, msg_uuid);
    stmt.exec();
}

std::vector<Message> MessageRepository::search(const std::string& query,
                                                 int limit) const {
    // Простой LIKE-поиск; для FTS5 нужна отдельная таблица
    SQLite::Statement stmt(db_.db(), R"SQL(
        SELECT * FROM messages
        WHERE content LIKE '%' || ? || '%'
        ORDER BY created_at DESC
        LIMIT ?
    )SQL");

    stmt.bind(1, query);
    stmt.bind(2, limit);

    std::vector<Message> result;
    while (stmt.executeStep()) {
        result.push_back(row_to_message(stmt));
    }
    return result;
}

int MessageRepository::unread_count(int64_t contact_id) const {
    SQLite::Statement stmt(db_.db(), R"SQL(
        SELECT COUNT(*) FROM messages
        WHERE contact_id = ? AND direction = 0 AND status < 2
    )SQL");
    stmt.bind(1, contact_id);
    stmt.executeStep();
    return stmt.getColumn(0).getInt();
}

void MessageRepository::mark_all_read(int64_t contact_id) {
    SQLite::Statement stmt(db_.db(), R"SQL(
        UPDATE messages SET status = 2
        WHERE contact_id = ? AND direction = 0 AND status < 2
    )SQL");
    stmt.bind(1, contact_id);
    stmt.exec();
}

std::optional<Message> MessageRepository::last_message(
    int64_t contact_id) const {

    SQLite::Statement stmt(db_.db(), R"SQL(
        SELECT * FROM messages
        WHERE contact_id = ?
        ORDER BY created_at DESC
        LIMIT 1
    )SQL");
    stmt.bind(1, contact_id);

    if (stmt.executeStep()) {
        return row_to_message(stmt);
    }
    return std::nullopt;
}

Message MessageRepository::row_to_message(SQLite::Statement& stmt) const {
    Message m;
    m.id         = stmt.getColumn("id").getInt64();
    m.contact_id = stmt.getColumn("contact_id").getInt64();
    m.direction  = static_cast<MessageDirection>(stmt.getColumn("direction").getInt());
    m.msg_type   = static_cast<MessageType>(stmt.getColumn("msg_type").getInt());
    m.content    = stmt.getColumn("content").getString();

    // encrypted может быть NULL
    auto enc_col = stmt.getColumn("encrypted");
    if (!enc_col.isNull()) {
        auto blob_ptr  = static_cast<const uint8_t*>(enc_col.getBlob());
        auto blob_size = enc_col.getBytes();
        m.encrypted.assign(blob_ptr, blob_ptr + blob_size);
    }

    m.status     = static_cast<MessageStatus>(stmt.getColumn("status").getInt());
    m.msg_uuid   = stmt.getColumn("msg_uuid").getString();
    m.created_at = stmt.getColumn("created_at").getString();
    return m;
}

} // namespace lm::storage
