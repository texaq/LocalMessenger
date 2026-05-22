#pragma once
// ═══════════════════════════════════════════════════════════════════════
// TcpServer.h — Асинхронный TCP-сервер (принимает входящие соединения)
//
// Слушает на заданном порту, создаёт TcpSession для каждого клиента.
// ═══════════════════════════════════════════════════════════════════════

#include "TcpSession.h"

#include <boost/asio.hpp>
#include <cstdint>
#include <functional>

namespace lm::net {

class TcpServer {
public:
    /// Callback при новом соединении: (session)
    using OnAcceptCb = std::function<void(TcpSession::Ptr)>;

    /// @param io      — io_context для async-операций
    /// @param port    — порт, на котором слушаем (по умолчанию 7777)
    TcpServer(boost::asio::io_context& io, uint16_t port);

    /// Начать приём соединений
    void start(OnAcceptCb on_accept);

    /// Остановить приём
    void stop();

    uint16_t port() const { return port_; }

private:
    void do_accept();

    boost::asio::io_context&        io_;
    boost::asio::ip::tcp::acceptor  acceptor_;
    uint16_t                        port_;
    OnAcceptCb                      on_accept_;
    bool                            running_ = false;
};

} // namespace lm::net
