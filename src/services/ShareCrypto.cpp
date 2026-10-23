#include "services/ShareCrypto.h"

#include <climits>
#include <cstring>
#include <iterator>

namespace eflow::sharecrypto {
namespace {

bool Ok(NTSTATUS status) { return status >= 0; }

} // namespace

bool RandomBytes(uint8_t* out, size_t size) {
    return size <= ULONG_MAX && Ok(BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::vector<uint8_t> NewKey() {
    std::vector<uint8_t> key(kKeyBytes);
    if (!RandomBytes(key.data(), key.size())) key.clear();
    return key;
}

std::string Base64Url(const std::vector<uint8_t>& bytes) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63], out += kAlphabet[(v >> 12) & 63], out += kAlphabet[(v >> 6) & 63], out += kAlphabet[v & 63];
    }
    if (const size_t rest = bytes.size() - i; rest) {
        const uint32_t v = (bytes[i] << 16) | (rest == 2 ? bytes[i + 1] << 8 : 0);
        out += kAlphabet[(v >> 18) & 63], out += kAlphabet[(v >> 12) & 63];
        if (rest == 2) out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

bool FromBase64Url(const std::string& text, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string s = text;
    while (!s.empty() && s.back() == '=') s.pop_back();
    if (s.size() % 4 == 1) return false;
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string Sha256Hex(const std::string& bytes) {
    uint8_t digest[32];
    if (bytes.size() > ULONG_MAX ||
        !Ok(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                       static_cast<ULONG>(bytes.size()), digest, sizeof(digest))))
        return {};
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest) out += kHex[b >> 4], out += kHex[b & 15];
    return out;
}

uint64_t ChunkCount(uint64_t fileSize) { return fileSize == 0 ? 1 : (fileSize + kChunkBytes - 1) / kChunkBytes; }

uint64_t SealedSize(uint64_t fileSize) { return fileSize + ChunkCount(fileSize) * kSealedOverhead; }

std::wstring MakePassword() {
    static const wchar_t kChars[] = L"abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no 0/O, 1/l/I
    constexpr unsigned kCount = static_cast<unsigned>(std::size(kChars) - 1);
    std::wstring out;
    while (out.size() < 14) {
        if (out.size() == 4 || out.size() == 9) {
            out += L'-';
            continue;
        }
        uint8_t r = 0;
        if (!RandomBytes(&r, 1)) return {};
        if (r >= 256 / kCount * kCount) continue;                         // (no bias towards the first characters)
        out += kChars[r % kCount];
    }
    return out;
}

ChunkCipher::ChunkCipher(const std::vector<uint8_t>& key) {
    if (key.size() != kKeyBytes ||
        !Ok(BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &key_, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                       static_cast<ULONG>(key.size()), 0)))
        key_ = nullptr;
}

ChunkCipher::~ChunkCipher() {
    if (key_) BCryptDestroyKey(key_);
}

bool ChunkCipher::Seal(const uint8_t* plain, size_t size, std::vector<uint8_t>& sealed) {
    if (!key_ || size > ULONG_MAX - kSealedOverhead) return false;
    sealed.resize(size + kSealedOverhead);
    uint8_t* iv = sealed.data();
    uint8_t* body = iv + kIvBytes;
    uint8_t* tag = body + size;
    if (!RandomBytes(iv, kIvBytes)) return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = iv;
    info.cbNonce = kIvBytes;
    info.pbTag = tag;
    info.cbTag = kTagBytes;
    ULONG written = 0;
    return Ok(BCryptEncrypt(key_, const_cast<PUCHAR>(plain), static_cast<ULONG>(size), &info, nullptr, 0, body,
                            static_cast<ULONG>(size), &written, 0)) &&
           written == size;
}

bool ChunkCipher::Open(const uint8_t* sealed, size_t size, std::vector<uint8_t>& plain) {
    if (!key_ || size < kSealedOverhead || size > ULONG_MAX) return false;
    const size_t bodySize = size - kSealedOverhead;
    std::vector<uint8_t> iv(sealed, sealed + kIvBytes), tag(sealed + kIvBytes + bodySize, sealed + size);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = iv.data();
    info.cbNonce = kIvBytes;
    info.pbTag = tag.data();
    info.cbTag = kTagBytes;
    plain.resize(bodySize);
    ULONG written = 0;
    const bool ok = Ok(BCryptDecrypt(key_, const_cast<PUCHAR>(sealed + kIvBytes), static_cast<ULONG>(bodySize), &info, nullptr, 0,
                                     plain.data(), static_cast<ULONG>(bodySize), &written, 0)) &&
                    written == bodySize;
    if (!ok) plain.clear();
    return ok;
}

} // namespace eflow::sharecrypto
