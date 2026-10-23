#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace eflow {

struct ImageBGRA {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;                // premultiplied BGRA8, top-down, stride = width * 4
    bool empty() const { return pixels.empty(); }
};

// Decodes PNG/JPEG/etc. with WIC into premultiplied BGRA, resampled so that the result is `scale`
// times the source size, then shrunk further if needed so neither side exceeds `maxSide` (0 = no limit).
// COM must be initialised on the calling thread.
bool DecodeImage(const uint8_t* bytes, size_t size, float scale, uint32_t maxSide, ImageBGRA& out);

// Writes `image` (premultiplied BGRA) to `path` as a PNG (straight alpha) with WIC. COM must be initialised on the calling thread.
bool SavePng(const ImageBGRA& image, const wchar_t* path);

} // namespace eflow
