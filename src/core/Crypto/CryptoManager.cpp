// ═══════════════════════════════════════════════════════════════════════
// CryptoManager.cpp — Реализация E2E-шифрования через libsodium
// ═══════════════════════════════════════════════════════════════════════

#include "CryptoManager.h"
#include <sodium.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace lm::crypto {

CryptoManager::CryptoManager() {
    // Проверяем аппаратную поддержку AES-256-GCM
    use_aes_gcm_ = has_aes_gcm_support();
    if (use_aes_gcm_) {
        spdlog::info("Crypto: используем AES-256-GCM (аппаратное ускорение)");
    } else {
        spdlog::info("Crypto: используем XChaCha20-Poly1305 (fallback)");
    }
}

CryptoManager::~CryptoManager() = default;

bool CryptoManager::initialize() {
    if (sodium_init() < 0) {
        spdlog::error("Не удалось инициализировать libsodium!");
        return false;
    }
    spdlog::info("libsodium инициализирован");
    return true;
}

// ── Генерация ключей X25519 ──────────────────────────────────────────
KeyPair CryptoManager::generate_keypair() const {
    KeyPair kp;
    crypto_box_keypair(kp.public_key.data(), kp.secret_key.data());
    return kp;
}

// ── Вычисление shared key через X25519 ECDH ──────────────────────────
// Используем crypto_scalarmult для raw X25519, затем хешируем через
// crypto_generichash (BLAKE2b) для получения финального ключа.
KeyBytes CryptoManager::compute_shared_key(const KeyBytes& my_secret,
                                            const KeyBytes& peer_public) const {
    // Сырой ECDH
    KeyBytes raw_shared;
    if (crypto_scalarmult(raw_shared.data(),
                          my_secret.data(),
                          peer_public.data()) != 0) {
        spdlog::error("crypto_scalarmult не удался");
        // Возвращаем нули — вызывающий код должен проверить
        raw_shared.fill(0);
        return raw_shared;
    }

    // Хешируем raw_shared в финальный ключ (защита от атак на raw ECDH)
    KeyBytes final_key;
    crypto_generichash(final_key.data(), KEY_SIZE,
                       raw_shared.data(), KEY_SIZE,
                       nullptr, 0);

    // Безопасно очищаем сырой shared secret
    sodium_memzero(raw_shared.data(), KEY_SIZE);

    return final_key;
}

// ── Шифрование ────────────────────────────────────────────────────────
std::vector<uint8_t> CryptoManager::encrypt(
    const KeyBytes& shared_key,
    const std::vector<uint8_t>& plaintext) const {

    if (use_aes_gcm_) {
        // AES-256-GCM
        size_t nonce_size = crypto_aead_aes256gcm_NPUBBYTES;  // 12
        size_t tag_size   = crypto_aead_aes256gcm_ABYTES;     // 16

        std::vector<uint8_t> result(nonce_size + plaintext.size() + tag_size);
        uint8_t* nonce = result.data();
        uint8_t* ct    = result.data() + nonce_size;

        // Генерируем случайный nonce
        randombytes_buf(nonce, nonce_size);

        unsigned long long ct_len = 0;
        crypto_aead_aes256gcm_encrypt(
            ct, &ct_len,
            plaintext.data(), plaintext.size(),
            nullptr, 0,         // additional data (нет)
            nullptr,            // nsec (не используется)
            nonce,
            shared_key.data());

        result.resize(nonce_size + ct_len);
        return result;
    } else {
        // XChaCha20-Poly1305
        size_t nonce_size = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
        size_t tag_size   = crypto_aead_xchacha20poly1305_ietf_ABYTES;    // 16

        std::vector<uint8_t> result(nonce_size + plaintext.size() + tag_size);
        uint8_t* nonce = result.data();
        uint8_t* ct    = result.data() + nonce_size;

        randombytes_buf(nonce, nonce_size);

        unsigned long long ct_len = 0;
        crypto_aead_xchacha20poly1305_ietf_encrypt(
            ct, &ct_len,
            plaintext.data(), plaintext.size(),
            nullptr, 0,
            nullptr,
            nonce,
            shared_key.data());

        result.resize(nonce_size + ct_len);
        return result;
    }
}

// ── Расшифровка ───────────────────────────────────────────────────────
std::optional<std::vector<uint8_t>> CryptoManager::decrypt(
    const KeyBytes& shared_key,
    const std::vector<uint8_t>& encrypted) const {

    if (use_aes_gcm_) {
        size_t nonce_size = crypto_aead_aes256gcm_NPUBBYTES;
        size_t tag_size   = crypto_aead_aes256gcm_ABYTES;

        if (encrypted.size() < nonce_size + tag_size) {
            spdlog::warn("AES-GCM decrypt: слишком короткий ввод");
            return std::nullopt;
        }

        const uint8_t* nonce = encrypted.data();
        const uint8_t* ct    = encrypted.data() + nonce_size;
        size_t ct_len        = encrypted.size() - nonce_size;

        std::vector<uint8_t> plaintext(ct_len - tag_size);
        unsigned long long pt_len = 0;

        if (crypto_aead_aes256gcm_decrypt(
                plaintext.data(), &pt_len,
                nullptr,
                ct, ct_len,
                nullptr, 0,
                nonce,
                shared_key.data()) != 0) {
            spdlog::warn("AES-GCM decrypt: MAC mismatch");
            return std::nullopt;
        }

        plaintext.resize(pt_len);
        return plaintext;
    } else {
        size_t nonce_size = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
        size_t tag_size   = crypto_aead_xchacha20poly1305_ietf_ABYTES;

        if (encrypted.size() < nonce_size + tag_size) {
            spdlog::warn("XChaCha20 decrypt: слишком короткий ввод");
            return std::nullopt;
        }

        const uint8_t* nonce = encrypted.data();
        const uint8_t* ct    = encrypted.data() + nonce_size;
        size_t ct_len        = encrypted.size() - nonce_size;

        std::vector<uint8_t> plaintext(ct_len - tag_size);
        unsigned long long pt_len = 0;

        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                plaintext.data(), &pt_len,
                nullptr,
                ct, ct_len,
                nullptr, 0,
                nonce,
                shared_key.data()) != 0) {
            spdlog::warn("XChaCha20 decrypt: MAC mismatch");
            return std::nullopt;
        }

        plaintext.resize(pt_len);
        return plaintext;
    }
}

// ── Утилиты ───────────────────────────────────────────────────────────

std::string CryptoManager::key_to_hex(const KeyBytes& key) {
    // sodium_bin2hex — потокобезопасная функция
    std::vector<char> hex(KEY_SIZE * 2 + 1);
    sodium_bin2hex(hex.data(), hex.size(), key.data(), KEY_SIZE);
    return std::string(hex.data());
}

std::optional<KeyBytes> CryptoManager::hex_to_key(const std::string& hex) {
    if (hex.size() != KEY_SIZE * 2) return std::nullopt;

    KeyBytes key;
    size_t bin_len = 0;
    if (sodium_hex2bin(key.data(), KEY_SIZE,
                       hex.c_str(), hex.size(),
                       nullptr, &bin_len, nullptr) != 0) {
        return std::nullopt;
    }
    if (bin_len != KEY_SIZE) return std::nullopt;

    return key;
}

std::string CryptoManager::fingerprint(const KeyBytes& public_key) {
    // SHA-256 через libsodium (crypto_hash_sha256), берём первые 8 байт
    uint8_t hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, public_key.data(), KEY_SIZE);

    std::vector<char> hex(8 * 2 + 1);
    sodium_bin2hex(hex.data(), hex.size(), hash, 8);

    // Форматируем как XX:XX:XX:XX:XX:XX:XX:XX
    std::string result;
    for (int i = 0; i < 16; i += 2) {
        if (!result.empty()) result += ':';
        result += hex[i];
        result += hex[i + 1];
    }

    return result;
}

bool CryptoManager::has_aes_gcm_support() {
    // libsodium проверяет AES-NI / ARM NEON при вызове
    return crypto_aead_aes256gcm_is_available() != 0;
}

} // namespace lm::crypto
