#pragma once
// ═══════════════════════════════════════════════════════════════════════
// FileTransferManager.h — Передача файлов по отдельному TCP-соединению
//
// Chunked transfer: файл разбивается на блоки по 64KB,
// каждый блок подтверждается ACK.
// Поддержка: пауза, возобновление, отмена, прогресс.
// ═══════════════════════════════════════════════════════════════════════

#include "core/Network/Protocol.h"
#include <boost/asio.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lm::filetransfer {

constexpr size_t CHUNK_SIZE = 64 * 1024;  // 64 KB
constexpr size_t WARN_SIZE  = 500 * 1024 * 1024;  // 500 MB предупреждение

/// Состояние передачи
enum class TransferState {
    PENDING,     // ожидает начала
    ACTIVE,      // идёт передача
    PAUSED,      // на паузе
    COMPLETED,   // завершена
    CANCELLED,   // отменена
    ERROR,       // ошибка
};

/// Информация о передаче файла
struct TransferInfo {
    std::string  transfer_id;   // UUID передачи
    std::string  file_name;
    std::string  file_path;     // локальный путь
    uint64_t     file_size = 0;
    uint64_t     transferred = 0;
    uint32_t     total_chunks = 0;
    uint32_t     current_chunk = 0;
    TransferState state = TransferState::PENDING;
    bool         is_sender = false;
    std::string  peer_ip;
    std::string  mime_type;

    double progress() const {
        return file_size > 0 ? static_cast<double>(transferred) / file_size : 0.0;
    }

    double speed_kbps = 0.0;     // текущая скорость (KB/s)
    int    eta_seconds = 0;      // оставшееся время (секунды)
};

class FileTransferManager {
public:
    using OnProgressCb  = std::function<void(const TransferInfo&)>;
    using OnCompleteCb  = std::function<void(const TransferInfo&)>;
    using OnIncomingCb  = std::function<void(const TransferInfo&)>;  // запрос на приём

    FileTransferManager(boost::asio::io_context& io, uint16_t port);

    /// Запуск сервера передачи файлов
    void start(const std::string& download_dir);

    /// Остановка
    void stop();

    /// Отправить файл пиру
    /// @return transfer_id
    std::string send_file(const std::string& peer_ip, uint16_t peer_port,
                           const std::string& file_path);

    /// Принять входящий файл
    void accept_transfer(const std::string& transfer_id);

    /// Поставить передачу на паузу
    void pause_transfer(const std::string& transfer_id);

    /// Возобновить передачу
    void resume_transfer(const std::string& transfer_id);

    /// Отменить передачу
    void cancel_transfer(const std::string& transfer_id);

    /// Получить информацию о передаче
    std::optional<TransferInfo> get_transfer(const std::string& transfer_id) const;

    /// Все активные передачи
    std::vector<TransferInfo> active_transfers() const;

    // Callback-и
    void set_on_progress(OnProgressCb cb)  { on_progress_ = std::move(cb); }
    void set_on_complete(OnCompleteCb cb)  { on_complete_ = std::move(cb); }
    void set_on_incoming(OnIncomingCb cb)  { on_incoming_ = std::move(cb); }

private:
    void do_accept();
    void handle_incoming_connection(boost::asio::ip::tcp::socket socket);
    void send_chunks(const std::string& transfer_id);

    boost::asio::io_context&       io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t                       port_;
    std::string                    download_dir_;
    bool                           running_ = false;

    mutable std::mutex                                    transfers_mutex_;
    std::unordered_map<std::string, TransferInfo>          transfers_;

    OnProgressCb on_progress_;
    OnCompleteCb on_complete_;
    OnIncomingCb on_incoming_;
};

} // namespace lm::filetransfer
