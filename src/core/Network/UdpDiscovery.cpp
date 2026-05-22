// ═══════════════════════════════════════════════════════════════════════
// UdpDiscovery.cpp — Реализация обнаружения пиров через UDP broadcast
//
// Broadcast-пакет — JSON строка:
// {
//   "username": "Alice",
//   "ip": "26.1.2.3",
//   "port": 7777,
//   "version": "0.1.0",
//   "public_key": "aabbccdd..."
// }
// ═══════════════════════════════════════════════════════════════════════

#include "UdpDiscovery.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace lm::net {

using udp = boost::asio::ip::udp;
using json = nlohmann::json;

UdpDiscovery::UdpDiscovery(boost::asio::io_context& io, uint16_t port)
    : io_(io)
    , socket_(io)
    , broadcast_timer_(io)
    , timeout_timer_(io)
    , port_(port) {}

void UdpDiscovery::start(const std::string& username,
                          uint16_t tcp_port,
                          const std::string& public_key,
                          OnPeerFoundCb on_found,
                          OnPeerLostCb  on_lost) {
    my_username_   = username;
    my_tcp_port_   = tcp_port;
    my_public_key_ = public_key;
    on_peer_found_ = std::move(on_found);
    on_peer_lost_  = std::move(on_lost);

    // Broadcast-адрес для подсети 26.0.0.0/8 (Radmin VPN)
    // Используем 26.255.255.255, но также поддерживаем 255.255.255.255
    // как fallback для локальной сети
    broadcast_ep_ = udp::endpoint(
        boost::asio::ip::address_v4::broadcast(), port_);

    // Открываем UDP-сокет
    socket_.open(udp::v4());
    socket_.set_option(udp::socket::reuse_address(true));
    socket_.set_option(boost::asio::socket_base::broadcast(true));

    // Привязываемся к порту для приёма broadcast от других
    socket_.bind(udp::endpoint(udp::v4(), port_));

    running_ = true;
    spdlog::info("UDP discovery запущен на порту {}", port_);

    // Запускаем первый broadcast и начинаем слушать
    do_broadcast();
    schedule_broadcast();
    do_receive();
    schedule_timeout_check();
}

void UdpDiscovery::stop() {
    running_ = false;
    broadcast_timer_.cancel();
    timeout_timer_.cancel();

    boost::system::error_code ec;
    socket_.close(ec);

    spdlog::info("UDP discovery остановлен");
}

// ── Отправка broadcast ────────────────────────────────────────────────
void UdpDiscovery::do_broadcast() {
    if (!running_) return;

    json j;
    j["username"]   = my_username_;
    j["port"]       = my_tcp_port_;
    j["version"]    = "0.1.0";
    j["public_key"] = my_public_key_;

    std::string data = j.dump();

    socket_.async_send_to(
        boost::asio::buffer(data), broadcast_ep_,
        [this](boost::system::error_code ec, std::size_t /*bytes*/) {
            if (ec && running_) {
                spdlog::warn("Broadcast ошибка: {}", ec.message());
            }
        });
}

void UdpDiscovery::schedule_broadcast() {
    if (!running_) return;

    broadcast_timer_.expires_after(BROADCAST_INTERVAL);
    broadcast_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec && running_) {
            do_broadcast();
            schedule_broadcast();
        }
    });
}

// ── Приём broadcast от других ─────────────────────────────────────────
void UdpDiscovery::do_receive() {
    if (!running_) return;

    socket_.async_receive_from(
        boost::asio::buffer(recv_buf_), sender_ep_,
        [this](boost::system::error_code ec, std::size_t bytes) {
            if (ec) {
                if (running_) {
                    spdlog::warn("UDP receive ошибка: {}", ec.message());
                    do_receive();
                }
                return;
            }

            // Парсим JSON
            try {
                std::string raw(recv_buf_.data(), bytes);
                auto j = json::parse(raw);

                PeerInfo info;
                info.username   = j.value("username", "");
                info.ip         = sender_ep_.address().to_string();
                info.port       = j.value("port", static_cast<uint16_t>(0));
                info.version    = j.value("version", "");
                info.public_key = j.value("public_key", "");
                info.last_seen  = std::chrono::steady_clock::now();

                // Не добавляем самого себя (фильтр по public_key)
                if (info.public_key == my_public_key_ && !my_public_key_.empty()) {
                    do_receive();
                    return;
                }

                bool is_new = false;
                {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    auto it = peers_.find(info.ip);
                    is_new = (it == peers_.end());
                    peers_[info.ip] = info;
                }

                if (is_new) {
                    spdlog::info("Обнаружен пир: {} ({})", info.username, info.ip);
                }

                if (on_peer_found_) {
                    on_peer_found_(info);
                }

            } catch (const std::exception& e) {
                spdlog::warn("UDP: невалидный пакет от {}: {}",
                             sender_ep_.address().to_string(), e.what());
            }

            do_receive();
        });
}

// ── Проверка таймаутов ────────────────────────────────────────────────
void UdpDiscovery::check_timeouts() {
    if (!running_) return;

    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> lost;

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto it = peers_.begin(); it != peers_.end(); ) {
            if ((now - it->second.last_seen) > PEER_TIMEOUT) {
                spdlog::info("Пир {} ({}) — таймаут, удаляем",
                             it->second.username, it->first);
                lost.push_back(it->first);
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& ip : lost) {
        if (on_peer_lost_) on_peer_lost_(ip);
    }
}

void UdpDiscovery::schedule_timeout_check() {
    if (!running_) return;

    timeout_timer_.expires_after(PEER_TIMEOUT);
    timeout_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec && running_) {
            check_timeouts();
            schedule_timeout_check();
        }
    });
}

std::vector<PeerInfo> UdpDiscovery::peers() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    for (auto& [ip, info] : peers_) {
        result.push_back(info);
    }
    return result;
}

} // namespace lm::net
