#pragma once
// ═══════════════════════════════════════════════════════════════════════
// ChatManager.h — Менеджер чата (логика отправки/получения сообщений)
//
// Связывает сетевой уровень, шифрование и хранилище.
// Предоставляет callback-и для UI (через сигналы или std::function).
// ═══════════════════════════════════════════════════════════════════════

#include "ChatMessage.h"
#include "core/Network/NetworkManager.h"
#include "core/Crypto/CryptoManager.h"
#include "storage/Database.h"
#include "storage/ContactRepository.h"
#include "storage/MessageRepository.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lm::chat {

class ChatManager {
public:
    // ── Callback-и для UI ─────────────────────────────────────────────
    using OnMessageCb     = std::function<void(const ChatMessage&)>;
    using OnStatusCb      = std::function<void(const std::string& uuid, Status)>;
    using OnTypingCb      = std::function<void(const std::string& peer_ip, bool typing)>;
    using OnReactionCb    = std::function<void(const std::string& msg_uuid,
                                                const Reaction& reaction)>;

    ChatManager(net::NetworkManager& net,
                crypto::CryptoManager& crypto,
                storage::Database& db,
                const crypto::KeyPair& my_keys,
                const std::string& my_username);

    /// Отправить текстовое сообщение
    /// @return UUID отправленного сообщения
    std::string send_text(const std::string& peer_ip,
                           const std::string& text);

    /// Отправить индикатор набора текста
    void send_typing(const std::string& peer_ip, bool is_typing);

    /// Отправить подтверждение прочтения для всех непрочитанных от пира
    void send_read_receipt(const std::string& peer_ip);

    /// Отправить реакцию на сообщение
    void send_reaction(const std::string& peer_ip,
                        const std::string& msg_uuid,
                        const std::string& emoji);

    /// Получить историю чата с контактом
    std::vector<ChatMessage> get_history(const std::string& peer_ip,
                                          int limit = 50, int offset = 0);

    /// Поиск по истории
    std::vector<ChatMessage> search(const std::string& query, int limit = 50);

    /// Количество непрочитанных от контакта
    int unread_count(const std::string& peer_ip);

    /// Пометить все от контакта как прочитанные
    void mark_all_read(const std::string& peer_ip);

    // ── Установить callback-и ─────────────────────────────────────────
    void set_on_message(OnMessageCb cb)   { on_message_ = std::move(cb); }
    void set_on_status(OnStatusCb cb)     { on_status_ = std::move(cb); }
    void set_on_typing(OnTypingCb cb)     { on_typing_ = std::move(cb); }
    void set_on_reaction(OnReactionCb cb) { on_reaction_ = std::move(cb); }

private:
    /// Обработка входящего пакета от NetworkManager
    void handle_packet(const std::string& ip, net::Packet pkt);

    /// Вычислить shared key для пира (кешируется)
    crypto::KeyBytes get_shared_key(const std::string& peer_public_hex);

    /// Генерация UUID v4
    static std::string generate_uuid();

    /// Сериализация ChatMessage → JSON → зашифрованный payload
    std::vector<uint8_t> encrypt_message(const ChatMessage& msg,
                                          const std::string& peer_pub_hex);

    /// Расшифровка payload → JSON → ChatMessage
    std::optional<ChatMessage> decrypt_message(const std::vector<uint8_t>& data,
                                                const std::string& peer_pub_hex);

    net::NetworkManager&      net_;
    crypto::CryptoManager&    crypto_;
    storage::Database&        db_;
    storage::ContactRepository contacts_;
    storage::MessageRepository messages_;

    crypto::KeyPair           my_keys_;
    std::string               my_username_;
    std::string               my_pub_hex_;

    // Кеш shared keys: peer_public_hex → shared_key
    mutable std::mutex                                       keys_mutex_;
    std::unordered_map<std::string, crypto::KeyBytes>        shared_keys_;

    OnMessageCb  on_message_;
    OnStatusCb   on_status_;
    OnTypingCb   on_typing_;
    OnReactionCb on_reaction_;
};

} // namespace lm::chat
