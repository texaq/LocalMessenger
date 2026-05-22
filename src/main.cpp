// ═══════════════════════════════════════════════════════════════════════
// main.cpp — Точка входа LocalMessenger (Этап 1: консольный режим)
//
// Запускает:
//   1. Инициализацию libsodium и генерацию ключей X25519
//   2. SQLite базу данных с миграциями
//   3. Загрузку настроек из settings.json
//   4. TCP-сервер + UDP-discovery через NetworkManager
//   5. Простой интерактивный цикл для тестирования отправки сообщений
// ═══════════════════════════════════════════════════════════════════════

#include "core/Crypto/CryptoManager.h"
#include "core/Network/NetworkManager.h"
#include "core/Network/Protocol.h"
#include "storage/Database.h"
#include "storage/ContactRepository.h"
#include "storage/MessageRepository.h"
#include "storage/SettingsManager.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <atomic>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Флаг для корректного завершения по Ctrl+C
static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running = false;
}

/// Определяем папку данных приложения (кроссплатформенно)
static std::string get_data_dir() {
#ifdef _WIN32
    // %APPDATA%\LocalMessenger
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "\\LocalMessenger";
    }
    return "LocalMessenger";
#else
    // ~/.local/share/LocalMessenger
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.local/share/LocalMessenger";
    }
    return "LocalMessenger";
#endif
}

/// Определяем папку загрузок
static std::string get_download_dir() {
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
    if (profile) {
        return std::string(profile) + "\\Downloads\\MessengerFiles";
    }
    return "Downloads";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/Downloads/MessengerFiles";
    }
    return "Downloads";
#endif
}

int main(int argc, char* argv[]) {
    // ── Логирование: консоль + файл с ротацией ───────────────────────
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);

    std::string data_dir = get_data_dir();
    fs::create_directories(data_dir);

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        data_dir + "/messenger.log",
        5 * 1024 * 1024,  // 5 MiB
        3                 // 3 файла ротации
    );
    file_sink->set_level(spdlog::level::debug);

    auto logger = std::make_shared<spdlog::logger>(
        "main", spdlog::sinks_init_list{console_sink, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::info("=== LocalMessenger v0.1.0 ===");
    spdlog::info("Папка данных: {}", data_dir);

    // ── Криптография ──────────────────────────────────────────────────
    if (!lm::crypto::CryptoManager::initialize()) {
        spdlog::critical("Не удалось инициализировать libsodium");
        return 1;
    }

    lm::crypto::CryptoManager crypto;
    auto keypair = crypto.generate_keypair();
    std::string pub_key_hex = lm::crypto::CryptoManager::key_to_hex(keypair.public_key);
    std::string fp = lm::crypto::CryptoManager::fingerprint(keypair.public_key);

    spdlog::info("Публичный ключ: {}", pub_key_hex);
    spdlog::info("Fingerprint:    {}", fp);

    // ── База данных ───────────────────────────────────────────────────
    std::string db_path = data_dir + "/messages.db";
    lm::storage::Database database(db_path);

    lm::storage::ContactRepository contacts(database);
    lm::storage::MessageRepository messages(database);

    // ── Настройки ─────────────────────────────────────────────────────
    std::string settings_path = data_dir + "/settings.json";
    lm::storage::SettingsManager settings(settings_path, database);

    auto cfg = settings.get();
    if (cfg.data_dir.empty()) {
        cfg.data_dir     = data_dir;
        cfg.download_dir = get_download_dir();
        settings.update(cfg);
    }

    // Создаём папку загрузок
    fs::create_directories(cfg.download_dir);
    // Создаём папку медиа
    fs::create_directories(data_dir + "/media");

    // Имя пользователя: из аргументов или из настроек
    std::string username = cfg.username;
    if (argc > 1) {
        username = argv[1];
        cfg.username = username;
        settings.update(cfg);
    }

    spdlog::info("Пользователь: {}", username);

    // ── Сетевой менеджер ──────────────────────────────────────────────
    lm::net::NetworkManager net_mgr;

    // Callback: получен пакет от пира
    net_mgr.set_on_message([&](const std::string& ip, lm::net::Packet pkt) {
        switch (pkt.header.type) {
            case lm::net::PacketType::MSG_TEXT: {
                std::string text(pkt.payload.begin(), pkt.payload.end());
                spdlog::info("[{}] Сообщение от {}: {}", username, ip, text);

                // Сохраняем в БД
                auto contact = contacts.find_by_ip(ip);
                if (contact) {
                    lm::storage::Message msg;
                    msg.contact_id = contact->id;
                    msg.direction  = lm::storage::MessageDirection::INCOMING;
                    msg.content    = text;
                    msg.msg_uuid   = "";  // TODO: извлечь из payload
                    messages.insert(msg);
                }

                // Отправляем подтверждение доставки
                net_mgr.send_to(ip, lm::net::make_packet(
                    lm::net::PacketType::MSG_RECEIPT,
                    std::string("delivered")));
                break;
            }

            case lm::net::PacketType::MSG_RECEIPT: {
                std::string status(pkt.payload.begin(), pkt.payload.end());
                spdlog::info("Подтверждение от {}: {}", ip, status);
                break;
            }

            case lm::net::PacketType::MSG_TYPING: {
                spdlog::info("{} печатает...", ip);
                break;
            }

            case lm::net::PacketType::HANDSHAKE: {
                std::string data(pkt.payload.begin(), pkt.payload.end());
                spdlog::info("Handshake от {}: {}", ip, data);
                break;
            }

            default:
                spdlog::debug("Пакет типа {} от {}", 
                             static_cast<int>(pkt.header.type), ip);
                break;
        }
    });

    // Callback: обнаружен новый пир через UDP
    net_mgr.set_on_peer_found([&](const lm::net::PeerInfo& info) {
        spdlog::info("Пир найден: {} ({}:{})", info.username, info.ip, info.port);

        // Сохраняем/обновляем контакт в БД
        lm::storage::Contact c;
        c.username   = info.username;
        c.ip         = info.ip;
        c.port       = info.port;
        c.public_key = info.public_key;
        if (!info.public_key.empty()) {
            auto key = lm::crypto::CryptoManager::hex_to_key(info.public_key);
            if (key) {
                c.fingerprint = lm::crypto::CryptoManager::fingerprint(*key);
            }
        }
        contacts.upsert(c);

        // Автоматически подключаемся к обнаруженному пиру
        if (!net_mgr.is_connected(info.ip)) {
            net_mgr.connect_to(info.ip, info.port);
        }
    });

    net_mgr.set_on_peer_lost([&](const std::string& ip) {
        spdlog::info("Пир потерян: {}", ip);
    });

    net_mgr.set_on_peer_connected([&](const std::string& ip) {
        spdlog::info("TCP-соединение установлено с {}", ip);

        // Отправляем handshake с нашим именем и публичным ключом
        json hs;
        hs["username"]   = username;
        hs["public_key"] = pub_key_hex;
        hs["version"]    = "0.1.0";

        net_mgr.send_to(ip, lm::net::make_packet(
            lm::net::PacketType::HANDSHAKE, hs.dump()));
    });

    // Запуск сетевого менеджера
    net_mgr.start(username, cfg.tcp_port, cfg.udp_port, pub_key_hex);

    // ── Обработка сигналов ────────────────────────────────────────────
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Интерактивный цикл (для тестирования) ─────────────────────────
    spdlog::info("Готов. Команды: /peers, /send <ip> <text>, /history <ip>, /search <text>, /quit");

    while (g_running) {
        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line == "/quit" || line == "/exit") {
            break;
        }

        if (line == "/peers") {
            auto peers = net_mgr.discovered_peers();
            if (peers.empty()) {
                std::cout << "Нет обнаруженных пиров\n";
            } else {
                for (auto& p : peers) {
                    std::cout << "  " << p.username << " — " << p.ip
                              << ":" << p.port;
                    if (net_mgr.is_connected(p.ip)) {
                        std::cout << " [TCP]";
                    }
                    std::cout << "\n";
                }
            }
            continue;
        }

        if (line.substr(0, 6) == "/send ") {
            auto rest = line.substr(6);
            auto space = rest.find(' ');
            if (space == std::string::npos) {
                std::cout << "Использование: /send <ip> <текст>\n";
                continue;
            }
            std::string ip   = rest.substr(0, space);
            std::string text = rest.substr(space + 1);

            auto pkt = lm::net::make_packet(lm::net::PacketType::MSG_TEXT, text);
            net_mgr.send_to(ip, pkt);

            // Сохраняем в БД
            auto contact = contacts.find_by_ip(ip);
            if (contact) {
                lm::storage::Message msg;
                msg.contact_id = contact->id;
                msg.direction  = lm::storage::MessageDirection::OUTGOING;
                msg.content    = text;
                messages.insert(msg);
            }

            std::cout << "Отправлено → " << ip << ": " << text << "\n";
            continue;
        }

        if (line.substr(0, 9) == "/history ") {
            std::string ip = line.substr(9);
            auto contact = contacts.find_by_ip(ip);
            if (!contact) {
                std::cout << "Контакт не найден: " << ip << "\n";
                continue;
            }
            auto msgs = messages.get_by_contact(contact->id, 20);
            for (auto& m : msgs) {
                std::string dir = (m.direction == lm::storage::MessageDirection::INCOMING)
                                    ? "<< " : ">> ";
                std::cout << "  " << m.created_at << " " << dir << m.content << "\n";
            }
            continue;
        }

        if (line.substr(0, 8) == "/search ") {
            std::string query = line.substr(8);
            auto results = messages.search(query);
            if (results.empty()) {
                std::cout << "Ничего не найдено\n";
            } else {
                for (auto& m : results) {
                    std::cout << "  [" << m.created_at << "] " << m.content << "\n";
                }
            }
            continue;
        }

        std::cout << "Неизвестная команда. /peers, /send, /history, /search, /quit\n";
    }

    // ── Завершение ────────────────────────────────────────────────────
    spdlog::info("Завершение работы...");
    net_mgr.stop();
    spdlog::info("LocalMessenger остановлен");

    return 0;
}
