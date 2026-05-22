# LocalMessenger

P2P мессенджер для локальной сети через Radmin VPN (26.x.x.x).

## Возможности (Этап 1 — Основа)

- **TCP-сервер / клиент** на Boost.Asio — асинхронный обмен пакетами
- **UDP broadcast** — автоматическое обнаружение пиров в подсети
- **Бинарный протокол** — `[magic:4][type:1][length:4][payload:N]`
- **SQLite хранилище** — контакты, сообщения, медиафайлы, настройки
- **E2E-шифрование** — X25519 (ECDH) + AES-256-GCM / XChaCha20-Poly1305 (libsodium)
- **Конфигурация** — `settings.json` (nlohmann/json)
- **Логирование** — spdlog с ротацией файлов

## Зависимости

Управляются через [vcpkg](https://github.com/microsoft/vcpkg):

```
boost-asio, boost-beast, boost-uuid, sqlite3, sqlitecpp,
libsodium, nlohmann-json, spdlog, msgpack-cxx, stb
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
| `/send <ip> <текст>` | Отправить сообщение |
| `/history <ip>` | История чата с контактом |
| `/search <текст>` | Поиск по сообщениям |
| `/quit` | Выход |

## Структура проекта

```
LocalMessenger/
├── CMakeLists.txt          # Основной файл сборки
├── vcpkg.json              # Манифест зависимостей vcpkg
├── src/
│   ├── main.cpp            # Точка входа (консольный режим)
│   ├── core/
│   │   ├── Network/
│   │   │   ├── Protocol.h/cpp        # Бинарный протокол
│   │   │   ├── TcpServer.h/cpp       # TCP-сервер
│   │   │   ├── TcpSession.h/cpp      # TCP-сессия (одно соединение)
│   │   │   ├── TcpClient.h/cpp       # TCP-клиент
│   │   │   ├── UdpDiscovery.h/cpp    # UDP broadcast discovery
│   │   │   └── NetworkManager.h/cpp  # Центральный сетевой менеджер
│   │   ├── Crypto/
│   │   │   └── CryptoManager.h/cpp   # X25519 + AES-256-GCM
│   │   ├── Chat/                     # (Этап 2)
│   │   ├── Call/                     # (Этап 4)
│   │   ├── FileTransfer/            # (Этап 3)
│   │   └── ScreenShare/             # (Этап 5)
│   ├── storage/
│   │   ├── Database.h/cpp            # SQLite инициализация + миграции
│   │   ├── ContactRepository.h/cpp   # CRUD контактов
│   │   ├── MessageRepository.h/cpp   # CRUD сообщений
│   │   └── SettingsManager.h/cpp     # JSON настройки + key-value
│   └── ui/                          # (Этап 2 — Qt 6)
└── tests/                           # Тесты
```

## Порты по умолчанию

| Порт | Протокол | Назначение |
|------|----------|------------|
| 7777 | TCP | Чат (сообщения, сигнализация) |
| 7778 | UDP | Discovery + медиапоток |
| 7779 | TCP | Передача файлов |

Все порты конфигурируются в `settings.json`.

## Планируемые этапы

- [x] **Этап 1** — Основа (сеть, протокол, БД, крипто)
- [ ] **Этап 2** — Чат (текст, история, шифрование, Qt UI)
- [ ] **Этап 3** — Медиа (файлы, изображения, голосовые)
- [ ] **Этап 4** — Звонки (аудио/видео, Opus, H.264)
- [ ] **Этап 5** — Демо экрана (DXGI, remote control)

## Лицензия

MIT
