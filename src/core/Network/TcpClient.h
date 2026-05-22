#pragma once
// ═══════════════════════════════════════════════════════════════════════
// TcpClient.h — Асинхронный TCP-клиент (исходящее соединение к пиру)
// ═══════════════════════════════════════════════════════════════════════

#include "TcpSession.h"

#include <boost/asio.hpp>
#include <functional>
#include <string>

namespace lm::net {

class TcpClient {
public:
    /// Callback при успешном подключении: (session)
    using OnConnectCb = std::function<void(TcpSession::Ptr)>;
    /// Callback при ошибке: (error_message)
    using OnErrorCb   = std::function<void(std::string)>;

    explicit TcpClient(boost::asio::io_context& io);

    /// Асинхронно подключиться к пиру по IP и порту
    void connect(const std::string& host, uint16_t port,
                 OnConnectCb on_connect, OnErrorCb on_error);

private:
    boost::asio::io_context& io_;
};

} // namespace lm::net
