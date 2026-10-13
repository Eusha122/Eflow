#include "core/PackageIdentity.h"

#include <appmodel.h>
#include <shobjidl.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include <filesystem>
#include <string>

#include "core/Log.h"

namespace eflow {
namespace {

// Must match package\AppxManifest.xml.
constexpr wchar_t kPackageName[] = L"Eflow.Island";
constexpr wchar_t kPackagePublisher[] = L"CN=Eflow";
constexpr wchar_t kApplicationId[] = L"Eflow";

std::filesystem::path ExeDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) { path.resize(n); break; }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec);
}

} // namespace

bool HasPackageIdentity() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}

bool RelaunchWithPackageIdentity(bool alreadyRelaunched) {
    if (alreadyRelaunched || HasPackageIdentity()) return false;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        struct Uninit { ~Uninit() { winrt::uninit_apartment(); } } uninit;   // App::Run initialises COM itself
        winrt::Windows::Management::Deployment::PackageManager manager;
        // Only a package registered for *this* folder counts: a debug exe must not launch the release build.
        std::wstring family;
        for (const auto& package : manager.FindPackagesForUser(L"", kPackageName, kPackagePublisher)) {
            const auto external = package.EffectiveExternalLocation();
            if (!external) continue;
            if (SamePath(std::filesystem::path(std::wstring(external.Path())), ExeDirectory())) {
                family = std::wstring(package.Id().FamilyName());
                break;
            }
        }
        if (family.empty()) return false;

        IApplicationActivationManager* activation = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(&activation))))
            return false;
        const std::wstring aumid = family + L"!" + kApplicationId;
        DWORD pid = 0;
        const HRESULT hr = activation->ActivateApplication(aumid.c_str(), L"--packaged", AO_NONE, &pid);
        activation->Release();
        if (FAILED(hr)) {
            Log("package: activation failed (0x%08X); running without identity", static_cast<unsigned>(hr));
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& e) {
        Log("package: lookup failed (0x%08X); running without identity", static_cast<unsigned>(e.code()));
        return false;
    }
}

} // namespace eflow
