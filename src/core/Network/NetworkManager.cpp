// ═══════════════════════════════════════════════════════════════════════
// NetworkManager.cpp — Реализация центрального сетевого менеджера
// ═══════════════════════════════════════════════════════════════════════

#include "NetworkManager.h"
#include <spdlog/spdlog.h>

namespace lm::net {

NetworkManager::NetworkManager()
    : work_guard_(boost::asio::make_work_guard(io_)) {}

NetworkManager::~NetworkManager() {
    stop();
}

void NetworkManager::start(const std::string& username,
                            uint16_t tcp_port,
                            uint16_t udp_port,
                            const std::string& public_key) {
    // TCP-сервер: принимает входящие соединения
    tcp_server_ = std::make_unique<TcpServer>(io_, tcp_port);
    tcp_server_->start([this](TcpSession::Ptr session) {
        on_incoming_session(std::move(session));
    });

    // TCP-клиент: для исходящих соединений
    tcp_client_ = std::make_unique<TcpClient>(io_);

    // UDP discovery: поиск пиров через broadcast
    discovery_ = std::make_unique<UdpDiscovery>(io_, udp_port);
    discovery_->start(
        username, tcp_port, public_key,
        // on_peer_found
        [this](const PeerInfo& info) {
            if (on_peer_found_) on_peer_found_(info);
        },
        // on_peer_lost
        [this](const std::string& ip) {
            // Закрываем TCP-сессию с потерянным пиром
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = sessions_.find(ip);
                if (it != sessions_.end()) {
                    it->second->close();
                    sessions_.erase(it);
                }
            }
            if (on_peer_lost_) on_peer_lost_(ip);
        }
    );

    // Запускаем io_context в нескольких потоках
    size_t thread_count = std::max(2u, std::thread::hardware_concurrency());
    for (size_t i = 0; i < thread_count; ++i) {
        io_threads_.emplace_back([this]() {
            try {
                io_.run();
            } catch (const std::exception& e) {
                spdlog::error("io_context поток: исключение: {}", e.what());
            }
        });
    }

    spdlog::info("NetworkManager запущен: TCP={}, UDP={}, потоков={}",
                 tcp_port, udp_port, thread_count);
}

void NetworkManager::stop() {
    if (tcp_server_) tcp_server_->stop();
    if (discovery_)  discovery_->stop();

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& [ip, session] : sessions_) {
            session->close();
        }
        sessions_.clear();
    }

    work_guard_.reset();
    io_.stop();

    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    io_threads_.clear();

    spdlog::info("NetworkManager остановлен");
}

// ── Подключение к пиру вручную ────────────────────────────────────────
void NetworkManager::connect_to(const std::string& ip, uint16_t port) {
    // Не подключаемся повторно
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (sessions_.count(ip)) {
            spdlog::debug("Уже подключены к {}", ip);
            return;
        }
    }

    tcp_client_->connect(
        ip, port,
        // on_connect
        [this, ip](TcpSession::Ptr session) {
            register_session(ip, session);
        },
        // on_error
        [ip](const std::string& err) {
            spdlog::warn("Не удалось подключиться к {}: {}", ip, err);
        });
}

// ── Отправка пакета ───────────────────────────────────────────────────
void NetworkManager::send_to(const std::string& ip, const Packet& packet) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(ip);
    if (it != sessions_.end() && it->second->is_open()) {
        it->second->send(packet);
    } else {
        spdlog::warn("Нет соединения с {} для отправки пакета", ip);
    }
}

void NetworkManager::broadcast(const Packet& packet) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [ip, session] : sessions_) {
        if (session->is_open()) {
            session->send(packet);
        }
    }
}

std::vector<PeerInfo> NetworkManager::discovered_peers() const {
    if (discovery_) return discovery_->peers();
    return {};
}

bool NetworkManager::is_connected(const std::string& ip) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(ip);
    return it != sessions_.end() && it->second->is_open();
}

// ── Внутренние callback-и ─────────────────────────────────────────────

void NetworkManager::on_incoming_session(TcpSession::Ptr session) {
    auto ip = session->remote_address();
    register_session(ip, session);
}

void NetworkManager::register_session(const std::string& ip,
                                       TcpSession::Ptr session) {
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);

        // Если уже есть старая сессия — закрываем
        auto it = sessions_.find(ip);
        if (it != sessions_.end()) {
            it->second->close();
        }

        sessions_[ip] = session;
    }

    // Запускаем чтение из сессии
    session->start(
        [this, ip](TcpSession::Ptr s, Packet pkt) {
            on_session_packet(ip, std::move(s), std::move(pkt));
        },
        [this, ip](TcpSession::Ptr s, std::string reason) {
            on_session_closed(ip, std::move(s), reason);
        });

    spdlog::info("Сессия зарегистрирована: {}", ip);
    if (on_peer_connected_) on_peer_connected_(ip);
}

void NetworkManager::on_session_packet(const std::string& ip,
                                        TcpSession::Ptr /*session*/,
                                        Packet pkt) {
    // PING/PONG обрабатываем автоматически
    if (pkt.header.type == PacketType::PING) {
        send_to(ip, make_pong());
        return;
    }

    if (on_message_) on_message_(ip, std::move(pkt));
}

void NetworkManager::on_session_closed(const std::string& ip,
                                        TcpSession::Ptr /*session*/,
                                        const std::string& reason) {
    spdlog::info("Сессия с {} закрыта: {}", ip, reason);

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(ip);
    }

    if (on_peer_lost_) on_peer_lost_(ip);
}

} // namespace lm::net
