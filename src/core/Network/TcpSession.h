#pragma once
// ═══════════════════════════════════════════════════════════════════════
// TcpSession.h — Одно TCP-соединение с удалённым пиром
//
// Каждый TcpSession:
//   • Читает данные из сокета, собирает полные пакеты (header + payload)
//   • Асинхронно отправляет пакеты в очередь записи
//   • Вызывает callback при получении полного пакета
// ═══════════════════════════════════════════════════════════════════════

#include "Protocol.h"

#include <boost/asio.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace lm::net {

class TcpSession : public std::enable_shared_from_this<TcpSession> {
public:
    using Ptr = std::shared_ptr<TcpSession>;

    // Callback при получении пакета: (session, packet)
    using OnPacketCb  = std::function<void(Ptr, Packet)>;
    // Callback при закрытии сессии: (session, error_message)
    using OnCloseCb   = std::function<void(Ptr, std::string)>;

    explicit TcpSession(boost::asio::ip::tcp::socket socket);

    /// Запуск чтения из сокета
    void start(OnPacketCb on_packet, OnCloseCb on_close);

    /// Поставить пакет в очередь на отправку (потокобезопасно)
    void send(const Packet& packet);

    /// Закрыть соединение
    void close();

    /// IP-адрес удалённого пира
    std::string remote_address() const;

    /// Порт удалённого пира
    uint16_t remote_port() const;

    /// Проверка, жива ли сессия
    bool is_open() const;

private:
    // Чтение заголовка (HEADER_SIZE байт)
    void do_read_header();

    // Чтение payload (header_.length байт)
    void do_read_payload();

    // Запись первого пакета из очереди
    void do_write();

    // Обработка ошибки — закрытие сессии и вызов callback
    void handle_error(const std::string& context,
                      const boost::system::error_code& ec);

    boost::asio::ip::tcp::socket socket_;

    // Буфер для чтения заголовка
    std::array<uint8_t, HEADER_SIZE> header_buf_{};
    PacketHeader                     current_header_;

    // Буфер для чтения payload
    std::vector<uint8_t> payload_buf_;

    // Очередь пакетов на отправку
    std::mutex                  write_mutex_;
    std::deque<std::vector<uint8_t>> write_queue_;
    bool                        writing_ = false;

    OnPacketCb on_packet_;
    OnCloseCb  on_close_;
    bool       closed_ = false;
};

} // namespace lm::net
