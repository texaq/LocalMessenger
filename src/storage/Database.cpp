// ═══════════════════════════════════════════════════════════════════════
// Database.cpp — Инициализация SQLite и миграции
// ═══════════════════════════════════════════════════════════════════════

#include "Database.h"
#include <spdlog/spdlog.h>

namespace lm::storage {

Database::Database(const std::string& db_path)
    : db_(std::make_unique<SQLite::Database>(
          db_path,
          SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)) {
    // WAL-режим для лучшей производительности при параллельном чтении
    db_->exec("PRAGMA journal_mode=WAL");
    db_->exec("PRAGMA foreign_keys=ON");

    spdlog::info("SQLite БД открыта: {}", db_path);

    migrate();
}

void Database::migrate() {
    // ── contacts ──────────────────────────────────────────────────────
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS contacts (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            username    TEXT    NOT NULL,
            ip          TEXT    NOT NULL,
            port        INTEGER NOT NULL DEFAULT 7777,
            public_key  TEXT    NOT NULL DEFAULT '',
            fingerprint TEXT    NOT NULL DEFAULT '',
            avatar_path TEXT    NOT NULL DEFAULT '',
            is_blocked  INTEGER NOT NULL DEFAULT 0,
            created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
            updated_at  TEXT    NOT NULL DEFAULT (datetime('now')),
            UNIQUE(public_key)
        )
    )SQL");

    // ── messages ──────────────────────────────────────────────────────
    // contact_id ссылается на contacts.id
    // direction: 0 = входящее, 1 = исходящее
    // status: 0 = sent, 1 = delivered, 2 = read
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS messages (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            contact_id  INTEGER NOT NULL,
            direction   INTEGER NOT NULL DEFAULT 1,
            msg_type    INTEGER NOT NULL DEFAULT 1,
            content     TEXT    NOT NULL DEFAULT '',
            encrypted   BLOB,
            status      INTEGER NOT NULL DEFAULT 0,
            msg_uuid    TEXT    NOT NULL DEFAULT '',
            created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY (contact_id) REFERENCES contacts(id)
        )
    )SQL");

    // Индекс для быстрого поиска по тексту и хронологии
    db_->exec(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_messages_contact
        ON messages(contact_id, created_at)
    )SQL");

    db_->exec(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_messages_uuid
        ON messages(msg_uuid)
    )SQL");

    // ── media_files ───────────────────────────────────────────────────
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS media_files (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            message_id  INTEGER,
            file_hash   TEXT    NOT NULL,
            file_name   TEXT    NOT NULL DEFAULT '',
            file_path   TEXT    NOT NULL DEFAULT '',
            file_size   INTEGER NOT NULL DEFAULT 0,
            mime_type   TEXT    NOT NULL DEFAULT '',
            created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY (message_id) REFERENCES messages(id)
        )
    )SQL");

    // ── settings (key-value) ──────────────────────────────────────────
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS settings (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL DEFAULT ''
        )
    )SQL");

    spdlog::info("Миграция БД завершена");
}

} // namespace lm::storage
