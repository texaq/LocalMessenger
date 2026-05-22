// ═══════════════════════════════════════════════════════════════════════
// ScreenShareManager.cpp — Реализация демонстрации экрана
//
// ВАЖНО: Полноценный захват экрана требует платформо-зависимые API:
// - Windows: DXGI Desktop Duplication API (GPU-ускоренный захват)
// - Linux: X11 XShmGetImage / PipeWire
// - Кодирование: FFmpeg libavcodec (H.264 NVENC / QSV / x264)
//
// Этот файл предоставляет каркас с заглушками для захвата.
// На Windows будет добавлен DXGI Duplication при сборке под Win10/11.
// ═══════════════════════════════════════════════════════════════════════

#include "ScreenShareManager.h"
#include "core/Network/Protocol.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace lm::screenshare {

using json = nlohmann::json;

static std::string gen_share_id() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << "ss-" << std::hex << dist(gen);
    return ss.str();
}

ScreenShareManager::ScreenShareManager(net::NetworkManager& net,
                                        boost::asio::io_context& io)
    : net_(net)
    , io_(io)
    , capture_timer_(io) {}

std::vector<CaptureTarget> ScreenShareManager::enumerate_targets() {
    std::vector<CaptureTarget> targets;

#ifdef _WIN32
    // TODO: Windows — EnumDisplayMonitors + DXGI Output enumeration
    // Пока возвращаем заглушку для основного монитора
    CaptureTarget primary;
    primary.index      = 0;
    primary.name       = "Primary Monitor";
    primary.width      = 1920;
    primary.height     = 1080;
    primary.is_primary = true;
    targets.push_back(primary);
#else
    // Linux — XRandR / PipeWire
    CaptureTarget primary;
    primary.index      = 0;
    primary.name       = "Primary Display";
    primary.width      = 1920;
    primary.height     = 1080;
    primary.is_primary = true;
    targets.push_back(primary);
#endif

    return targets;
}

void ScreenShareManager::start_sharing(const std::string& peer_ip,
                                        const CaptureSettings& settings) {
    std::lock_guard<std::mutex> lock(share_mutex_);

    if (current_.state == ShareState::ACTIVE) {
        spdlog::warn("Демонстрация уже активна");
        return;
    }

    current_ = ShareInfo{};
    current_.share_id  = gen_share_id();
    current_.peer_ip   = peer_ip;
    current_.is_sender = true;
    current_.state     = ShareState::STARTING;
    current_.settings  = settings;

    spdlog::info("Демонстрация экрана → {} ({}fps)",
                 peer_ip, settings.fps);

    // Отправляем уведомление пиру о начале демонстрации
    json j;
    j["share_id"]       = current_.share_id;
    j["action"]         = "start";
    j["fps"]            = settings.fps;
    j["remote_control"] = settings.remote_control_enabled;

    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::SCREEN_FRAME, j.dump()));

    current_.state = ShareState::ACTIVE;

    if (on_state_) on_state_(current_);

    // Запускаем цикл захвата
    capture_loop();
}

void ScreenShareManager::stop_sharing() {
    std::lock_guard<std::mutex> lock(share_mutex_);

    if (current_.state == ShareState::IDLE) return;

    // Уведомляем пира
    json j;
    j["share_id"] = current_.share_id;
    j["action"]   = "stop";
    net_.send_to(current_.peer_ip, net::make_packet(
        net::PacketType::SCREEN_FRAME, j.dump()));

    current_.state = ShareState::STOPPED;
    capture_timer_.cancel();

    spdlog::info("Демонстрация экрана остановлена");

    if (on_state_) on_state_(current_);
    current_ = ShareInfo{};
}

void ScreenShareManager::pause_sharing() {
    std::lock_guard<std::mutex> lock(share_mutex_);
    if (current_.state == ShareState::ACTIVE) {
        current_.state = ShareState::PAUSED;
        capture_timer_.cancel();
        if (on_state_) on_state_(current_);
    }
}

void ScreenShareManager::resume_sharing() {
    std::lock_guard<std::mutex> lock(share_mutex_);
    if (current_.state == ShareState::PAUSED) {
        current_.state = ShareState::ACTIVE;
        if (on_state_) on_state_(current_);
        capture_loop();
    }
}

void ScreenShareManager::request_view(const std::string& peer_ip) {
    json j;
    j["action"] = "request_view";
    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::SCREEN_FRAME, j.dump()));
}

void ScreenShareManager::send_remote_input(const RemoteInputEvent& event) {
    std::lock_guard<std::mutex> lock(share_mutex_);

    if (current_.state == ShareState::IDLE) return;

    json j;
    j["type"]  = static_cast<int>(event.type);
    j["x"]     = event.x;
    j["y"]     = event.y;
    j["button"] = event.button;
    j["key"]   = event.key_code;
    j["scroll"] = event.scroll_delta;
    j["dbl"]   = event.is_double_click;

    net_.send_to(current_.peer_ip, net::make_packet(
        net::PacketType::SCREEN_INPUT, j.dump()));
}

ShareInfo ScreenShareManager::current_share() const {
    std::lock_guard<std::mutex> lock(share_mutex_);
    return current_;
}

bool ScreenShareManager::is_sharing() const {
    std::lock_guard<std::mutex> lock(share_mutex_);
    return current_.is_sender && current_.state == ShareState::ACTIVE;
}

bool ScreenShareManager::is_viewing() const {
    std::lock_guard<std::mutex> lock(share_mutex_);
    return !current_.is_sender && current_.state == ShareState::ACTIVE;
}

// ── Обработка входящих пакетов ────────────────────────────────────────
void ScreenShareManager::handle_packet(const std::string& ip,
                                        net::Packet pkt) {
    switch (pkt.header.type) {
        case net::PacketType::SCREEN_FRAME: {
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);

                std::string action = j.value("action", "");

                if (action == "start") {
                    std::lock_guard<std::mutex> lock(share_mutex_);
                    current_ = ShareInfo{};
                    current_.share_id  = j.value("share_id", gen_share_id());
                    current_.peer_ip   = ip;
                    current_.is_sender = false;
                    current_.state     = ShareState::ACTIVE;
                    current_.settings.remote_control_enabled =
                        j.value("remote_control", false);

                    spdlog::info("Просмотр демонстрации от {}", ip);
                    if (on_state_) on_state_(current_);
                }
                else if (action == "stop") {
                    std::lock_guard<std::mutex> lock(share_mutex_);
                    current_.state = ShareState::STOPPED;
                    spdlog::info("Демонстрация от {} завершена", ip);
                    if (on_state_) on_state_(current_);
                    current_ = ShareInfo{};
                }
                else if (action == "frame") {
                    // Декодировать и передать в UI
                    // TODO: FFmpeg H.264 декодирование
                    if (on_frame_) {
                        int w = j.value("w", 0);
                        int h = j.value("h", 0);
                        // Реальные данные фрейма будут в бинарной части
                        on_frame_({}, w, h);
                    }
                }
            } catch (...) {}
            break;
        }

        case net::PacketType::SCREEN_INPUT: {
            try {
                std::string raw(pkt.payload.begin(), pkt.payload.end());
                auto j = json::parse(raw);

                RemoteInputEvent event;
                event.type       = static_cast<RemoteInputEvent::Type>(
                    j.value("type", 0));
                event.x          = j.value("x", 0);
                event.y          = j.value("y", 0);
                event.button     = j.value("button", 0);
                event.key_code   = j.value("key", 0);
                event.scroll_delta    = j.value("scroll", 0);
                event.is_double_click = j.value("dbl", false);

                // Проверяем, разрешён ли remote control
                {
                    std::lock_guard<std::mutex> lock(share_mutex_);
                    if (!current_.settings.remote_control_enabled) {
                        spdlog::warn("Remote control не разрешён");
                        return;
                    }
                }

#ifdef _WIN32
                // TODO: SendInput для имитации ввода
                // INPUT input = {};
                // ...
                // SendInput(1, &input, sizeof(INPUT));
#endif

                if (on_remote_input_) on_remote_input_(event);

            } catch (...) {}
            break;
        }

        default:
            break;
    }
}

// ── Цикл захвата ─────────────────────────────────────────────────────
void ScreenShareManager::capture_loop() {
    // Проверяем состояние без блокировки (capture_timer уже был сброшен)
    {
        std::lock_guard<std::mutex> lock(share_mutex_);
        if (current_.state != ShareState::ACTIVE) return;
    }

    int fps;
    {
        std::lock_guard<std::mutex> lock(share_mutex_);
        fps = current_.settings.fps;
    }

    auto interval = std::chrono::milliseconds(1000 / std::max(1, fps));
    capture_timer_.expires_after(interval);
    capture_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;

        // Захват кадра (платформо-зависимый)
        // TODO: DXGI Desktop Duplication API (Windows)
        // TODO: X11 XShmGetImage (Linux)

        // Заглушка — пустой фрейм
        // В реальной реализации: захват → кодирование H.264 → отправка по UDP

        {
            std::lock_guard<std::mutex> lock(share_mutex_);
            current_.frames_sent++;
        }

        capture_loop();
    });
}

void ScreenShareManager::encode_and_send(const std::vector<uint8_t>& raw_frame,
                                          int width, int height) {
    // TODO: H.264 кодирование через FFmpeg API
    // 1. avcodec_find_encoder_by_name("h264_nvenc") → QSV → x264
    // 2. avcodec_send_frame → avcodec_receive_packet
    // 3. Отправка закодированного пакета через UDP

    json j;
    j["action"] = "frame";
    j["w"]      = width;
    j["h"]      = height;

    // В реальности payload будет бинарным H.264 NAL unit
    net_.send_to(current_.peer_ip, net::make_packet(
        net::PacketType::SCREEN_FRAME, j.dump()));
}

void ScreenShareManager::adapt_fps(double network_latency_ms) {
    std::lock_guard<std::mutex> lock(share_mutex_);

    if (network_latency_ms > 200) {
        current_.settings.fps = std::max(
            current_.settings.fps_min,
            current_.settings.fps - 5);
    } else if (network_latency_ms < 50) {
        current_.settings.fps = std::min(
            current_.settings.fps_max,
            current_.settings.fps + 2);
    }
}

} // namespace lm::screenshare
