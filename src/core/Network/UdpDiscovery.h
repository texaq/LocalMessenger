#pragma once
// ═══════════════════════════════════════════════════════════════════════
// UdpDiscovery.h — Обнаружение пиров через UDP broadcast
//
// Периодически (каждые 5 сек) шлёт broadcast в подсеть 26.0.0.0/8
// (Radmin VPN), содержащий информацию о себе (username, IP, port, версию).
// Одновременно слушает broadcast от других пиров и уведомляет о них.
//
// Таймаут узла: если не было broadcast 15 секунд — узел считается offline.
// ═══════════════════════════════════════════════════════════════════════

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>

namespace lm::net {

/// Информация о найденном пире
struct PeerInfo {
    std::string  username;
    std::string  ip;
    uint16_t     port       = 0;
    std::string  version;
    std::string  public_key;   // hex-encoded X25519 public key
    std::chrono::steady_clock::time_point last_seen;
};

class UdpDiscovery {
public:
    /// Callback при обнаружении нового пира или обновлении существующего
    using OnPeerFoundCb   = std::function<void(const PeerInfo&)>;
    /// Callback при потере пира (таймаут 15 сек)
    using OnPeerLostCb    = std::function<void(const std::string& ip)>;

    UdpDiscovery(boost::asio::io_context& io, uint16_t port);

    /// Запуск рассылки и прослушивания
    /// @param username   — имя текущего пользователя
    /// @param tcp_port   — TCP-порт, на котором слушает чат-сервер
    /// @param public_key — публичный ключ X25519 (hex)
    void start(const std::string& username,
               uint16_t tcp_port,
               const std::string& public_key,
               OnPeerFoundCb on_found,
               OnPeerLostCb  on_lost);

    /// Остановить
    void stop();

    /// Получить текущий список известных пиров
    std::vector<PeerInfo> peers() const;

private:
    void do_broadcast();
    void schedule_broadcast();
    void do_receive();
    void check_timeouts();
    void schedule_timeout_check();

    boost::asio::io_context&          io_;
    boost::asio::ip::udp::socket      socket_;
    boost::asio::ip::udp::endpoint    broadcast_ep_;
    boost::asio::ip::udp::endpoint    sender_ep_;
    boost::asio::steady_timer         broadcast_timer_;
    boost::asio::steady_timer         timeout_timer_;
    uint16_t                          port_;

    // Данные о себе
    std::string  my_username_;
    uint16_t     my_tcp_port_ = 0;
    std::string  my_public_key_;

    // Известные пиры: ключ — IP-адрес
    mutable std::mutex                         peers_mutex_;
    std::unordered_map<std::string, PeerInfo>   peers_;

    // Буфер приёма
    std::array<char, 2048> recv_buf_{};

    OnPeerFoundCb  on_peer_found_;
    OnPeerLostCb   on_peer_lost_;

    bool running_ = false;

    static constexpr auto BROADCAST_INTERVAL = std::chrono::seconds(5);
    static constexpr auto PEER_TIMEOUT       = std::chrono::seconds(15);
};

} // namespace lm::net
