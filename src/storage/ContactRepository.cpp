// ═══════════════════════════════════════════════════════════════════════
// ContactRepository.cpp — Реализация CRUD для контактов
// ═══════════════════════════════════════════════════════════════════════

#include "ContactRepository.h"
#include <spdlog/spdlog.h>

namespace lm::storage {

ContactRepository::ContactRepository(Database& db) : db_(db) {}

int64_t ContactRepository::upsert(const Contact& c) {
    // INSERT OR REPLACE — если public_key уже есть, обновляем запись
    SQLite::Statement stmt(db_.db(), R"SQL(
        INSERT INTO contacts (username, ip, port, public_key, fingerprint, avatar_path, is_blocked)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(public_key) DO UPDATE SET
            username    = excluded.username,
            ip          = excluded.ip,
            port        = excluded.port,
            fingerprint = excluded.fingerprint,
            avatar_path = excluded.avatar_path,
            updated_at  = datetime('now')
    )SQL");

    stmt.bind(1, c.username);
    stmt.bind(2, c.ip);
    stmt.bind(3, static_cast<int>(c.port));
    stmt.bind(4, c.public_key);
    stmt.bind(5, c.fingerprint);
    stmt.bind(6, c.avatar_path);
    stmt.bind(7, c.is_blocked ? 1 : 0);
    stmt.exec();

    return db_.db().getLastInsertRowid();
}

std::optional<Contact> ContactRepository::find_by_id(int64_t id) const {
    SQLite::Statement stmt(db_.db(),
        "SELECT * FROM contacts WHERE id = ?");
    stmt.bind(1, id);

    if (stmt.executeStep()) {
        return row_to_contact(stmt);
    }
    return std::nullopt;
}

std::optional<Contact> ContactRepository::find_by_ip(const std::string& ip) const {
    SQLite::Statement stmt(db_.db(),
        "SELECT * FROM contacts WHERE ip = ? ORDER BY updated_at DESC LIMIT 1");
    stmt.bind(1, ip);

    if (stmt.executeStep()) {
        return row_to_contact(stmt);
    }
    return std::nullopt;
}

std::optional<Contact> ContactRepository::find_by_public_key(
    const std::string& pub_key) const {
    SQLite::Statement stmt(db_.db(),
        "SELECT * FROM contacts WHERE public_key = ?");
    stmt.bind(1, pub_key);

    if (stmt.executeStep()) {
        return row_to_contact(stmt);
    }
    return std::nullopt;
}

std::vector<Contact> ContactRepository::all() const {
    std::vector<Contact> result;
    SQLite::Statement stmt(db_.db(),
        "SELECT * FROM contacts ORDER BY username");

    while (stmt.executeStep()) {
        result.push_back(row_to_contact(stmt));
    }
    return result;
}

void ContactRepository::remove(int64_t id) {
    SQLite::Statement stmt(db_.db(),
        "DELETE FROM contacts WHERE id = ?");
    stmt.bind(1, id);
    stmt.exec();
}

void ContactRepository::update_address(int64_t id,
                                        const std::string& ip,
                                        uint16_t port) {
    SQLite::Statement stmt(db_.db(), R"SQL(
        UPDATE contacts SET ip = ?, port = ?, updated_at = datetime('now')
        WHERE id = ?
    )SQL");
    stmt.bind(1, ip);
    stmt.bind(2, static_cast<int>(port));
    stmt.bind(3, id);
    stmt.exec();
}

void ContactRepository::set_blocked(int64_t id, bool blocked) {
    SQLite::Statement stmt(db_.db(),
        "UPDATE contacts SET is_blocked = ? WHERE id = ?");
    stmt.bind(1, blocked ? 1 : 0);
    stmt.bind(2, id);
    stmt.exec();
}

Contact ContactRepository::row_to_contact(SQLite::Statement& stmt) const {
    Contact c;
    c.id          = stmt.getColumn("id").getInt64();
    c.username    = stmt.getColumn("username").getString();
    c.ip          = stmt.getColumn("ip").getString();
    c.port        = static_cast<uint16_t>(stmt.getColumn("port").getInt());
    c.public_key  = stmt.getColumn("public_key").getString();
    c.fingerprint = stmt.getColumn("fingerprint").getString();
    c.avatar_path = stmt.getColumn("avatar_path").getString();
    c.is_blocked  = stmt.getColumn("is_blocked").getInt() != 0;
    c.created_at  = stmt.getColumn("created_at").getString();
    c.updated_at  = stmt.getColumn("updated_at").getString();
    return c;
}

} // namespace lm::storage
