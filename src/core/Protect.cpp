#include "core/Protect.h"

#include <windows.h>
#include <dpapi.h>

#include <cstring>

namespace eflow {
namespace {

bool Run(bool protect, const std::string& in, const char* purpose, std::string& out) {
    DATA_BLOB input{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(std::strlen(purpose)), reinterpret_cast<BYTE*>(const_cast<char*>(purpose))};
    DATA_BLOB result{};
    const BOOL ok = protect ? CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result)
                            : CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (!ok) return false;
    out.assign(reinterpret_cast<const char*>(result.pbData), result.cbData);
    SecureZeroMemory(result.pbData, result.cbData);
    LocalFree(result.pbData);
    return true;
}

} // namespace

bool ProtectForUser(const std::string& plain, const char* purpose, std::string& sealed) { return Run(true, plain, purpose, sealed); }

bool UnprotectForUser(const std::string& sealed, const char* purpose, std::string& plain) { return Run(false, sealed, purpose, plain); }

} // namespace eflow
