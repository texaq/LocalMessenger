// ═══════════════════════════════════════════════════════════════════════
// ChatManager.cpp — Реализация менеджера чата
// ═══════════════════════════════════════════════════════════════════════

#include "ChatManager.h"
#include "core/Network/Protocol.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <random>
#include <sstream>
#include <iomanip>

namespace lm::chat {

using json = nlohmann::json;

ChatManager::ChatManager(net::NetworkManager& net,
                          crypto::CryptoManager& crypto,
                          storage::Database& db,
                          const crypto::KeyPair& my_keys,
                          const std::string& my_username)
    : net_(net)
    , crypto_(crypto)
    , db_(db)
    , contacts_(db)
    , messages_(db)
    , my_keys_(my_keys)
    , my_username_(my_username)
    , my_pub_hex_(crypto::CryptoManager::key_to_hex(my_keys.public_key)) {

    // Подписываемся на входящие пакеты от NetworkManager
    net_.set_on_message([this](const std::string& ip, net::Packet pkt) {
        handle_packet(ip, std::move(pkt));
    });
}

// ── Отправка текстового сообщения ─────────────────────────────────────
std::string ChatManager::send_text(const std::string& peer_ip,
                                    const std::string& text) {
    auto contact = contacts_.find_by_ip(peer_ip);
    if (!contact) {
        spdlog::warn("ChatManager: контакт {} не найден", peer_ip);
        return "";
    }

    ChatMessage msg;
    msg.uuid         = generate_uuid();
    msg.sender_id    = my_pub_hex_;
    msg.receiver_id  = contact->public_key;
    msg.direction    = Direction::OUTGOING;
    msg.content_type = ContentType::TEXT;
    msg.text         = text;
    msg.status       = Status::SENT;
    msg.timestamp    = ChatMessage::now_ms();
    msg.sender_name  = my_username_;

    // Шифруем и отправляем
    auto encrypted = encrypt_message(msg, contact->public_key);
    net_.send_to(peer_ip, net::make_packet(net::PacketType::MSG_TEXT, encrypted));

    // Сохраняем в БД
    storage::Message db_msg;
    db_msg.contact_id = contact->id;
    db_msg.direction  = storage::MessageDirection::OUTGOING;
    db_msg.msg_type   = storage::MessageType::TEXT;
    db_msg.content    = text;
    db_msg.status     = storage::MessageStatus::SENT;
    db_msg.msg_uuid   = msg.uuid;
    messages_.insert(db_msg);

    spdlog::info("Отправлено → {}: {}", peer_ip, text);
    return msg.uuid;
}

// ── Отправка индикатора набора текста ──────────────────────────────────
void ChatManager::send_typing(const std::string& peer_ip, bool is_typing) {
    json j;
    j["typing"]    = is_typing;
    j["sender_id"] = my_pub_hex_;

    std::string data = j.dump();
    net_.send_to(peer_ip, net::make_packet(net::PacketType::MSG_TYPING, data));
}

// ── Подтверждение прочтения ───────────────────────────────────────────
void ChatManager::send_read_receipt(const std::string& peer_ip) {
    auto contact = contacts_.find_by_ip(peer_ip);
    if (!contact) return;

    // Помечаем все входящие как прочитанные
    messages_.mark_all_read(contact->id);

    json j;
    j["status"]    = "read";
    j["sender_id"] = my_pub_hex_;

    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::MSG_RECEIPT,
        j.dump()));
}

// ── Отправка реакции ──────────────────────────────────────────────────
void ChatManager::send_reaction(const std::string& peer_ip,
                                 const std::string& msg_uuid,
                                 const std::string& emoji) {
    json j;
    j["msg_uuid"]  = msg_uuid;
    j["emoji"]     = emoji;
    j["sender_id"] = my_pub_hex_;

    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::MSG_REACTION,
        j.dump()));
}

// ── История и поиск ───────────────────────────────────────────────────
std::vector<ChatMessage> ChatManager::get_history(const std::string& peer_ip,
                                                    int limit, int offset) {
    auto contact = contacts_.find_by_ip(peer_ip);
    if (!contact) return {};

    auto db_msgs = messages_.get_by_contact(contact->id, limit, offset);

    std::vector<ChatMessage> result;
    result.reserve(db_msgs.size());

    for (auto& m : db_msgs) {
        ChatMessage cm;
        cm.uuid         = m.msg_uuid;
        cm.text         = m.content;
        cm.direction    = (m.direction == storage::MessageDirection::INCOMING)
                            ? Direction::INCOMING : Direction::OUTGOING;
        cm.status       = static_cast<Status>(static_cast<int>(m.status));
        cm.content_type = static_cast<ContentType>(static_cast<int>(m.msg_type) - 1);
        cm.sender_name  = (cm.direction == Direction::INCOMING)
                            ? contact->username : my_username_;
        result.push_back(std::move(cm));
    }
    return result;
}

std::vector<ChatMessage> ChatManager::search(const std::string& query, int limit) {
    auto db_msgs = messages_.search(query, limit);

    std::vector<ChatMessage> result;
    for (auto& m : db_msgs) {
        ChatMessage cm;
        cm.uuid = m.msg_uuid;
        cm.text = m.content;
        cm.direction = (m.direction == storage::MessageDirection::INCOMING)
                        ? Direction::INCOMING : Direction::OUTGOING;
        result.push_back(std::move(cm));
    }
    return result;
}

int ChatManager::unread_count(const std::string& peer_ip) {
    auto contact = contacts_.find_by_ip(peer_ip);
    if (!contact) return 0;
    return messages_.unread_count(contact->id);
}

void ChatManager::mark_all_read(const std::string& peer_ip) {
    auto contact = contacts_.find_by_ip(peer_ip);
    if (!contact) return;
    messages_.mark_all_read(contact->id);
}

// ── Обработка входящих пакетов ────────────────────────────────────────
void ChatManager::handle_packet(const std::string& ip, net::Packet pkt) {
    switch (pkt.header.type) {
        case net::PacketType::MSG_TEXT: {
            auto contact = contacts_.find_by_ip(ip);
            std::string peer_pub = contact ? contact->public_key : "";

            // Пытаемся расшифровать
            auto decrypted = decrypt_message(pkt.payload, peer_pub);
            ChatMessage msg;

            if (decrypted) {
                msg = *decrypted;
            } else {
                // Fallback: незашифрованное сообщение (совместимость с этапом 1)
                msg.text = std::string(pkt.payload.begin(), pkt.payload.end());
                msg.uuid = generate_uuid();
                msg.timestamp = ChatMessage::now_ms();
            }

            msg.direction = Direction::INCOMING;
            msg.status    = Status::DELIVERED;

            // Сохраняем в БД
            if (contact) {
                storage::Message db_msg;
                db_msg.contact_id = contact->id;
                db_msg.direction  = storage::MessageDirection::INCOMING;
                db_msg.msg_type   = storage::MessageType::TEXT;
                db_msg.content    = msg.text;
                db_msg.status     = storage::MessageStatus::DELIVERED;
                db_msg.msg_uuid   = msg.uuid;
                messages_.insert(db_msg);
                msg.sender_name = contact->username;
            }

            spdlog::info("Сообщение от {}: {}", ip, msg.text);

            // Отправляем подтверждение доставки
            json receipt;
            receipt["status"]   = "delivered";
            receipt["msg_uuid"] = msg.uuid;
            receipt["sender_id"] = my_pub_hex_;
            net_.send_to(ip, net::make_packet(
                net::PacketType::MSG_RECEIPT, receipt.dump()));

            if (on_message_) on_message_(msg);
            break;
        }

        case net::PacketType::MSG_RECEIPT: {
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);

                std::string status_str = j.value("status", "");
                std::string msg_uuid   = j.value("msg_uuid", "");

                Status new_status = Status::DELIVERED;
                if (status_str == "read") new_status = Status::READ;

                if (!msg_uuid.empty()) {
                    auto st = static_cast<storage::MessageStatus>(
                        static_cast<int>(new_status));
                    messages_.update_status(msg_uuid, st);
                }

                if (on_status_) on_status_(msg_uuid, new_status);
            } catch (...) {}
            break;
        }

        case net::PacketType::MSG_TYPING: {
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);
                bool typing = j.value("typing", false);
                if (on_typing_) on_typing_(ip, typing);
            } catch (...) {}
            break;
        }

        case net::PacketType::MSG_REACTION: {
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);

                Reaction r;
                r.emoji     = j.value("emoji", "");
                r.sender_id = j.value("sender_id", "");
                std::string msg_uuid = j.value("msg_uuid", "");

                if (on_reaction_) on_reaction_(msg_uuid, r);
            } catch (...) {}
            break;
        }

        case net::PacketType::HANDSHAKE: {
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);

                std::string username = j.value("username", "");
                std::string pub_key  = j.value("public_key", "");

                if (!pub_key.empty()) {
                    storage::Contact c;
                    c.username   = username;
                    c.ip         = ip;
                    c.public_key = pub_key;
                    auto key = crypto::CryptoManager::hex_to_key(pub_key);
                    if (key) {
                        c.fingerprint = crypto::CryptoManager::fingerprint(*key);
                    }
                    contacts_.upsert(c);
                    spdlog::info("Handshake от {} ({})", username, ip);
                }
            } catch (...) {}
            break;
        }

        case net::PacketType::KEY_EXCHANGE: {
            // Обмен ключами — сохраняем публичный ключ пира
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);
                std::string pub_key = j.value("public_key", "");

                if (!pub_key.empty()) {
                    auto contact = contacts_.find_by_ip(ip);
                    if (contact) {
                        storage::Contact updated = *contact;
                        updated.public_key = pub_key;
                        auto key = crypto::CryptoManager::hex_to_key(pub_key);
                        if (key) {
                            updated.fingerprint = crypto::CryptoManager::fingerprint(*key);
                        }
                        contacts_.upsert(updated);
                    }
                }
            } catch (...) {}
            break;
        }

        default:
            // PING/PONG обрабатываются в NetworkManager
            break;
    }
}

// ── Шифрование ────────────────────────────────────────────────────────
crypto::KeyBytes ChatManager::get_shared_key(const std::string& peer_pub_hex) {
    std::lock_guard<std::mutex> lock(keys_mutex_);

    auto it = shared_keys_.find(peer_pub_hex);
    if (it != shared_keys_.end()) {
        return it->second;
    }

    auto peer_key = crypto::CryptoManager::hex_to_key(peer_pub_hex);
    if (!peer_key) {
        spdlog::warn("Невалидный публичный ключ: {}", peer_pub_hex);
        return {};
    }

    auto shared = crypto_.compute_shared_key(my_keys_.secret_key, *peer_key);
    shared_keys_[peer_pub_hex] = shared;
    return shared;
}

std::vector<uint8_t> ChatManager::encrypt_message(
    const ChatMessage& msg, const std::string& peer_pub_hex) {

    if (peer_pub_hex.empty()) {
        // Нет ключа — отправляем открытым текстом (совместимость)
        return std::vector<uint8_t>(msg.text.begin(), msg.text.end());
    }

    json j;
    j["uuid"]         = msg.uuid;
    j["sender_id"]    = msg.sender_id;
    j["text"]         = msg.text;
    j["timestamp"]    = msg.timestamp;
    j["content_type"] = static_cast<int>(msg.content_type);
    j["sender_name"]  = msg.sender_name;
    if (!msg.reply_to_uuid.empty()) {
        j["reply_to"] = msg.reply_to_uuid;
    }

    std::string plaintext = j.dump();
    std::vector<uint8_t> pt(plaintext.begin(), plaintext.end());

    auto shared_key = get_shared_key(peer_pub_hex);

    // Маркер шифрования: первый байт = 0x01 (encrypted), затем ciphertext
    auto encrypted = crypto_.encrypt(shared_key, pt);
    std::vector<uint8_t> result;
    result.reserve(1 + encrypted.size());
    result.push_back(0x01);  // флаг: зашифровано
    result.insert(result.end(), encrypted.begin(), encrypted.end());
    return result;
}

std::optional<ChatMessage> ChatManager::decrypt_message(
    const std::vector<uint8_t>& data, const std::string& peer_pub_hex) {

    if (data.empty() || data[0] != 0x01 || peer_pub_hex.empty()) {
        return std::nullopt;  // не зашифровано или нет ключа
    }

    std::vector<uint8_t> ciphertext(data.begin() + 1, data.end());
    auto shared_key = get_shared_key(peer_pub_hex);
    auto plaintext = crypto_.decrypt(shared_key, ciphertext);

    if (!plaintext) {
        spdlog::warn("Не удалось расшифровать сообщение");
        return std::nullopt;
    }

    try {
        std::string raw(plaintext->begin(), plaintext->end());
        auto j = json::parse(raw);

        ChatMessage msg;
        msg.uuid         = j.value("uuid", "");
        msg.sender_id    = j.value("sender_id", "");
        msg.text         = j.value("text", "");
        msg.timestamp    = j.value("timestamp", static_cast<int64_t>(0));
        msg.content_type = static_cast<ContentType>(j.value("content_type", 0));
        msg.sender_name  = j.value("sender_name", "");
        msg.reply_to_uuid = j.value("reply_to", "");

        return msg;
    } catch (const std::exception& e) {
        spdlog::warn("Ошибка парсинга расшифрованного сообщения: {}", e.what());
        return std::nullopt;
    }
}

// ── Генерация UUID v4 ─────────────────────────────────────────────────
std::string ChatManager::generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);

    // Формат UUID v4: 8-4-4-4-12
    // Версия 4: bits 48-51 = 0100
    // Variant:  bits 64-65 = 10
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << (a >> 32) << '-';
    ss << std::setw(4) << ((a >> 16) & 0xFFFF) << '-';
    ss << std::setw(4) << (a & 0xFFFF) << '-';
    ss << std::setw(4) << (b >> 48) << '-';
    ss << std::setw(12) << (b & 0xFFFFFFFFFFFFULL);

    return ss.str();
}

} // namespace lm::chat
