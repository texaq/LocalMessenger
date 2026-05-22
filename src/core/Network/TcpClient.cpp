// ═══════════════════════════════════════════════════════════════════════
// TcpClient.cpp — Реализация TCP-клиента
// ═══════════════════════════════════════════════════════════════════════

#include "TcpClient.h"
#include <spdlog/spdlog.h>

namespace lm::net {

TcpClient::TcpClient(boost::asio::io_context& io)
    : io_(io) {}

void TcpClient::connect(const std::string& host, uint16_t port,
                         OnConnectCb on_connect, OnErrorCb on_error) {
    using boost::asio::ip::tcp;

    auto resolver = std::make_shared<tcp::resolver>(io_);
    auto socket   = std::make_shared<tcp::socket>(io_);

    // Резолвим адрес (для IP-адреса резолвинг мгновенный)
    resolver->async_resolve(
        host, std::to_string(port),
        [resolver, socket, on_connect, on_error, host, port]
        (boost::system::error_code ec, tcp::resolver::results_type results) {
            if (ec) {
                spdlog::error("Resolve ошибка для {}:{} — {}", host, port, ec.message());
                if (on_error) on_error(ec.message());
                return;
            }

            boost::asio::async_connect(
                *socket, results,
                [socket, on_connect, on_error, host, port]
                (boost::system::error_code ec, const tcp::endpoint& /*ep*/) {
                    if (ec) {
                        spdlog::error("Connect ошибка к {}:{} — {}",
                                      host, port, ec.message());
                        if (on_error) on_error(ec.message());
                        return;
                    }

                    spdlog::info("TCP подключение к {}:{} установлено", host, port);

                    auto session = std::make_shared<TcpSession>(std::move(*socket));
                    if (on_connect) on_connect(session);
                });
        });
}

} // namespace lm::net
