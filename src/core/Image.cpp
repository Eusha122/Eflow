#include "core/Image.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace eflow {

bool DecodeImage(const uint8_t* bytes, size_t size, float scale, uint32_t maxSide, ImageBGRA& out) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size))))
        return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return false;

    // Convert to premultiplied first so the scaler filters colour correctly at transparent edges.
    ComPtr<IWICFormatConverter> premul;
    if (FAILED(wic->CreateFormatConverter(&premul)) ||
        FAILED(premul->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    premul->GetSize(&w, &h);
    if (!w || !h) return false;
    float s = scale;
    if (maxSide) s = std::min(s, static_cast<float>(maxSide) / static_cast<float>(std::max(w, h)));
    const UINT sw = std::max(1u, static_cast<UINT>(std::lround(w * s)));
    const UINT sh = std::max(1u, static_cast<UINT>(std::lround(h * s)));

    ComPtr<IWICBitmapSource> source = premul;
    ComPtr<IWICBitmapScaler> scaler;
    if (sw != w || sh != h) {
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(premul.Get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        source = scaler;
    }
    out.width = sw;
    out.height = sh;
    out.pixels.resize(static_cast<size_t>(sw) * sh * 4);
    return SUCCEEDED(source->CopyPixels(nullptr, sw * 4, static_cast<UINT>(out.pixels.size()), out.pixels.data()));
}

} // namespace eflow
