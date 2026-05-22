// ═══════════════════════════════════════════════════════════════════════
// SettingsManager.cpp — Управление настройками (JSON + SQLite)
// ═══════════════════════════════════════════════════════════════════════

#include "SettingsManager.h"
#include <spdlog/spdlog.h>
#include <fstream>

namespace lm::storage {

using json = nlohmann::json;

// ── JSON-сериализация ─────────────────────────────────────────────────

void to_json(json& j, const AppSettings& s) {
    j = json{
        {"username",      s.username},
        {"avatar_path",   s.avatar_path},
        {"tcp_port",      s.tcp_port},
        {"udp_port",      s.udp_port},
        {"file_port",     s.file_port},
        {"data_dir",      s.data_dir},
        {"download_dir",  s.download_dir},
        {"theme",         s.theme},
        {"accent_color",  s.accent_color},
        {"password_hash", s.password_hash},
    };
}

void from_json(const json& j, AppSettings& s) {
    if (j.contains("username"))      j.at("username").get_to(s.username);
    if (j.contains("avatar_path"))   j.at("avatar_path").get_to(s.avatar_path);
    if (j.contains("tcp_port"))      j.at("tcp_port").get_to(s.tcp_port);
    if (j.contains("udp_port"))      j.at("udp_port").get_to(s.udp_port);
    if (j.contains("file_port"))     j.at("file_port").get_to(s.file_port);
    if (j.contains("data_dir"))      j.at("data_dir").get_to(s.data_dir);
    if (j.contains("download_dir"))  j.at("download_dir").get_to(s.download_dir);
    if (j.contains("theme"))         j.at("theme").get_to(s.theme);
    if (j.contains("accent_color"))  j.at("accent_color").get_to(s.accent_color);
    if (j.contains("password_hash")) j.at("password_hash").get_to(s.password_hash);
}

// ── SettingsManager ───────────────────────────────────────────────────

SettingsManager::SettingsManager(const std::string& json_path, Database& db)
    : json_path_(json_path)
    , db_(db) {
    load();
}

void SettingsManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream ifs(json_path_);
    if (ifs.is_open()) {
        try {
            json j = json::parse(ifs);
            settings_ = j.get<AppSettings>();
            spdlog::info("Настройки загружены из {}", json_path_);
        } catch (const std::exception& e) {
            spdlog::warn("Ошибка чтения {}: {}. Используем настройки по умолчанию",
                         json_path_, e.what());
        }
    } else {
        spdlog::info("Файл {} не найден — создаём с настройками по умолчанию",
                     json_path_);
        // Сохраняем дефолтные настройки
        std::ofstream ofs(json_path_);
        if (ofs.is_open()) {
            json j = settings_;
            ofs << j.dump(2);
        }
    }
}

void SettingsManager::save() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream ofs(json_path_);
    if (ofs.is_open()) {
        json j = settings_;
        ofs << j.dump(2);
        spdlog::debug("Настройки сохранены в {}", json_path_);
    } else {
        spdlog::error("Не удалось сохранить настройки в {}", json_path_);
    }
}

AppSettings SettingsManager::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void SettingsManager::update(const AppSettings& settings) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;
    }
    save();
}

std::string SettingsManager::get_value(const std::string& key,
                                        const std::string& default_val) const {
    SQLite::Statement stmt(db_.db(),
        "SELECT value FROM settings WHERE key = ?");
    stmt.bind(1, key);

    if (stmt.executeStep()) {
        return stmt.getColumn(0).getString();
    }
    return default_val;
}

void SettingsManager::set_value(const std::string& key,
                                 const std::string& value) {
    SQLite::Statement stmt(db_.db(), R"SQL(
        INSERT INTO settings (key, value) VALUES (?, ?)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )SQL");
    stmt.bind(1, key);
    stmt.bind(2, value);
    stmt.exec();
}

} // namespace lm::storage
