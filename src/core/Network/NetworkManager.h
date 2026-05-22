#pragma once
// ═══════════════════════════════════════════════════════════════════════
// NetworkManager.h — Центральный менеджер сетевого уровня
//
// Объединяет TCP-сервер, TCP-клиент и UDP-discovery.
// Управляет списком активных сессий (пиров).
// ═══════════════════════════════════════════════════════════════════════

#include "TcpServer.h"
#include "TcpClient.h"
#include "UdpDiscovery.h"

#include <boost/asio.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lm::net {

class NetworkManager {
public:
    /// Callback при получении пакета от пира: (remote_ip, packet)
    using OnMessageCb   = std::function<void(const std::string&, Packet)>;
    /// Callback при обнаружении нового пира
    using OnPeerFoundCb = std::function<void(const PeerInfo&)>;
    /// Callback при потере пира
    using OnPeerLostCb  = std::function<void(const std::string& ip)>;
    /// Callback при установке TCP-соединения с пиром
    using OnPeerConnectedCb = std::function<void(const std::string& ip)>;

    NetworkManager();
    ~NetworkManager();

    /// Запуск сетевого уровня
    /// @param username   — имя текущего пользователя
    /// @param tcp_port   — порт TCP-сервера (чат)
    /// @param udp_port   — порт UDP discovery
    /// @param public_key — публичный ключ X25519 (hex)
    void start(const std::string& username,
               uint16_t tcp_port,
               uint16_t udp_port,
               const std::string& public_key);

    /// Остановка
    void stop();

    /// Подключиться к пиру вручную (по IP и порту)
    void connect_to(const std::string& ip, uint16_t port);

    /// Отправить пакет конкретному пиру (по IP)
    void send_to(const std::string& ip, const Packet& packet);

    /// Отправить пакет всем подключённым пирам
    void broadcast(const Packet& packet);

    /// Установить callback-и
    void set_on_message(OnMessageCb cb)          { on_message_ = std::move(cb); }
    void set_on_peer_found(OnPeerFoundCb cb)     { on_peer_found_ = std::move(cb); }
    void set_on_peer_lost(OnPeerLostCb cb)       { on_peer_lost_ = std::move(cb); }
    void set_on_peer_connected(OnPeerConnectedCb cb) { on_peer_connected_ = std::move(cb); }

    /// Список известных пиров (из UDP discovery)
    std::vector<PeerInfo> discovered_peers() const;

    /// Проверка, есть ли TCP-соединение с пиром
    bool is_connected(const std::string& ip) const;

private:
    void on_incoming_session(TcpSession::Ptr session);
    void register_session(const std::string& ip, TcpSession::Ptr session);
    void on_session_packet(const std::string& ip, TcpSession::Ptr session, Packet pkt);
    void on_session_closed(const std::string& ip, TcpSession::Ptr session, const std::string& reason);

    boost::asio::io_context             io_;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> work_guard_;
    std::vector<std::thread>            io_threads_;

    std::unique_ptr<TcpServer>          tcp_server_;
    std::unique_ptr<TcpClient>          tcp_client_;
    std::unique_ptr<UdpDiscovery>       discovery_;

    // Активные TCP-сессии: IP → session
    mutable std::mutex                              sessions_mutex_;
    std::unordered_map<std::string, TcpSession::Ptr> sessions_;

    OnMessageCb        on_message_;
    OnPeerFoundCb      on_peer_found_;
    OnPeerLostCb       on_peer_lost_;
    OnPeerConnectedCb  on_peer_connected_;
};

} // namespace lm::net
