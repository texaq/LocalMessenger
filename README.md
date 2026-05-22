# LocalMessenger

P2P мессенджер для локальной сети через Radmin VPN (26.x.x.x).
Целевая платформа: **Windows 10/11**, опционально Linux.

## Возможности

### Этап 1 — Основа
- **TCP-сервер / клиент** на Boost.Asio — асинхронный обмен пакетами
- **UDP broadcast** — автоматическое обнаружение пиров в подсети 26.0.0.0/8
- **Бинарный протокол** — `[magic:4][type:1][length:4][payload:N]`
- **SQLite хранилище** — контакты, сообщения, медиафайлы, настройки
- **E2E-шифрование** — X25519 (ECDH) + AES-256-GCM / XChaCha20-Poly1305 (libsodium)
- **Конфигурация** — `settings.json` (nlohmann/json)
- **Логирование** — spdlog с ротацией файлов

### Этап 2 — Чат
- **Отправка/получение** текстовых сообщений через протокол с E2E шифрованием
- **Статусы доставки** — отправлено → доставлено → прочитано
- **Индикатор набора** — "печатает..." в реальном времени
- **Реакции** на сообщения (emoji)
- **История и поиск** по SQLite с пагинацией
- **Handshake** — обмен публичными ключами при подключении

### Этап 3 — Передача файлов
- **Chunked transfer** — разбивка на блоки по 64KB с ACK подтверждением
- **Прогресс** — скорость (KB/s) и оставшееся время (ETA)
- **Управление** — пауза, возобновление, отмена передачи
- **Отдельный порт** — TCP 7779 (не мешает основному чату)
- **Предупреждение** при файлах > 500MB

### Этап 4 — Звонки
- **Аудиозвонки** и **видеозвонки** — полная сигнализация
- **Состояния** — IDLE → OFFERING → RINGING → ACTIVE → ENDED
- **Управление** — mute микрофона, вкл/выкл камеры
- **Push-to-talk** и **VAD** (Voice Activity Detection)
- **Регулировка громкости** входа и выхода

### Этап 5 — Демонстрация экрана
- **Выбор источника** — весь экран, конкретный монитор, отдельное окно
- **Адаптивный FPS** — 5-30 fps в зависимости от нагрузки сети
- **Remote control** — опциональная передача событий мыши/клавиатуры
- **Управление** — старт, пауза, возобновление, остановка

## Зависимости

Управляются через [vcpkg](https://github.com/microsoft/vcpkg):

```
boost-asio, boost-beast, boost-uuid, sqlite3, sqlitecpp,
libsodium, nlohmann-json, spdlog, stb
```

## Сборка

```bash
# 1. Клонировать vcpkg (если ещё нет)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh   # Linux / macOS
# или: .\vcpkg\bootstrap-vcpkg.bat  # Windows

# 2. Сборка проекта
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=<путь-к-vcpkg>/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

## Запуск

```bash
./build/local_messenger [имя_пользователя]
```

### Команды консольного режима

| Команда | Описание |
|---------|----------|
| `/peers` | Список обнаруженных пиров |
| `/send <ip> <текст>` | Отправить сообщение (E2E шифрование) |
| `/file <ip> <путь>` | Отправить файл |
| `/call <ip>` | Аудиозвонок |
| `/video <ip>` | Видеозвонок |
| `/accept` | Принять входящий звонок |
| `/decline` | Отклонить входящий звонок |
| `/hangup` | Завершить звонок |
| `/screen <ip>` | Демонстрация экрана |
| `/stopscreen` | Остановить демонстрацию |
| `/history <ip>` | История чата с контактом |
| `/search <текст>` | Поиск по сообщениям |
| `/read <ip>` | Пометить как прочитанные |
| `/transfers` | Активные передачи файлов |
| `/quit` | Выход |

## Структура проекта

```
LocalMessenger/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── Network/
│   │   │   ├── Protocol.h/cpp        — бинарный протокол
│   │   │   ├── TcpServer.h/cpp       — TCP-сервер
│   │   │   ├── TcpSession.h/cpp      — TCP-сессия
│   │   │   ├── TcpClient.h/cpp       — TCP-клиент
│   │   │   ├── UdpDiscovery.h/cpp    — UDP broadcast discovery
│   │   │   └── NetworkManager.h/cpp  — центральный сетевой менеджер
│   │   ├── Chat/
│   │   │   ├── ChatMessage.h          — модель сообщения
│   │   │   └── ChatManager.h/cpp     — E2E чат, статусы, реакции
│   │   ├── FileTransfer/
│   │   │   └── FileTransferManager.h/cpp — chunked TCP передача
│   │   ├── Call/
│   │   │   └── CallManager.h/cpp     — сигнализация звонков
│   │   ├── ScreenShare/
│   │   │   └── ScreenShareManager.h/cpp — демонстрация экрана
│   │   └── Crypto/
│   │       └── CryptoManager.h/cpp   — X25519 + AES-256-GCM
│   ├── storage/
│   │   ├── Database.h/cpp
│   │   ├── ContactRepository.h/cpp
│   │   ├── MessageRepository.h/cpp
│   │   └── SettingsManager.h/cpp
│   └── ui/                          — (Qt 6 — в разработке)
└── tests/
```

## Порты по умолчанию

| Порт | Протокол | Назначение |
|------|----------|------------|
| 7777 | TCP | Чат (сообщения, сигнализация) |
| 7778 | UDP | Discovery + медиапоток |
| 7779 | TCP | Передача файлов |

Все порты конфигурируются в `settings.json`.

## Этапы разработки

- [x] **Этап 1** — Основа (сеть, протокол, БД, крипто)
- [x] **Этап 2** — Чат (E2E шифрование, статусы, реакции)
- [x] **Этап 3** — Передача файлов (chunked TCP, прогресс)
- [x] **Этап 4** — Звонки (сигнализация, управление)
- [x] **Этап 5** — Демонстрация экрана (каркас + remote control)
- [ ] **Qt 6 UI** — графический интерфейс

## Лицензия

MIT
