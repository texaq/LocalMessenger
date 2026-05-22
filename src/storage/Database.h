#pragma once
// ═══════════════════════════════════════════════════════════════════════
// Database.h — Инициализация SQLite базы данных
//
// Создаёт и открывает файл БД, выполняет миграции (CREATE TABLE),
// предоставляет доступ к SQLite::Database.
//
// Схема:
//   contacts — список контактов (username, IP, public_key, avatar)
//   messages — история сообщений (sender, receiver, text, timestamp, status)
//   media_files — медиафайлы (hash, path, mime_type)
//   settings — ключ-значение для настроек
// ═══════════════════════════════════════════════════════════════════════

#include <SQLiteCpp/SQLiteCpp.h>
#include <memory>
#include <string>

namespace lm::storage {

class Database {
public:
    /// @param db_path — путь к файлу БД (напр. messages.db)
    explicit Database(const std::string& db_path);

    /// Получить SQLite::Database для выполнения запросов
    SQLite::Database& db() { return *db_; }

    /// Выполнить миграции (создать таблицы, если не существуют)
    void migrate();

private:
    std::unique_ptr<SQLite::Database> db_;
};

} // namespace lm::storage
