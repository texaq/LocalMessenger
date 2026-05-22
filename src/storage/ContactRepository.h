#pragma once
// ═══════════════════════════════════════════════════════════════════════
// ContactRepository.h — CRUD операции для таблицы contacts
// ═══════════════════════════════════════════════════════════════════════

#include "Database.h"
#include <optional>
#include <string>
#include <vector>

namespace lm::storage {

struct Contact {
    int64_t     id          = 0;
    std::string username;
    std::string ip;
    uint16_t    port        = 7777;
    std::string public_key;
    std::string fingerprint;
    std::string avatar_path;
    bool        is_blocked  = false;
    std::string created_at;
    std::string updated_at;
};

class ContactRepository {
public:
    explicit ContactRepository(Database& db);

    /// Добавить или обновить контакт (upsert по public_key)
    int64_t upsert(const Contact& contact);

    /// Получить контакт по id
    std::optional<Contact> find_by_id(int64_t id) const;

    /// Получить контакт по IP
    std::optional<Contact> find_by_ip(const std::string& ip) const;

    /// Получить контакт по public_key
    std::optional<Contact> find_by_public_key(const std::string& pub_key) const;

    /// Получить все контакты
    std::vector<Contact> all() const;

    /// Удалить контакт по id
    void remove(int64_t id);

    /// Обновить IP-адрес и порт контакта
    void update_address(int64_t id, const std::string& ip, uint16_t port);

    /// Заблокировать / разблокировать контакт
    void set_blocked(int64_t id, bool blocked);

private:
    Contact row_to_contact(SQLite::Statement& stmt) const;

    Database& db_;
};

} // namespace lm::storage
