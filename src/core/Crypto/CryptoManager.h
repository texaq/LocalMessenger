#pragma once
// ═══════════════════════════════════════════════════════════════════════
// CryptoManager.h — Шифрование сообщений (E2E)
//
// Используем libsodium:
//   • X25519 (Curve25519) — обмен ключами (ECDH)
//   • AES-256-GCM — шифрование сообщений (через crypto_aead_aes256gcm)
//   • Если AES-256-GCM не поддерживается аппаратно, fallback на
//     XChaCha20-Poly1305 (crypto_aead_xchacha20poly1305_ietf)
// ═══════════════════════════════════════════════════════════════════════

#include <array>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace lm::crypto {

// Размеры ключей в байтах
constexpr size_t KEY_SIZE        = 32;  // X25519 / AES-256
constexpr size_t NONCE_SIZE_GCM  = 12;  // AES-256-GCM nonce
constexpr size_t NONCE_SIZE_XCHACHA = 24; // XChaCha20 nonce
constexpr size_t TAG_SIZE        = 16;  // AEAD tag

using KeyBytes   = std::array<uint8_t, KEY_SIZE>;
using NonceBytes = std::vector<uint8_t>;

/// Пара ключей X25519 (публичный + приватный)
struct KeyPair {
    KeyBytes public_key;
    KeyBytes secret_key;
};

class CryptoManager {
public:
    CryptoManager();
    ~CryptoManager();

    /// Инициализация libsodium (вызывать один раз при старте)
    static bool initialize();

    /// Генерация новой пары ключей X25519
    KeyPair generate_keypair() const;

    /// Вычисление общего секрета (shared key) из своего приватного
    /// и чужого публичного ключа через X25519 ECDH
    KeyBytes compute_shared_key(const KeyBytes& my_secret,
                                const KeyBytes& peer_public) const;

    /// Шифрование данных с помощью AES-256-GCM (или XChaCha20-Poly1305)
    /// Возвращает: nonce + ciphertext + tag
    std::vector<uint8_t> encrypt(const KeyBytes& shared_key,
                                  const std::vector<uint8_t>& plaintext) const;

    /// Расшифровка данных
    /// На входе: nonce + ciphertext + tag
    /// Возвращает plaintext или nullopt при ошибке (MAC mismatch)
    std::optional<std::vector<uint8_t>> decrypt(
        const KeyBytes& shared_key,
        const std::vector<uint8_t>& encrypted) const;

    /// Публичный ключ в hex-строку (для передачи в broadcast)
    static std::string key_to_hex(const KeyBytes& key);

    /// Hex-строку в ключ
    static std::optional<KeyBytes> hex_to_key(const std::string& hex);

    /// Fingerprint ключа (первые 8 байт SHA-256, отображаемые как hex)
    static std::string fingerprint(const KeyBytes& public_key);

    /// Проверка аппаратной поддержки AES-256-GCM
    static bool has_aes_gcm_support();

private:
    bool use_aes_gcm_ = false; // true если CPU поддерживает AES-NI
};

} // namespace lm::crypto
