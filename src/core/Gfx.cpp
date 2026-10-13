#include "core/Gfx.h"

namespace eflow {

bool Gfx::Init() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &d3d_, nullptr, &d3dContext_);
    if (FAILED(hr)) // GPU-less sessions (RDP, VMs)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &d3d_, nullptr, &d3dContext_);
    if (FAILED(hr)) return false;

    if (FAILED(d3d_.As(&dxgi_))) return false;
    Com<IDXGIAdapter> adapter;
    if (FAILED(dxgi_->GetAdapter(&adapter))) return false;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_)))) return false;

    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))))
        return false;
    if (FAILED(d2dFactory_->CreateDevice(dxgi_.Get(), &d2dDevice_))) return false;
    if (FAILED(DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    return true;
}

IDWriteTextFormat* Gfx::Text(float size, DWRITE_FONT_WEIGHT weight, bool icons) {
    const unsigned key = (static_cast<unsigned>(size * 10) << 12) | (static_cast<unsigned>(weight) << 1) | (icons ? 1u : 0u);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();

    Com<IDWriteTextFormat> fmt;
    const wchar_t* family = icons ? kIconFont : (size >= 16.0f ? kDisplayFont : kUiFont);
    if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt)))
        if (FAILED(dwrite_->CreateTextFormat(icons ? L"Segoe MDL2 Assets" : L"Segoe UI", nullptr, weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt)))
            return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    Com<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis))) fmt->SetTrimming(&trim, ellipsis.Get());
    return formats_.emplace(key, std::move(fmt)).first->second.Get();
}

float Gfx::MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt) {
    Com<IDWriteTextLayout> layout;
    if (!fmt || FAILED(dwrite_->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), fmt, 10000.0f, 1000.0f, &layout)))
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

} // namespace eflow
