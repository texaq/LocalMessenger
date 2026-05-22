// ═══════════════════════════════════════════════════════════════════════
// FileTransferManager.cpp — Реализация передачи файлов
// ═══════════════════════════════════════════════════════════════════════

#include "FileTransferManager.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lm::filetransfer {

// Генерация UUID для передачи
static std::string gen_transfer_id() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(gen);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << a;
    return "ft-" + ss.str();
}

FileTransferManager::FileTransferManager(boost::asio::io_context& io,
                                          uint16_t port)
    : io_(io)
    , acceptor_(io)
    , port_(port) {}

void FileTransferManager::start(const std::string& download_dir) {
    download_dir_ = download_dir;
    fs::create_directories(download_dir_);

    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), port_);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    running_ = true;

    spdlog::info("FileTransfer сервер запущен на порту {}", port_);
    do_accept();
}

void FileTransferManager::stop() {
    running_ = false;
    boost::system::error_code ec;
    acceptor_.close(ec);
    spdlog::info("FileTransfer сервер остановлен");
}

void FileTransferManager::do_accept() {
    if (!running_) return;

    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec) {
                handle_incoming_connection(std::move(socket));
            }
            do_accept();
        });
}

void FileTransferManager::handle_incoming_connection(
    boost::asio::ip::tcp::socket socket) {

    auto peer_ip = socket.remote_endpoint().address().to_string();
    spdlog::info("FileTransfer: входящее соединение от {}", peer_ip);

    // Читаем метаданные файла (первый пакет — FILE_META)
    auto shared_socket = std::make_shared<boost::asio::ip::tcp::socket>(
        std::move(socket));
    auto header_buf = std::make_shared<std::array<uint8_t, net::HEADER_SIZE>>();

    boost::asio::async_read(
        *shared_socket,
        boost::asio::buffer(*header_buf),
        [this, shared_socket, header_buf, peer_ip]
        (boost::system::error_code ec, std::size_t) {
            if (ec) {
                spdlog::warn("FileTransfer: ошибка чтения заголовка: {}",
                             ec.message());
                return;
            }

            auto hdr = net::deserialize_header(header_buf->data(),
                                                net::HEADER_SIZE);
            if (!hdr || hdr->type != net::PacketType::FILE_META) {
                spdlog::warn("FileTransfer: ожидался FILE_META пакет");
                return;
            }

            // Читаем payload метаданных
            auto meta_buf = std::make_shared<std::vector<uint8_t>>(hdr->length);
            boost::asio::async_read(
                *shared_socket,
                boost::asio::buffer(*meta_buf),
                [this, shared_socket, meta_buf, peer_ip]
                (boost::system::error_code ec, std::size_t) {
                    if (ec) return;

                    try {
                        std::string raw(meta_buf->begin(), meta_buf->end());
                        auto j = json::parse(raw);

                        TransferInfo info;
                        info.transfer_id  = j.value("transfer_id", gen_transfer_id());
                        info.file_name    = j.value("file_name", "unknown");
                        info.file_size    = j.value("file_size", static_cast<uint64_t>(0));
                        info.total_chunks = j.value("total_chunks", static_cast<uint32_t>(0));
                        info.mime_type    = j.value("mime_type", "");
                        info.peer_ip      = peer_ip;
                        info.is_sender    = false;
                        info.state        = TransferState::ACTIVE;
                        info.file_path    = download_dir_ + "/" + info.file_name;

                        // Проверка размера
                        if (info.file_size > WARN_SIZE) {
                            spdlog::warn("Файл {} > 500MB ({} байт)",
                                         info.file_name, info.file_size);
                        }

                        {
                            std::lock_guard<std::mutex> lock(transfers_mutex_);
                            transfers_[info.transfer_id] = info;
                        }

                        if (on_incoming_) on_incoming_(info);

                        // Начинаем приём чанков
                        auto out = std::make_shared<std::ofstream>(
                            info.file_path, std::ios::binary);
                        auto tid = info.transfer_id;
                        auto start_time = std::make_shared<
                            std::chrono::steady_clock::time_point>(
                                std::chrono::steady_clock::now());

                        std::function<void()> read_chunk;
                        read_chunk = [this, shared_socket, out, tid,
                                      start_time, &read_chunk]() {
                            auto h_buf = std::make_shared<
                                std::array<uint8_t, net::HEADER_SIZE>>();

                            boost::asio::async_read(
                                *shared_socket,
                                boost::asio::buffer(*h_buf),
                                [this, shared_socket, out, tid,
                                 start_time, h_buf, &read_chunk]
                                (boost::system::error_code ec, std::size_t) {
                                    if (ec) {
                                        out->close();
                                        std::lock_guard<std::mutex> lock(
                                            transfers_mutex_);
                                        if (transfers_.count(tid)) {
                                            transfers_[tid].state =
                                                TransferState::ERROR;
                                        }
                                        return;
                                    }

                                    auto hdr = net::deserialize_header(
                                        h_buf->data(), net::HEADER_SIZE);
                                    if (!hdr) return;

                                    if (hdr->type == net::PacketType::FILE_CANCEL) {
                                        out->close();
                                        std::lock_guard<std::mutex> lock(
                                            transfers_mutex_);
                                        if (transfers_.count(tid)) {
                                            transfers_[tid].state =
                                                TransferState::CANCELLED;
                                        }
                                        return;
                                    }

                                    auto c_buf = std::make_shared<
                                        std::vector<uint8_t>>(hdr->length);
                                    boost::asio::async_read(
                                        *shared_socket,
                                        boost::asio::buffer(*c_buf),
                                        [this, shared_socket, out, tid,
                                         start_time, c_buf, &read_chunk]
                                        (boost::system::error_code ec,
                                         std::size_t) {
                                            if (ec) return;

                                            out->write(
                                                reinterpret_cast<const char*>(
                                                    c_buf->data()),
                                                c_buf->size());

                                            TransferInfo info;
                                            {
                                                std::lock_guard<std::mutex> lock(
                                                    transfers_mutex_);
                                                auto& t = transfers_[tid];
                                                t.transferred += c_buf->size();
                                                t.current_chunk++;

                                                auto elapsed =
                                                    std::chrono::steady_clock::now()
                                                    - *start_time;
                                                auto secs =
                                                    std::chrono::duration<double>(
                                                        elapsed).count();
                                                if (secs > 0) {
                                                    t.speed_kbps =
                                                        (t.transferred / 1024.0)
                                                        / secs;
                                                    auto remaining =
                                                        t.file_size
                                                        - t.transferred;
                                                    t.eta_seconds =
                                                        static_cast<int>(
                                                            (remaining
                                                             / 1024.0)
                                                            / t.speed_kbps);
                                                }

                                                info = t;
                                            }

                                            if (on_progress_) on_progress_(info);

                                            // Отправляем ACK
                                            auto ack = net::make_packet(
                                                net::PacketType::FILE_ACK,
                                                std::to_string(
                                                    info.current_chunk));
                                            auto ack_data = net::serialize(ack);
                                            boost::asio::async_write(
                                                *shared_socket,
                                                boost::asio::buffer(ack_data),
                                                [](boost::system::error_code,
                                                   std::size_t) {});

                                            // Проверяем завершение
                                            if (info.transferred
                                                >= info.file_size) {
                                                out->close();
                                                {
                                                    std::lock_guard<std::mutex>
                                                        lock(transfers_mutex_);
                                                    transfers_[tid].state =
                                                        TransferState::COMPLETED;
                                                }
                                                spdlog::info(
                                                    "Файл {} принят",
                                                    info.file_name);
                                                if (on_complete_)
                                                    on_complete_(info);
                                            } else {
                                                read_chunk();
                                            }
                                        });
                                });
                        };

                        read_chunk();

                    } catch (const std::exception& e) {
                        spdlog::error("FileTransfer meta parse: {}",
                                      e.what());
                    }
                });
        });
}

// ── Отправка файла ────────────────────────────────────────────────────
std::string FileTransferManager::send_file(const std::string& peer_ip,
                                            uint16_t peer_port,
                                            const std::string& file_path) {
    if (!fs::exists(file_path)) {
        spdlog::error("Файл не найден: {}", file_path);
        return "";
    }

    auto file_size = fs::file_size(file_path);
    auto file_name = fs::path(file_path).filename().string();
    auto transfer_id = gen_transfer_id();

    if (file_size > WARN_SIZE) {
        spdlog::warn("Файл {} > 500MB ({} байт)", file_name, file_size);
    }

    uint32_t total_chunks = static_cast<uint32_t>(
        (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE);

    TransferInfo info;
    info.transfer_id  = transfer_id;
    info.file_name    = file_name;
    info.file_path    = file_path;
    info.file_size    = file_size;
    info.total_chunks = total_chunks;
    info.peer_ip      = peer_ip;
    info.is_sender    = true;
    info.state        = TransferState::ACTIVE;

    {
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        transfers_[transfer_id] = info;
    }

    // Подключаемся к пиру на порт файлов
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
    auto endpoint = boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address(peer_ip), peer_port);

    socket->async_connect(endpoint,
        [this, socket, transfer_id, file_path, file_name,
         file_size, total_chunks]
        (boost::system::error_code ec) {
            if (ec) {
                spdlog::error("FileTransfer: connect ошибка: {}", ec.message());
                std::lock_guard<std::mutex> lock(transfers_mutex_);
                if (transfers_.count(transfer_id)) {
                    transfers_[transfer_id].state = TransferState::ERROR;
                }
                return;
            }

            // Отправляем метаданные
            json meta;
            meta["transfer_id"]  = transfer_id;
            meta["file_name"]    = file_name;
            meta["file_size"]    = file_size;
            meta["total_chunks"] = total_chunks;

            auto meta_pkt = net::make_packet(net::PacketType::FILE_META,
                                              meta.dump());
            auto meta_data = std::make_shared<std::vector<uint8_t>>(
                net::serialize(meta_pkt));

            boost::asio::async_write(*socket,
                boost::asio::buffer(*meta_data),
                [this, socket, transfer_id, file_path, meta_data]
                (boost::system::error_code ec, std::size_t) {
                    if (ec) return;

                    // Начинаем отправку чанков
                    auto ifs = std::make_shared<std::ifstream>(
                        file_path, std::ios::binary);
                    auto start_time = std::make_shared<
                        std::chrono::steady_clock::time_point>(
                            std::chrono::steady_clock::now());

                    std::function<void()> send_next;
                    send_next = [this, socket, ifs, transfer_id,
                                 start_time, &send_next]() {
                        // Проверяем состояние (пауза/отмена)
                        {
                            std::lock_guard<std::mutex> lock(transfers_mutex_);
                            auto it = transfers_.find(transfer_id);
                            if (it == transfers_.end()) return;
                            if (it->second.state == TransferState::CANCELLED) {
                                auto cancel = net::make_packet(
                                    net::PacketType::FILE_CANCEL, "");
                                auto data = net::serialize(cancel);
                                boost::asio::write(*socket,
                                    boost::asio::buffer(data));
                                return;
                            }
                            if (it->second.state == TransferState::PAUSED) {
                                return;  // TODO: возобновить по resume
                            }
                        }

                        auto chunk = std::make_shared<std::vector<uint8_t>>(
                            CHUNK_SIZE);
                        ifs->read(reinterpret_cast<char*>(chunk->data()),
                                  CHUNK_SIZE);
                        auto bytes_read = static_cast<size_t>(ifs->gcount());

                        if (bytes_read == 0) {
                            ifs->close();
                            return;
                        }

                        chunk->resize(bytes_read);

                        auto pkt = net::make_packet(
                            net::PacketType::FILE_CHUNK, *chunk);
                        auto data = std::make_shared<std::vector<uint8_t>>(
                            net::serialize(pkt));

                        boost::asio::async_write(*socket,
                            boost::asio::buffer(*data),
                            [this, socket, ifs, transfer_id,
                             bytes_read, start_time, data, &send_next]
                            (boost::system::error_code ec, std::size_t) {
                                if (ec) return;

                                // Ждём ACK
                                auto ack_buf = std::make_shared<
                                    std::array<uint8_t, net::HEADER_SIZE>>();
                                boost::asio::async_read(*socket,
                                    boost::asio::buffer(*ack_buf),
                                    [this, socket, ifs, transfer_id,
                                     bytes_read, start_time, ack_buf,
                                     &send_next]
                                    (boost::system::error_code ec,
                                     std::size_t) {
                                        if (ec) return;

                                        // Читаем ACK payload (если есть)
                                        auto hdr = net::deserialize_header(
                                            ack_buf->data(), net::HEADER_SIZE);
                                        if (hdr && hdr->length > 0) {
                                            std::vector<uint8_t> ack_payload(
                                                hdr->length);
                                            boost::asio::read(*socket,
                                                boost::asio::buffer(
                                                    ack_payload));
                                        }

                                        // Обновляем прогресс
                                        TransferInfo info;
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                transfers_mutex_);
                                            auto& t = transfers_[transfer_id];
                                            t.transferred += bytes_read;
                                            t.current_chunk++;

                                            auto elapsed =
                                                std::chrono::steady_clock::now()
                                                - *start_time;
                                            auto secs =
                                                std::chrono::duration<double>(
                                                    elapsed).count();
                                            if (secs > 0) {
                                                t.speed_kbps =
                                                    (t.transferred / 1024.0)
                                                    / secs;
                                                auto remaining =
                                                    t.file_size - t.transferred;
                                                t.eta_seconds =
                                                    static_cast<int>(
                                                        (remaining / 1024.0)
                                                        / t.speed_kbps);
                                            }

                                            if (t.transferred >= t.file_size) {
                                                t.state =
                                                    TransferState::COMPLETED;
                                            }

                                            info = t;
                                        }

                                        if (on_progress_) on_progress_(info);

                                        if (info.state
                                            == TransferState::COMPLETED) {
                                            spdlog::info(
                                                "Файл {} отправлен",
                                                info.file_name);
                                            if (on_complete_)
                                                on_complete_(info);
                                        } else {
                                            send_next();
                                        }
                                    });
                            });
                    };

                    send_next();
                });
        });

    return transfer_id;
}

void FileTransferManager::pause_transfer(const std::string& transfer_id) {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    auto it = transfers_.find(transfer_id);
    if (it != transfers_.end() && it->second.state == TransferState::ACTIVE) {
        it->second.state = TransferState::PAUSED;
        spdlog::info("Передача {} на паузе", transfer_id);
    }
}

void FileTransferManager::resume_transfer(const std::string& transfer_id) {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    auto it = transfers_.find(transfer_id);
    if (it != transfers_.end() && it->second.state == TransferState::PAUSED) {
        it->second.state = TransferState::ACTIVE;
        spdlog::info("Передача {} возобновлена", transfer_id);
    }
}

void FileTransferManager::cancel_transfer(const std::string& transfer_id) {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    auto it = transfers_.find(transfer_id);
    if (it != transfers_.end()) {
        it->second.state = TransferState::CANCELLED;
        spdlog::info("Передача {} отменена", transfer_id);
    }
}

std::optional<TransferInfo> FileTransferManager::get_transfer(
    const std::string& transfer_id) const {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    auto it = transfers_.find(transfer_id);
    if (it != transfers_.end()) return it->second;
    return std::nullopt;
}

std::vector<TransferInfo> FileTransferManager::active_transfers() const {
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    std::vector<TransferInfo> result;
    for (auto& [id, info] : transfers_) {
        if (info.state == TransferState::ACTIVE ||
            info.state == TransferState::PAUSED) {
            result.push_back(info);
        }
    }
    return result;
}

void FileTransferManager::accept_transfer(const std::string& /*transfer_id*/) {
    // В текущей реализации передачи принимаются автоматически.
    // Этот метод зарезервирован для UI-подтверждения.
}

} // namespace lm::filetransfer
