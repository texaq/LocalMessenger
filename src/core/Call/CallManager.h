#pragma once
// ═══════════════════════════════════════════════════════════════════════
// CallManager.h — Управление голосовыми и видеозвонками
//
// Сигнализация: SDP offer/answer и ICE candidates через TCP-канал чата
// Медиа: UDP для аудио/видео потоков
// Состояния: IDLE → OFFERING → RINGING → ACTIVE → ENDED
// ═══════════════════════════════════════════════════════════════════════

#include "core/Network/NetworkManager.h"
#include <boost/asio.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <mutex>

namespace lm::call {

/// Состояние звонка
enum class CallState {
    IDLE,       // нет звонка
    OFFERING,   // отправлен offer, ждём answer
    RINGING,    // входящий звонок, ждём ответа пользователя
    ACTIVE,     // активный звонок
    ENDED,      // завершён
    DECLINED,   // отклонён
};

/// Тип звонка
enum class CallType {
    AUDIO,      // только аудио
    VIDEO,      // аудио + видео
};

/// Информация о текущем звонке
struct CallInfo {
    std::string call_id;
    std::string peer_ip;
    std::string peer_name;
    CallState   state = CallState::IDLE;
    CallType    type  = CallType::AUDIO;
    int64_t     start_time = 0;    // unix timestamp начала
    int         duration_sec = 0;  // длительность в секундах

    // Настройки
    bool mic_muted    = false;
    bool camera_off   = false;
    bool speaker_muted = false;
    float mic_volume   = 1.0f;    // 0.0 — 1.0
    float speaker_volume = 1.0f;

    // Push-to-talk
    bool push_to_talk = false;
    bool ptt_active   = false;    // кнопка зажата

    // VAD (Voice Activity Detection)
    bool vad_enabled  = true;
    bool voice_active = false;    // голос обнаружен
};

class CallManager {
public:
    using OnCallStateCb  = std::function<void(const CallInfo&)>;
    using OnIncomingCb   = std::function<void(const CallInfo&)>;

    CallManager(net::NetworkManager& net, boost::asio::io_context& io);

    /// Инициировать звонок
    void start_call(const std::string& peer_ip, CallType type);

    /// Принять входящий звонок
    void accept_call();

    /// Отклонить входящий звонок
    void decline_call();

    /// Завершить текущий звонок
    void hang_up();

    /// Управление микрофоном
    void toggle_mic();
    void set_mic_muted(bool muted);

    /// Управление камерой (для видеозвонков)
    void toggle_camera();
    void set_camera_off(bool off);

    /// Push-to-talk
    void set_push_to_talk(bool enabled);
    void ptt_press();
    void ptt_release();

    /// Регулировка громкости
    void set_mic_volume(float vol);
    void set_speaker_volume(float vol);

    /// Текущее состояние звонка
    CallInfo current_call() const;
    bool is_in_call() const;

    // Callback-и
    void set_on_state_change(OnCallStateCb cb) { on_state_ = std::move(cb); }
    void set_on_incoming(OnIncomingCb cb)      { on_incoming_ = std::move(cb); }

    /// Обработка пакетов сигнализации (вызывается из ChatManager)
    void handle_signaling(const std::string& ip, net::Packet pkt);

private:
    void send_offer(const std::string& peer_ip, CallType type);
    void send_answer(const std::string& peer_ip);
    void send_hangup(const std::string& peer_ip);
    void update_duration();

    net::NetworkManager&        net_;
    boost::asio::io_context&    io_;
    boost::asio::steady_timer   duration_timer_;

    mutable std::mutex call_mutex_;
    CallInfo           current_;

    OnCallStateCb on_state_;
    OnIncomingCb  on_incoming_;
};

} // namespace lm::call
