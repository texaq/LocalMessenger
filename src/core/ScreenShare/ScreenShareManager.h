#pragma once
// ═══════════════════════════════════════════════════════════════════════
// ScreenShareManager.h — Демонстрация экрана
//
// Захват: DXGI Desktop Duplication API (Windows), X11/PipeWire (Linux)
// Кодирование: H.264 через FFmpeg (NVENC → QSV → x264 fallback)
// Транспорт: UDP для фреймов
// Опционально: remote control (SendInput на стороне получателя)
// ═══════════════════════════════════════════════════════════════════════

#include "core/Network/NetworkManager.h"
#include <boost/asio.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace lm::screenshare {

/// Источник захвата
enum class CaptureSource {
    FULL_SCREEN,      // весь экран (основной монитор)
    SPECIFIC_MONITOR, // конкретный монитор (по индексу)
    WINDOW,           // отдельное окно
};

/// Настройки захвата
struct CaptureSettings {
    CaptureSource source = CaptureSource::FULL_SCREEN;
    int monitor_index    = 0;
    std::string window_title;  // для WINDOW

    int fps_min = 5;
    int fps_max = 30;
    int fps     = 15;          // текущий целевой FPS

    int quality = 80;          // 0–100 (влияет на bitrate)

    // Remote control
    bool remote_control_enabled = false;
};

/// Состояние демонстрации
enum class ShareState {
    IDLE,       // не активна
    STARTING,   // инициализация захвата
    ACTIVE,     // идёт трансляция
    PAUSED,     // на паузе
    STOPPED,    // остановлена
};

/// Информация о монитере/окне для выбора
struct CaptureTarget {
    int index = 0;
    std::string name;
    int width  = 0;
    int height = 0;
    bool is_primary = false;
};

/// Информация о демонстрации
struct ShareInfo {
    std::string  share_id;
    std::string  peer_ip;
    ShareState   state = ShareState::IDLE;
    bool         is_sender = false;
    CaptureSettings settings;

    int current_fps    = 0;
    int bitrate_kbps   = 0;
    int frame_width    = 0;
    int frame_height   = 0;
    int64_t frames_sent = 0;
};

/// Событие remote control от удалённой стороны
struct RemoteInputEvent {
    enum Type { MOUSE_MOVE, MOUSE_CLICK, MOUSE_SCROLL, KEY_DOWN, KEY_UP };
    Type type;
    int x = 0, y = 0;           // координаты мыши
    int button = 0;              // кнопка мыши (0=left, 1=right, 2=middle)
    int key_code = 0;            // virtual key code
    int scroll_delta = 0;        // дельта скролла
    bool is_double_click = false;
};

class ScreenShareManager {
public:
    using OnFrameCb       = std::function<void(const std::vector<uint8_t>& frame,
                                                int width, int height)>;
    using OnStateChangeCb = std::function<void(const ShareInfo&)>;
    using OnRemoteInputCb = std::function<void(const RemoteInputEvent&)>;

    ScreenShareManager(net::NetworkManager& net,
                        boost::asio::io_context& io);

    /// Получить доступные цели захвата (мониторы, окна)
    std::vector<CaptureTarget> enumerate_targets();

    /// Начать демонстрацию экрана для пира
    void start_sharing(const std::string& peer_ip,
                        const CaptureSettings& settings);

    /// Остановить демонстрацию
    void stop_sharing();

    /// Пауза/возобновление
    void pause_sharing();
    void resume_sharing();

    /// Запрос на просмотр демонстрации пира
    void request_view(const std::string& peer_ip);

    /// Отправить remote input событие
    void send_remote_input(const RemoteInputEvent& event);

    /// Текущая информация
    ShareInfo current_share() const;
    bool is_sharing() const;
    bool is_viewing() const;

    // Callback-и
    void set_on_frame(OnFrameCb cb)             { on_frame_ = std::move(cb); }
    void set_on_state_change(OnStateChangeCb cb) { on_state_ = std::move(cb); }
    void set_on_remote_input(OnRemoteInputCb cb) { on_remote_input_ = std::move(cb); }

    /// Обработка пакетов (вызывается из NetworkManager)
    void handle_packet(const std::string& ip, net::Packet pkt);

private:
    void capture_loop();
    void encode_and_send(const std::vector<uint8_t>& raw_frame,
                          int width, int height);
    void adapt_fps(double network_latency_ms);

    net::NetworkManager&       net_;
    boost::asio::io_context&   io_;
    boost::asio::steady_timer  capture_timer_;

    mutable std::mutex share_mutex_;
    ShareInfo          current_;

    OnFrameCb       on_frame_;
    OnStateChangeCb on_state_;
    OnRemoteInputCb on_remote_input_;
};

} // namespace lm::screenshare
