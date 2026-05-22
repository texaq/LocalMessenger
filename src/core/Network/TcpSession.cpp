// ═══════════════════════════════════════════════════════════════════════
// TcpSession.cpp — Реализация TCP-сессии с пиром
// ═══════════════════════════════════════════════════════════════════════

#include "TcpSession.h"
#include <spdlog/spdlog.h>

namespace lm::net {

TcpSession::TcpSession(boost::asio::ip::tcp::socket socket)
    : socket_(std::move(socket)) {}

void TcpSession::start(OnPacketCb on_packet, OnCloseCb on_close) {
    on_packet_ = std::move(on_packet);
    on_close_  = std::move(on_close);
    do_read_header();
}

// ── Чтение заголовка ──────────────────────────────────────────────────
void TcpSession::do_read_header() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_,
        boost::asio::buffer(header_buf_),
        [this, self](boost::system::error_code ec, std::size_t /*bytes*/) {
            if (ec) {
                handle_error("read_header", ec);
                return;
            }

            auto hdr = deserialize_header(header_buf_.data(), HEADER_SIZE);
            if (!hdr) {
                handle_error("bad_header", boost::asio::error::invalid_argument);
                return;
            }

            current_header_ = *hdr;

            // Если payload пустой — сразу доставляем пакет
            if (current_header_.length == 0) {
                Packet pkt;
                pkt.header = current_header_;
                if (on_packet_) on_packet_(self, std::move(pkt));
                do_read_header();
            } else {
                payload_buf_.resize(current_header_.length);
                do_read_payload();
            }
        });
}

// ── Чтение payload ───────────────────────────────────────────────────
void TcpSession::do_read_payload() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_,
        boost::asio::buffer(payload_buf_),
        [this, self](boost::system::error_code ec, std::size_t /*bytes*/) {
            if (ec) {
                handle_error("read_payload", ec);
                return;
            }

            Packet pkt;
            pkt.header  = current_header_;
            pkt.payload = std::move(payload_buf_);

            if (on_packet_) on_packet_(self, std::move(pkt));

            // Продолжаем читать следующий пакет
            do_read_header();
        });
}

// ── Отправка пакета ───────────────────────────────────────────────────
void TcpSession::send(const Packet& packet) {
    auto data = serialize(packet);

    std::lock_guard<std::mutex> lock(write_mutex_);
    write_queue_.push_back(std::move(data));

    // Если запись не идёт — запускаем
    if (!writing_) {
        writing_ = true;
        do_write();
    }
}

void TcpSession::do_write() {
    auto self = shared_from_this();

    // Забираем первый буфер из очереди (под мьютексом)
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        buf = std::move(write_queue_.front());
        write_queue_.pop_front();
    }

    // Копируем в shared_ptr, чтобы буфер жил до завершения async_write
    auto shared_buf = std::make_shared<std::vector<uint8_t>>(std::move(buf));

    boost::asio::async_write(
        socket_,
        boost::asio::buffer(*shared_buf),
        [this, self, shared_buf](boost::system::error_code ec, std::size_t /*bytes*/) {
            if (ec) {
                handle_error("write", ec);
                return;
            }
            do_write();  // пишем следующий из очереди
        });
}

// ── Закрытие ──────────────────────────────────────────────────────────
void TcpSession::close() {
    if (closed_) return;
    closed_ = true;

    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

std::string TcpSession::remote_address() const {
    try {
        return socket_.remote_endpoint().address().to_string();
    } catch (...) {
        return "unknown";
    }
}

uint16_t TcpSession::remote_port() const {
    try {
        return socket_.remote_endpoint().port();
    } catch (...) {
        return 0;
    }
}

bool TcpSession::is_open() const {
    return socket_.is_open() && !closed_;
}

void TcpSession::handle_error(const std::string& context,
                               const boost::system::error_code& ec) {
    if (closed_) return;
    closed_ = true;

    std::string msg = context + ": " + ec.message();
    spdlog::warn("TcpSession [{}]: {}", remote_address(), msg);

    boost::system::error_code ignore;
    socket_.close(ignore);

    if (on_close_) on_close_(shared_from_this(), msg);
}

} // namespace lm::net
