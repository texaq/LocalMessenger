// ═══════════════════════════════════════════════════════════════════════
// main.cpp — Точка входа LocalMessenger (Этапы 1-5: консольный режим)
//
// Запускает все модули:
//   1. Инициализация libsodium и генерация ключей X25519
//   2. SQLite база данных с миграциями
//   3. Настройки из settings.json
//   4. TCP-сервер + UDP-discovery (NetworkManager)
//   5. ChatManager (E2E шифрование, статусы)
//   6. FileTransferManager (передача файлов по TCP)
//   7. CallManager (голосовые/видеозвонки, сигнализация)
//   8. ScreenShareManager (демонстрация экрана)
// ═══════════════════════════════════════════════════════════════════════

#include "core/Crypto/CryptoManager.h"
#include "core/Network/NetworkManager.h"
#include "core/Network/Protocol.h"
#include "core/Chat/ChatManager.h"
#include "core/FileTransfer/FileTransferManager.h"
#include "core/Call/CallManager.h"
#include "core/ScreenShare/ScreenShareManager.h"
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

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running = false;
}

/// Папка данных приложения (кроссплатформенно)
static std::string get_data_dir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::string(appdata) + "\\LocalMessenger";
    return "LocalMessenger";
#else
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/.local/share/LocalMessenger";
    return "LocalMessenger";
#endif
}

/// Папка загрузок
static std::string get_download_dir() {
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
    if (profile) return std::string(profile) + "\\Downloads\\MessengerFiles";
    return "Downloads";
#else
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/Downloads/MessengerFiles";
    return "Downloads";
#endif
}

int main(int argc, char* argv[]) {
    // ── Логирование ───────────────────────────────────────────────────
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);

    std::string data_dir = get_data_dir();
    fs::create_directories(data_dir);

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        data_dir + "/messenger.log", 5 * 1024 * 1024, 3);
    file_sink->set_level(spdlog::level::debug);

    auto logger = std::make_shared<spdlog::logger>(
        "main", spdlog::sinks_init_list{console_sink, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::info("=== LocalMessenger v0.2.0 ===");
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

    fs::create_directories(cfg.download_dir);
    fs::create_directories(data_dir + "/media");

    std::string username = cfg.username;
    if (argc > 1) {
        username = argv[1];
        cfg.username = username;
        settings.update(cfg);
    }

    spdlog::info("Пользователь: {}", username);

    // ── Сетевой менеджер ──────────────────────────────────────────────
    lm::net::NetworkManager net_mgr;

    // ── ChatManager (Этап 2) ──────────────────────────────────────────
    lm::chat::ChatManager chat_mgr(net_mgr, crypto, database,
                                     keypair, username);

    chat_mgr.set_on_message([](const lm::chat::ChatMessage& msg) {
        std::cout << "\n[" << msg.sender_name << "] " << msg.text << "\n> "
                  << std::flush;
    });

    chat_mgr.set_on_status([](const std::string& uuid, lm::chat::Status status) {
        const char* s = (status == lm::chat::Status::READ) ? "прочитано"
                      : (status == lm::chat::Status::DELIVERED) ? "доставлено"
                      : "отправлено";
        spdlog::debug("Статус {}: {}", uuid, s);
    });

    chat_mgr.set_on_typing([](const std::string& ip, bool typing) {
        if (typing) {
            std::cout << "\n" << ip << " печатает...\n> " << std::flush;
        }
    });

    // ── FileTransferManager (Этап 3) ──────────────────────────────────
    lm::filetransfer::FileTransferManager file_mgr(
        net_mgr.io_context(), cfg.file_port);

    file_mgr.set_on_progress([](const lm::filetransfer::TransferInfo& info) {
        std::cout << "\r  [" << info.file_name << "] "
                  << static_cast<int>(info.progress() * 100) << "% "
                  << static_cast<int>(info.speed_kbps) << " KB/s"
                  << std::flush;
    });

    file_mgr.set_on_complete([](const lm::filetransfer::TransferInfo& info) {
        std::cout << "\n  Файл " << (info.is_sender ? "отправлен" : "принят")
                  << ": " << info.file_name << "\n> " << std::flush;
    });

    file_mgr.set_on_incoming([](const lm::filetransfer::TransferInfo& info) {
        std::cout << "\n  Входящий файл от " << info.peer_ip << ": "
                  << info.file_name << " ("
                  << info.file_size / 1024 << " KB)\n> " << std::flush;
    });

    file_mgr.start(cfg.download_dir);
    spdlog::info("FileTransfer на порту {}", cfg.file_port);

    // ── CallManager (Этап 4) ──────────────────────────────────────────
    lm::call::CallManager call_mgr(net_mgr, net_mgr.io_context());

    call_mgr.set_on_incoming([](const lm::call::CallInfo& info) {
        std::string type_str = (info.type == lm::call::CallType::VIDEO)
                                ? "видео" : "аудио";
        std::cout << "\n  Входящий " << type_str << " звонок от "
                  << info.peer_name << " (" << info.peer_ip << ")"
                  << "\n  /accept — принять, /decline — отклонить"
                  << "\n> " << std::flush;
    });

    call_mgr.set_on_state_change([](const lm::call::CallInfo& info) {
        switch (info.state) {
            case lm::call::CallState::ACTIVE:
                std::cout << "\n  Звонок активен с " << info.peer_ip
                          << "\n> " << std::flush;
                break;
            case lm::call::CallState::ENDED:
                std::cout << "\n  Звонок завершён ("
                          << info.duration_sec << " сек)\n> " << std::flush;
                break;
            case lm::call::CallState::DECLINED:
                std::cout << "\n  Звонок отклонён\n> " << std::flush;
                break;
            default:
                break;
        }
    });

    // ── ScreenShareManager (Этап 5) ───────────────────────────────────
    lm::screenshare::ScreenShareManager screen_mgr(net_mgr,
                                                     net_mgr.io_context());

    screen_mgr.set_on_state_change([](const lm::screenshare::ShareInfo& info) {
        switch (info.state) {
            case lm::screenshare::ShareState::ACTIVE:
                std::cout << "\n  Демонстрация экрана "
                          << (info.is_sender ? "начата" : "просматривается")
                          << "\n> " << std::flush;
                break;
            case lm::screenshare::ShareState::STOPPED:
                std::cout << "\n  Демонстрация экрана завершена"
                          << "\n> " << std::flush;
                break;
            default:
                break;
        }
    });

    // ── Callback-и NetworkManager ─────────────────────────────────────
    net_mgr.set_on_peer_found([&](const lm::net::PeerInfo& info) {
        spdlog::info("Пир найден: {} ({}:{})", info.username, info.ip, info.port);

        lm::storage::Contact c;
        c.username   = info.username;
        c.ip         = info.ip;
        c.port       = info.port;
        c.public_key = info.public_key;
        if (!info.public_key.empty()) {
            auto key = lm::crypto::CryptoManager::hex_to_key(info.public_key);
            if (key) c.fingerprint = lm::crypto::CryptoManager::fingerprint(*key);
        }
        contacts.upsert(c);

        if (!net_mgr.is_connected(info.ip)) {
            net_mgr.connect_to(info.ip, info.port);
        }
    });

    net_mgr.set_on_peer_lost([](const std::string& ip) {
        spdlog::info("Пир потерян: {}", ip);
    });

    net_mgr.set_on_peer_connected([&](const std::string& ip) {
        spdlog::info("TCP-соединение с {}", ip);

        json hs;
        hs["username"]   = username;
        hs["public_key"] = pub_key_hex;
        hs["version"]    = "0.2.0";
        net_mgr.send_to(ip, lm::net::make_packet(
            lm::net::PacketType::HANDSHAKE, hs.dump()));
    });

    net_mgr.start(username, cfg.tcp_port, cfg.udp_port, pub_key_hex);

    // ── Сигналы ───────────────────────────────────────────────────────
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Интерактивный цикл ────────────────────────────────────────────
    spdlog::info("Готов. Команды:");
    spdlog::info("  /peers              — список пиров");
    spdlog::info("  /send <ip> <text>   — отправить сообщение");
    spdlog::info("  /file <ip> <path>   — отправить файл");
    spdlog::info("  /call <ip>          — аудиозвонок");
    spdlog::info("  /video <ip>         — видеозвонок");
    spdlog::info("  /accept             — принять звонок");
    spdlog::info("  /decline            — отклонить звонок");
    spdlog::info("  /hangup             — завершить звонок");
    spdlog::info("  /screen <ip>        — демонстрация экрана");
    spdlog::info("  /stopscreen         — остановить демонстрацию");
    spdlog::info("  /history <ip>       — история чата");
    spdlog::info("  /search <text>      — поиск сообщений");
    spdlog::info("  /read <ip>          — пометить как прочитанные");
    spdlog::info("  /transfers          — активные передачи файлов");
    spdlog::info("  /quit               — выход");

    while (g_running) {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        // ── Выход ─────────────────────────────────────────────────────
        if (line == "/quit" || line == "/exit") break;

        // ── Список пиров ──────────────────────────────────────────────
        if (line == "/peers") {
            auto peers = net_mgr.discovered_peers();
            if (peers.empty()) {
                std::cout << "Нет обнаруженных пиров\n";
            } else {
                for (auto& p : peers) {
                    std::cout << "  " << p.username << " — " << p.ip
                              << ":" << p.port;
                    if (net_mgr.is_connected(p.ip)) std::cout << " [TCP]";
                    std::cout << "\n";
                }
            }
            continue;
        }

        // ── Отправка сообщения (через ChatManager с шифрованием) ──────
        if (line.substr(0, 6) == "/send ") {
            auto rest = line.substr(6);
            auto space = rest.find(' ');
            if (space == std::string::npos) {
                std::cout << "Использование: /send <ip> <текст>\n";
                continue;
            }
            std::string ip   = rest.substr(0, space);
            std::string text = rest.substr(space + 1);

            auto uuid = chat_mgr.send_text(ip, text);
            if (uuid.empty()) {
                std::cout << "Не удалось отправить (контакт не найден)\n";
            } else {
                std::cout << "Отправлено [" << uuid.substr(0, 8) << "]\n";
            }
            continue;
        }

        // ── Отправка файла ────────────────────────────────────────────
        if (line.substr(0, 6) == "/file ") {
            auto rest = line.substr(6);
            auto space = rest.find(' ');
            if (space == std::string::npos) {
                std::cout << "Использование: /file <ip> <путь_к_файлу>\n";
                continue;
            }
            std::string ip   = rest.substr(0, space);
            std::string path = rest.substr(space + 1);

            auto tid = file_mgr.send_file(ip, cfg.file_port, path);
            if (tid.empty()) {
                std::cout << "Ошибка: файл не найден\n";
            } else {
                std::cout << "Передача файла начата [" << tid << "]\n";
            }
            continue;
        }

        // ── Активные передачи ─────────────────────────────────────────
        if (line == "/transfers") {
            auto transfers = file_mgr.active_transfers();
            if (transfers.empty()) {
                std::cout << "Нет активных передач\n";
            } else {
                for (auto& t : transfers) {
                    std::cout << "  " << t.transfer_id << " "
                              << t.file_name << " "
                              << static_cast<int>(t.progress() * 100) << "% "
                              << (t.is_sender ? "↑" : "↓") << "\n";
                }
            }
            continue;
        }

        // ── Аудиозвонок ───────────────────────────────────────────────
        if (line.substr(0, 6) == "/call ") {
            std::string ip = line.substr(6);
            call_mgr.start_call(ip, lm::call::CallType::AUDIO);
            continue;
        }

        // ── Видеозвонок ───────────────────────────────────────────────
        if (line.substr(0, 7) == "/video ") {
            std::string ip = line.substr(7);
            call_mgr.start_call(ip, lm::call::CallType::VIDEO);
            continue;
        }

        // ── Принять звонок ────────────────────────────────────────────
        if (line == "/accept") {
            call_mgr.accept_call();
            continue;
        }

        // ── Отклонить звонок ──────────────────────────────────────────
        if (line == "/decline") {
            call_mgr.decline_call();
            continue;
        }

        // ── Завершить звонок ──────────────────────────────────────────
        if (line == "/hangup") {
            call_mgr.hang_up();
            continue;
        }

        // ── Демонстрация экрана ───────────────────────────────────────
        if (line.substr(0, 8) == "/screen ") {
            std::string ip = line.substr(8);
            lm::screenshare::CaptureSettings cs;
            cs.fps = 15;
            cs.source = lm::screenshare::CaptureSource::FULL_SCREEN;
            screen_mgr.start_sharing(ip, cs);
            continue;
        }

        if (line == "/stopscreen") {
            screen_mgr.stop_sharing();
            continue;
        }

        // ── История чата ──────────────────────────────────────────────
        if (line.substr(0, 9) == "/history ") {
            std::string ip = line.substr(9);
            auto msgs = chat_mgr.get_history(ip, 20);
            if (msgs.empty()) {
                std::cout << "История пуста\n";
            } else {
                for (auto& m : msgs) {
                    std::string dir = (m.direction == lm::chat::Direction::INCOMING)
                                        ? "<< " : ">> ";
                    std::cout << "  " << dir << m.text << "\n";
                }
            }
            continue;
        }

        // ── Поиск ─────────────────────────────────────────────────────
        if (line.substr(0, 8) == "/search ") {
            std::string query = line.substr(8);
            auto results = chat_mgr.search(query);
            if (results.empty()) {
                std::cout << "Ничего не найдено\n";
            } else {
                for (auto& m : results) {
                    std::cout << "  " << m.text << "\n";
                }
            }
            continue;
        }

        // ── Прочитать все от пира ─────────────────────────────────────
        if (line.substr(0, 6) == "/read ") {
            std::string ip = line.substr(6);
            chat_mgr.send_read_receipt(ip);
            std::cout << "Помечено как прочитанные\n";
            continue;
        }

        std::cout << "Неизвестная команда. Введите /quit для выхода.\n";
    }

    // ── Завершение ────────────────────────────────────────────────────
    spdlog::info("Завершение работы...");
    screen_mgr.stop_sharing();
    if (call_mgr.is_in_call()) call_mgr.hang_up();
    file_mgr.stop();
    net_mgr.stop();
    spdlog::info("LocalMessenger остановлен");

    return 0;
}
