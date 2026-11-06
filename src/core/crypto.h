// vsemvamdam-switcher — small self-contained crypto (portable, unit-tested):
// SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1, AES-256-CBC (PKCS#7), base64, RSA PKCS#1 v1.5 encryption.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vs::crypto {

using Bytes = std::vector<uint8_t>;

Bytes to_bytes(std::string_view s);
std::string to_string(const Bytes& b);

Bytes sha1(const uint8_t* data, size_t n);
inline Bytes sha1(const Bytes& b) { return sha1(b.data(), b.size()); }
Bytes hmac_sha1(const Bytes& key, const uint8_t* data, size_t n);
inline Bytes hmac_sha1(const Bytes& key, const Bytes& msg) { return hmac_sha1(key, msg.data(), msg.size()); }
Bytes pbkdf2_sha1(std::string_view password, const Bytes& salt, uint32_t iterations, size_t dk_len);

// AES-256-CBC with PKCS#7 padding. key: 32 bytes, iv: 16 bytes.
Bytes aes256_cbc_encrypt(const Bytes& key, const Bytes& iv, const Bytes& plain);
bool aes256_cbc_decrypt(const Bytes& key, const Bytes& iv, const Bytes& cipher, Bytes& plain);  // false on bad padding / size
// Raw single-block AES-256 (for known-answer tests)
void aes256_encrypt_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);
void aes256_decrypt_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);

std::string base64_encode(const uint8_t* data, size_t n);
inline std::string base64_encode(const Bytes& b) { return base64_encode(b.data(), b.size()); }
// Accepts standard and URL-safe alphabets, ignores whitespace, padding optional. False on garbage.
bool base64_decode(std::string_view s, Bytes& out);
std::string hex_encode(const Bytes& b);
bool hex_decode(std::string_view s, Bytes& out);

// Cryptographically secure random bytes (RtlGenRandom on Windows, /dev/urandom elsewhere).
Bytes random_bytes(size_t n);

// Modular exponentiation on big-endian byte strings: base^exp mod mod (mod must be odd). Result has mod's byte length.
Bytes modexp(const Bytes& base, const Bytes& exp, const Bytes& mod);
// RSA PKCS#1 v1.5 (type 2) encryption with a public key given as hex strings (as Steam returns them).
// `rnd` supplies padding bytes (tests); empty => random. Returns empty on error.
Bytes rsa_pkcs1_encrypt(const std::string& modulus_hex, const std::string& exponent_hex, const Bytes& message, const Bytes& rnd = {});

// Overwrites the buffer before it is freed.
void wipe(std::string& s);
void wipe(Bytes& b);

}  // namespace vs::crypto
