// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/geometry.h"

namespace cutefish::ui {

// Pixels in memory for offscreen work: decoding, scaling, painting and
// encoding, with raw access for uploads.
class Bitmap
{
public:
    enum class Format {
        // 32-bit words 0xAARRGGBB, premultiplied; B, G, R, A bytes in memory.
        Argb32Premultiplied,
        // R, G, B, A bytes, premultiplied.
        Rgba8888Premultiplied,
        // One coverage byte per pixel.
        Alpha8,
    };

    enum class Scaling {
        // The box exactly.
        Stretch,
        // Inside the box, the aspect kept.
        Fit,
        // Covering the box, the aspect kept.
        Cover,
    };

    // Null.
    Bitmap() = default;
    // Transparent; null when the size is empty or too large.
    Bitmap(int width, int height, Format format);
    Bitmap(base::Size size, Format format) : Bitmap(size.width(), size.height(), format) {}

    // Uses the memory itself, without a copy: writes go to it while no copy
    // shares it, and it must outlive the bitmap and its copies. Copies share
    // pixels until one of them is written to.
    static Bitmap wrap(void* pixels, int width, int height, size_t stride, Format format);
    static Bitmap copyOf(const void* pixels, int width, int height, size_t stride, Format format);

    // PNG, JPEG, WebP and the rest the toolkit reads, decoded to
    // Argb32Premultiplied. `box` (device pixels) scales while decoding, as
    // `scaling` says; empty keeps the size. `autoTransform` turns the pixels
    // as the file's EXIF orientation says. Null when the file cannot be read.
    static Bitmap decodeFile(const std::string& path, base::Size box = {}, Scaling scaling = Scaling::Fit,
                             bool autoTransform = false);
    // The same from a file's bytes in memory.
    static Bitmap decode(std::string_view bytes);
    static Bitmap decode(std::string_view bytes, base::Size box, Scaling scaling = Scaling::Fit,
                         bool autoTransform = false);

    bool isNull() const { return !m_pixels; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    base::Size size() const { return {m_width, m_height}; }
    base::Rect rect() const { return base::Rect(size()); }
    Format format() const { return m_format; }
    size_t stride() const { return m_stride; }
    static int bytesPerPixel(Format format) { return format == Format::Alpha8 ? 1 : 4; }
    size_t sizeInBytes() const { return m_stride * size_t(m_height); }

    // Writing detaches shared pixels first.
    uint8_t* bits();
    const uint8_t* constBits() const { return m_pixels.get(); }
    const uint8_t* bits() const { return m_pixels.get(); }
    uint8_t* scanLine(int y) { return bits() + size_t(y) * m_stride; }
    const uint8_t* constScanLine(int y) const { return constBits() + size_t(y) * m_stride; }

    // Premultiplied 0xAARRGGBB in any format (Alpha8 gives black of that
    // coverage); 0 outside.
    uint32_t pixel(int x, int y) const;
    uint8_t alpha(int x, int y) const { return uint8_t(pixel(x, y) >> 24); }
    // Premultiplied 0xAARRGGBB, stored in the bitmap's format.
    void setPixel(int x, int y, uint32_t argb);
    // Every pixel to premultiplied 0xAARRGGBB; 0 is transparent.
    void fill(uint32_t argb);

    Bitmap converted(Format format) const;
    // Smooth: averaged when shrinking, filtered when growing.
    Bitmap scaled(int width, int height, bool smooth = true) const;
    Bitmap scaled(base::Size size, bool smooth = true) const { return scaled(size.width(), size.height(), smooth); }
    // The part inside `rect`; what lies outside the bitmap is transparent.
    Bitmap copy(const base::Rect& rect) const;
    Bitmap mirrored(bool horizontal, bool vertical) const;

    bool savePng(const std::string& path) const;
    std::optional<std::string> encodePng() const;

    // Same size, format and pixels.
    bool operator==(const Bitmap& other) const;

private:
    friend class BitmapPainter;

    void detach();
    void allocate(int width, int height, Format format);

    std::shared_ptr<uint8_t> m_pixels;
    int m_width = 0;
    int m_height = 0;
    size_t m_stride = 0;
    Format m_format = Format::Argb32Premultiplied;
};

// The box scaled by `scaling` from width x height, with integer rounding as
// the common image scaling has it.
base::Size scaledSize(base::Size size, base::Size box, Bitmap::Scaling scaling);

} // namespace cutefish::ui
