#pragma once
// ═══════════════════════════════════════════════════════════════════════
// SettingsManager.h — Управление настройками приложения
//
// Настройки хранятся в двух местах:
//   1. settings.json — основной файл конфигурации (порты, темы, пути)
//   2. SQLite таблица settings — для быстрого key-value доступа
//
// При старте settings.json загружается в память; изменения
// синхронизируются обратно в файл.
// ═══════════════════════════════════════════════════════════════════════

#include "Database.h"
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>

namespace lm::storage {

struct AppSettings {
    // Профиль пользователя
    std::string username    = "User";
    std::string avatar_path;

    // Сеть
    uint16_t tcp_port       = 7777;   // чат
    uint16_t udp_port       = 7778;   // discovery + медиа
    uint16_t file_port      = 7779;   // передача файлов

    // Пути хранения
    std::string data_dir;     // %APPDATA%/LocalMessenger или ~/.local/share/LocalMessenger
    std::string download_dir; // папка загрузок файлов

    // UI
    std::string theme       = "dark"; // "light" / "dark"
    std::string accent_color = "#2B7FE0";

    // Безопасность
    std::string password_hash;  // хеш пароля профиля (опционально)
};

class SettingsManager {
public:
    /// @param json_path — путь к settings.json
    /// @param db        — ссылка на БД (для key-value хранилища)
    SettingsManager(const std::string& json_path, Database& db);

    /// Загрузить настройки из settings.json
    void load();

    /// Сохранить текущие настройки в settings.json
    void save();

    /// Получить текущие настройки (потокобезопасно)
    AppSettings get() const;

    /// Обновить настройки (потокобезопасно)
    void update(const AppSettings& settings);

    /// Прочитать одно значение из SQLite key-value
    std::string get_value(const std::string& key,
                           const std::string& default_val = "") const;

    /// Записать одно значение в SQLite key-value
    void set_value(const std::string& key, const std::string& value);

private:
    std::string json_path_;
    Database&   db_;

    mutable std::mutex mutex_;
    AppSettings        settings_;
};

// JSON сериализация для AppSettings
void to_json(nlohmann::json& j, const AppSettings& s);
void from_json(const nlohmann::json& j, AppSettings& s);

} // namespace lm::storage
