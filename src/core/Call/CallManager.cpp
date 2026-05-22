// ═══════════════════════════════════════════════════════════════════════
// CallManager.cpp — Реализация управления звонками
//
// Сигнализация через TCP (CALL_OFFER, CALL_ANSWER, CALL_HANGUP).
// Медиапоток (аудио/видео) через UDP — заглушка, будет реализована
// с интеграцией Opus/PortAudio и H.264/VP8.
// ═══════════════════════════════════════════════════════════════════════

#include "CallManager.h"
#include "core/Network/Protocol.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace lm::call {

using json = nlohmann::json;

static std::string gen_call_id() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << "call-" << std::hex << dist(gen);
    return ss.str();
}

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

CallManager::CallManager(net::NetworkManager& net,
                          boost::asio::io_context& io)
    : net_(net)
    , io_(io)
    , duration_timer_(io) {}

void CallManager::start_call(const std::string& peer_ip, CallType type) {
    std::lock_guard<std::mutex> lock(call_mutex_);

    if (current_.state != CallState::IDLE) {
        spdlog::warn("Уже в звонке, невозможно начать новый");
        return;
    }

    current_ = CallInfo{};
    current_.call_id   = gen_call_id();
    current_.peer_ip   = peer_ip;
    current_.type      = type;
    current_.state     = CallState::OFFERING;

    send_offer(peer_ip, type);

    spdlog::info("Исходящий звонок → {} ({})",
                 peer_ip, type == CallType::VIDEO ? "видео" : "аудио");

    if (on_state_) on_state_(current_);
}

void CallManager::accept_call() {
    std::lock_guard<std::mutex> lock(call_mutex_);

    if (current_.state != CallState::RINGING) {
        spdlog::warn("Нет входящего звонка для принятия");
        return;
    }

    current_.state      = CallState::ACTIVE;
    current_.start_time = now_ms();

    send_answer(current_.peer_ip);

    spdlog::info("Звонок принят с {}", current_.peer_ip);

    if (on_state_) on_state_(current_);

    // Запускаем таймер длительности
    update_duration();
}

void CallManager::decline_call() {
    std::lock_guard<std::mutex> lock(call_mutex_);

    if (current_.state != CallState::RINGING) return;

    current_.state = CallState::DECLINED;

    json j;
    j["call_id"] = current_.call_id;
    j["action"]  = "decline";
    net_.send_to(current_.peer_ip, net::make_packet(
        net::PacketType::CALL_DECLINE, j.dump()));

    spdlog::info("Звонок отклонён от {}", current_.peer_ip);

    if (on_state_) on_state_(current_);
    current_ = CallInfo{};
}

void CallManager::hang_up() {
    std::lock_guard<std::mutex> lock(call_mutex_);

    if (current_.state == CallState::IDLE) return;

    auto prev_state = current_.state;
    current_.state = CallState::ENDED;

    send_hangup(current_.peer_ip);
    duration_timer_.cancel();

    spdlog::info("Звонок завершён с {} ({}сек)",
                 current_.peer_ip, current_.duration_sec);

    if (on_state_) on_state_(current_);
    current_ = CallInfo{};
}

// ── Управление ───────────────────────────────────────────────────────

void CallManager::toggle_mic() {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.mic_muted = !current_.mic_muted;
    spdlog::debug("Микрофон: {}", current_.mic_muted ? "выкл" : "вкл");
}

void CallManager::set_mic_muted(bool muted) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.mic_muted = muted;
}

void CallManager::toggle_camera() {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.camera_off = !current_.camera_off;
    spdlog::debug("Камера: {}", current_.camera_off ? "выкл" : "вкл");
}

void CallManager::set_camera_off(bool off) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.camera_off = off;
}

void CallManager::set_push_to_talk(bool enabled) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.push_to_talk = enabled;
    if (enabled) {
        current_.mic_muted = true;  // по умолчанию мьютим в PTT режиме
    }
}

void CallManager::ptt_press() {
    std::lock_guard<std::mutex> lock(call_mutex_);
    if (current_.push_to_talk) {
        current_.ptt_active = true;
        current_.mic_muted  = false;
    }
}

void CallManager::ptt_release() {
    std::lock_guard<std::mutex> lock(call_mutex_);
    if (current_.push_to_talk) {
        current_.ptt_active = false;
        current_.mic_muted  = true;
    }
}

void CallManager::set_mic_volume(float vol) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.mic_volume = std::max(0.0f, std::min(1.0f, vol));
}

void CallManager::set_speaker_volume(float vol) {
    std::lock_guard<std::mutex> lock(call_mutex_);
    current_.speaker_volume = std::max(0.0f, std::min(1.0f, vol));
}

CallInfo CallManager::current_call() const {
    std::lock_guard<std::mutex> lock(call_mutex_);
    return current_;
}

bool CallManager::is_in_call() const {
    std::lock_guard<std::mutex> lock(call_mutex_);
    return current_.state == CallState::ACTIVE ||
           current_.state == CallState::OFFERING ||
           current_.state == CallState::RINGING;
}

// ── Обработка сигнализации ────────────────────────────────────────────
void CallManager::handle_signaling(const std::string& ip, net::Packet pkt) {
    try {
        std::string raw(pkt.payload.begin(), pkt.payload.end());
        auto j = json::parse(raw);

        switch (pkt.header.type) {
            case net::PacketType::CALL_OFFER: {
                std::lock_guard<std::mutex> lock(call_mutex_);

                if (current_.state != CallState::IDLE) {
                    // Уже в звонке — автоматически отклоняем
                    json decline;
                    decline["call_id"] = j.value("call_id", "");
                    decline["action"]  = "busy";
                    net_.send_to(ip, net::make_packet(
                        net::PacketType::CALL_DECLINE, decline.dump()));
                    return;
                }

                current_ = CallInfo{};
                current_.call_id   = j.value("call_id", gen_call_id());
                current_.peer_ip   = ip;
                current_.peer_name = j.value("caller_name", "");
                current_.type      = j.value("type", "audio") == "video"
                                        ? CallType::VIDEO : CallType::AUDIO;
                current_.state     = CallState::RINGING;

                spdlog::info("Входящий звонок от {} ({})",
                             current_.peer_name, ip);

                if (on_incoming_) on_incoming_(current_);
                if (on_state_)    on_state_(current_);
                break;
            }

            case net::PacketType::CALL_ANSWER: {
                std::lock_guard<std::mutex> lock(call_mutex_);

                if (current_.state != CallState::OFFERING) return;

                current_.state      = CallState::ACTIVE;
                current_.start_time = now_ms();

                spdlog::info("Звонок принят: {}", ip);

                if (on_state_) on_state_(current_);
                update_duration();
                break;
            }

            case net::PacketType::CALL_HANGUP: {
                std::lock_guard<std::mutex> lock(call_mutex_);

                current_.state = CallState::ENDED;
                duration_timer_.cancel();

                spdlog::info("Звонок завершён пиром: {}", ip);

                if (on_state_) on_state_(current_);
                current_ = CallInfo{};
                break;
            }

            case net::PacketType::CALL_DECLINE: {
                std::lock_guard<std::mutex> lock(call_mutex_);

                current_.state = CallState::DECLINED;
                std::string reason = j.value("action", "declined");

                spdlog::info("Звонок отклонён: {} ({})", ip, reason);

                if (on_state_) on_state_(current_);
                current_ = CallInfo{};
                break;
            }

            default:
                break;
        }
    } catch (const std::exception& e) {
        spdlog::warn("CallManager: ошибка парсинга пакета: {}", e.what());
    }
}

// ── Отправка сигнализации ─────────────────────────────────────────────
void CallManager::send_offer(const std::string& peer_ip, CallType type) {
    json j;
    j["call_id"]     = current_.call_id;
    j["type"]        = (type == CallType::VIDEO) ? "video" : "audio";
    j["caller_name"] = "";  // будет установлено из main.cpp

    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::CALL_OFFER, j.dump()));
}

void CallManager::send_answer(const std::string& peer_ip) {
    json j;
    j["call_id"] = current_.call_id;
    j["action"]  = "accept";

    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::CALL_ANSWER, j.dump()));
}

void CallManager::send_hangup(const std::string& peer_ip) {
    json j;
    j["call_id"] = current_.call_id;
    j["action"]  = "hangup";

    net_.send_to(peer_ip, net::make_packet(
        net::PacketType::CALL_HANGUP, j.dump()));
}

void CallManager::update_duration() {
    duration_timer_.expires_after(std::chrono::seconds(1));
    duration_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;

        {
            std::lock_guard<std::mutex> lock(call_mutex_);
            if (current_.state != CallState::ACTIVE) return;
            current_.duration_sec++;
        }

        update_duration();
    });
}

} // namespace lm::call
