#pragma once
#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eflow::sharecrypto {

// SafeShare's file encryption, byte for byte as safeshare.co does it in the browser (src/lib/crypto.ts), so a room sent
// from Eflow opens on the website like any other: format "aes_gcm_chunked_v1", version "v1". Each 25 MB of a file is one
// chunk, sealed with AES-256-GCM under the room key and a fresh random 96-bit IV, stored as [IV | ciphertext | 16-byte tag];
// the chunks follow each other. The key travels only in the link's fragment (#key=...), base64url without padding. Room
// passwords are sent as the SHA-256 of their UTF-8 bytes, in lower-case hex. Windows CNG does the work (no third party).

inline constexpr size_t kKeyBytes = 32, kIvBytes = 12, kTagBytes = 16;
inline constexpr size_t kChunkBytes = 25 * 1024 * 1024;                    // plaintext per chunk
inline constexpr size_t kSealedOverhead = kIvBytes + kTagBytes;
inline constexpr const char* kMode = "aes_gcm_chunked_v1";
inline constexpr const char* kVersion = "v1";

bool RandomBytes(uint8_t* out, size_t size);                                // the system's CSPRNG
std::vector<uint8_t> NewKey();                                              // 32 random bytes (empty on failure)
std::string Base64Url(const std::vector<uint8_t>& bytes);                  // no padding
bool FromBase64Url(const std::string& text, std::vector<uint8_t>& out);
std::string Sha256Hex(const std::string& bytes);                           // lower-case hex
uint64_t ChunkCount(uint64_t fileSize);                                     // chunks a file of this size makes (an empty file: 1)
uint64_t SealedSize(uint64_t fileSize);                                     // its size once encrypted
std::wstring MakePassword();                                                // "k7Qm-3xTa-9pLr": 12 random, easy-to-read characters

// Seals and opens chunks with one room key. One instance per thread (each holds its own CNG key object).
class ChunkCipher {
public:
    explicit ChunkCipher(const std::vector<uint8_t>& key);
    ~ChunkCipher();
    ChunkCipher(const ChunkCipher&) = delete;
    ChunkCipher& operator=(const ChunkCipher&) = delete;

    bool ok() const { return key_ != nullptr; }
    bool Seal(const uint8_t* plain, size_t size, std::vector<uint8_t>& sealed);   // sealed = IV | ciphertext | tag
    bool Open(const uint8_t* sealed, size_t size, std::vector<uint8_t>& plain);   // false: wrong key or tampered

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
};

} // namespace eflow::sharecrypto
