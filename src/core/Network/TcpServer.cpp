// ═══════════════════════════════════════════════════════════════════════
// TcpServer.cpp — Реализация TCP-сервера
// ═══════════════════════════════════════════════════════════════════════

#include "TcpServer.h"
#include <spdlog/spdlog.h>

namespace lm::net {

TcpServer::TcpServer(boost::asio::io_context& io, uint16_t port)
    : io_(io)
    , acceptor_(io)
    , port_(port) {}

void TcpServer::start(OnAcceptCb on_accept) {
    on_accept_ = std::move(on_accept);

    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), port_);
    acceptor_.open(ep.protocol());

    // Переиспользуем адрес — удобно при перезапуске
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();

    running_ = true;
    spdlog::info("TCP сервер запущен на порту {}", port_);

    do_accept();
}

void TcpServer::stop() {
    running_ = false;
    boost::system::error_code ec;
    acceptor_.close(ec);
    spdlog::info("TCP сервер остановлен");
}

void TcpServer::do_accept() {
    if (!running_) return;

    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (ec) {
                if (running_) {
                    spdlog::error("Accept ошибка: {}", ec.message());
                }
                return;
            }

            auto addr = socket.remote_endpoint().address().to_string();
            spdlog::info("Новое TCP-соединение от {}", addr);

            auto session = std::make_shared<TcpSession>(std::move(socket));
            if (on_accept_) {
                on_accept_(session);
            }

            // Продолжаем принимать следующие соединения
            do_accept();
        });
}

} // namespace lm::net
