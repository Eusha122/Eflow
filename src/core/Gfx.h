#pragma once
#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>

namespace eflow {

template <class T> using Com = Microsoft::WRL::ComPtr<T>;

// Process-wide graphics factories and devices shared by every island window.
class Gfx {
public:
    bool Init();

    ID3D11Device* d3d() const { return d3d_.Get(); }
    IDXGIDevice* dxgi() const { return dxgi_.Get(); }
    ID2D1Factory1* d2dFactory() const { return d2dFactory_.Get(); }
    ID2D1Device* d2dDevice() const { return d2dDevice_.Get(); }
    IDCompositionDevice* dcomp() const { return dcomp_.Get(); }
    IDXGIFactory2* dxgiFactory() const { return dxgiFactory_.Get(); }

    // Cached text formats: Segoe UI Variable (Display for >= 16 DIP), or the icon font.
    IDWriteTextFormat* Text(float size, DWRITE_FONT_WEIGHT weight, bool icons = false);
    float MeasureWidth(const wchar_t* text, IDWriteTextFormat* fmt);

    static constexpr const wchar_t* kUiFont = L"Segoe UI Variable Text";
    static constexpr const wchar_t* kDisplayFont = L"Segoe UI Variable Display";
    static constexpr const wchar_t* kIconFont = L"Segoe Fluent Icons";

private:
    Com<ID3D11Device> d3d_;
    Com<IDXGIDevice> dxgi_;
    Com<IDXGIFactory2> dxgiFactory_;
    Com<ID2D1Factory1> d2dFactory_;
    Com<ID2D1Device> d2dDevice_;
    Com<IDCompositionDevice> dcomp_;
    Com<IDWriteFactory> dwrite_;
    std::unordered_map<unsigned, Com<IDWriteTextFormat>> formats_;
};

} // namespace eflow
