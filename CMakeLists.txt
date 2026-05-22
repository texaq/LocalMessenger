cmake_minimum_required(VERSION 3.20)
project(LocalMessenger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Зависимости через vcpkg / find_package ───────────────────────────
find_package(Boost       REQUIRED COMPONENTS system)
find_package(SQLiteCpp   CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog      REQUIRED)

# ─── Основная библиотека ядра (без UI) ─────────────────────────────────
add_library(messenger_core STATIC
    # Протокол
    src/core/Network/Protocol.cpp
    # Сетевой движок
    src/core/Network/TcpServer.cpp
    src/core/Network/TcpSession.cpp
    src/core/Network/TcpClient.cpp
    src/core/Network/UdpDiscovery.cpp
    src/core/Network/NetworkManager.cpp
    # Чат
    src/core/Chat/ChatManager.cpp
    # Передача файлов
    src/core/FileTransfer/FileTransferManager.cpp
    # Звонки
    src/core/Call/CallManager.cpp
    # Демонстрация экрана
    src/core/ScreenShare/ScreenShareManager.cpp
    # Криптография
    src/core/Crypto/CryptoManager.cpp
    # Хранилище
    src/storage/Database.cpp
    src/storage/ContactRepository.cpp
    src/storage/MessageRepository.cpp
    src/storage/SettingsManager.cpp
)

target_include_directories(messenger_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(messenger_core PUBLIC
    Boost::system
    SQLiteCpp
    unofficial-sodium::sodium
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# На Linux нужен pthread
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(messenger_core PUBLIC Threads::Threads)
endif()

# ─── Консольное приложение (Этап 1, без Qt) ────────────────────────────
add_executable(local_messenger src/main.cpp)
target_link_libraries(local_messenger PRIVATE messenger_core)

# ─── Тесты ──────────────────────────────────────────────────────────────
option(BUILD_TESTS "Собрать тесты" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
