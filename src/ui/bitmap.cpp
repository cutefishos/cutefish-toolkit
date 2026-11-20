// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/bitmap.h"

#include <cstring>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "ui/bitmap_private.h"
#include "ui/image_decoder.h"

namespace cutefish::ui {

// As the decoders' limit: no bitmap past 256 MiB.
static constexpr size_t kMaxBytes = size_t(256) << 20;

SkColorType skColorType(Bitmap::Format format)
{
    switch (format) {
    case Bitmap::Format::Argb32Premultiplied:
        return kBGRA_8888_SkColorType;
    case Bitmap::Format::Rgba8888Premultiplied:
        return kRGBA_8888_SkColorType;
    case Bitmap::Format::Alpha8:
        return kAlpha_8_SkColorType;
    }
    return kBGRA_8888_SkColorType;
}

SkPixmap skPixmap(const Bitmap& bitmap)
{
    const SkImageInfo info = SkImageInfo::Make(bitmap.width(), bitmap.height(), skColorType(bitmap.format()),
                                               kPremul_SkAlphaType);
    return SkPixmap(info, bitmap.constBits(), bitmap.stride());
}

void Bitmap::allocate(int width, int height, Format format)
{
    m_pixels.reset();
    m_width = m_height = 0;
    m_stride = 0;
    m_format = format;
    if (width <= 0 || height <= 0)
        return;
    const size_t stride = (size_t(width) * size_t(bytesPerPixel(format)) + 3) & ~size_t(3);
    if (stride * size_t(height) > kMaxBytes || stride / size_t(bytesPerPixel(format)) < size_t(width))
        return;
    m_pixels = std::shared_ptr<uint8_t>(new uint8_t[stride * size_t(height)](), std::default_delete<uint8_t[]>());
    m_width = width;
    m_height = height;
    m_stride = stride;
}

Bitmap::Bitmap(int width, int height, Format format)
{
    allocate(width, height, format);
}

Bitmap Bitmap::wrap(void* pixels, int width, int height, size_t stride, Format format)
{
    Bitmap bitmap;
    bitmap.m_format = format;
    if (!pixels || width <= 0 || height <= 0 || stride < size_t(width) * size_t(bytesPerPixel(format)))
        return bitmap;
    bitmap.m_pixels = std::shared_ptr<uint8_t>(static_cast<uint8_t*>(pixels), [](uint8_t*) {});
    bitmap.m_width = width;
    bitmap.m_height = height;
    bitmap.m_stride = stride;
    return bitmap;
}

Bitmap Bitmap::copyOf(const void* pixels, int width, int height, size_t stride, Format format)
{
    Bitmap bitmap(width, height, format);
    if (bitmap.isNull() || !pixels || stride < size_t(width) * size_t(bytesPerPixel(format)))
        return {};
    const size_t row = size_t(width) * size_t(bytesPerPixel(format));
    for (int y = 0; y < height; ++y)
        std::memcpy(bitmap.scanLine(y), static_cast<const uint8_t*>(pixels) + size_t(y) * stride, row);
    return bitmap;
}

static Bitmap fromSkImage(const SkImage* image)
{
    if (!image)
        return {};
    Bitmap bitmap(image->width(), image->height(), Bitmap::Format::Argb32Premultiplied);
    if (bitmap.isNull())
        return {};
    const SkImageInfo info = SkImageInfo::Make(bitmap.width(), bitmap.height(), kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType);
    if (!image->readPixels(nullptr, info, bitmap.bits(), bitmap.stride(), 0, 0))
        return {};
    return bitmap;
}

static Bitmap decodeRequest(ImageRequest request, base::Size box, Bitmap::Scaling scaling, bool autoTransform)
{
    if (!box.isEmpty()) {
        request.width = box.width();
        request.height = box.height();
        request.enlarge = true;
    }
    request.scaling = scaling == Bitmap::Scaling::Stretch ? ImageScaling::Stretch
            : scaling == Bitmap::Scaling::Cover          ? ImageScaling::Cover
                                                         : ImageScaling::Fit;
    request.autoTransform = autoTransform;
    const DecodedImage decoded = decodeImage(request);
    return fromSkImage(decoded.image.get());
}

Bitmap Bitmap::decodeFile(const std::string& path, base::Size box, Scaling scaling, bool autoTransform)
{
    ImageRequest request;
    request.path = path;
    return decodeRequest(std::move(request), box, scaling, autoTransform);
}

Bitmap Bitmap::decode(std::string_view bytes)
{
    return decode(bytes, {});
}

Bitmap Bitmap::decode(std::string_view bytes, base::Size box, Scaling scaling, bool autoTransform)
{
    if (bytes.empty())
        return {};
    ImageRequest request;
    request.data = bytes;
    return decodeRequest(std::move(request), box, scaling, autoTransform);
}

void Bitmap::detach()
{
    if (!m_pixels || m_pixels.use_count() == 1)
        return;
    const std::shared_ptr<uint8_t> source = m_pixels;
    const size_t bytes = sizeInBytes();
    m_pixels = std::shared_ptr<uint8_t>(new uint8_t[bytes], std::default_delete<uint8_t[]>());
    std::memcpy(m_pixels.get(), source.get(), bytes);
}

uint8_t* Bitmap::bits()
{
    detach();
    return m_pixels.get();
}

uint32_t Bitmap::pixel(int x, int y) const
{
    if (isNull() || x < 0 || y < 0 || x >= m_width || y >= m_height)
        return 0;
    const uint8_t* p = constScanLine(y) + size_t(x) * size_t(bytesPerPixel(m_format));
    switch (m_format) {
    case Format::Argb32Premultiplied: {
        uint32_t value;
        std::memcpy(&value, p, 4);
        return value;
    }
    case Format::Rgba8888Premultiplied:
        return uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
    case Format::Alpha8:
        return uint32_t(p[0]) << 24;
    }
    return 0;
}

void Bitmap::setPixel(int x, int y, uint32_t argb)
{
    if (isNull() || x < 0 || y < 0 || x >= m_width || y >= m_height)
        return;
    uint8_t* p = scanLine(y) + size_t(x) * size_t(bytesPerPixel(m_format));
    switch (m_format) {
    case Format::Argb32Premultiplied:
        std::memcpy(p, &argb, 4);
        break;
    case Format::Rgba8888Premultiplied:
        p[0] = uint8_t(argb >> 16);
        p[1] = uint8_t(argb >> 8);
        p[2] = uint8_t(argb);
        p[3] = uint8_t(argb >> 24);
        break;
    case Format::Alpha8:
        p[0] = uint8_t(argb >> 24);
        break;
    }
}

void Bitmap::fill(uint32_t argb)
{
    if (isNull())
        return;
    if (argb == 0) {
        for (int y = 0; y < m_height; ++y)
            std::memset(scanLine(y), 0, size_t(m_width) * size_t(bytesPerPixel(m_format)));
        return;
    }
    for (int x = 0; x < m_width; ++x)
        setPixel(x, 0, argb);
    const size_t row = size_t(m_width) * size_t(bytesPerPixel(m_format));
    for (int y = 1; y < m_height; ++y)
        std::memcpy(scanLine(y), constScanLine(0), row);
}

Bitmap Bitmap::converted(Format format) const
{
    if (isNull() || format == m_format)
        return *this;
    Bitmap result(m_width, m_height, format);
    if (result.isNull())
        return {};
    if (format == Format::Alpha8 || m_format == Format::Alpha8) {
        for (int y = 0; y < m_height; ++y) {
            for (int x = 0; x < m_width; ++x)
                result.setPixel(x, y, pixel(x, y));
        }
        return result;
    }
    // Swapping red and blue between the two 32-bit layouts.
    for (int y = 0; y < m_height; ++y) {
        const uint8_t* source = constScanLine(y);
        uint8_t* target = result.scanLine(y);
        for (int x = 0; x < m_width; ++x) {
            target[4 * x] = source[4 * x + 2];
            target[4 * x + 1] = source[4 * x + 1];
            target[4 * x + 2] = source[4 * x];
            target[4 * x + 3] = source[4 * x + 3];
        }
    }
    return result;
}

Bitmap Bitmap::scaled(int width, int height, bool smooth) const
{
    if (isNull() || width <= 0 || height <= 0)
        return {};
    if (width == m_width && height == m_height)
        return *this;
    Bitmap result(width, height, m_format);
    if (result.isNull())
        return {};
    const SkPixmap source = skPixmap(*this);
    SkPixmap target(source.info().makeWH(width, height), result.bits(), result.stride());
    SkSamplingOptions sampling;
    if (smooth) {
        const bool shrinking = width < m_width || height < m_height;
        sampling = shrinking ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                             : SkSamplingOptions(SkCubicResampler::Mitchell());
    }
    if (smooth && (width < m_width || height < m_height)) {
        // Mipmaps need an image.
        const sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(source);
        const sk_sp<SkImage> mipmapped = image ? image->withDefaultMipmaps() : nullptr;
        SkBitmap bitmap;
        bitmap.installPixels(target);
        SkCanvas canvas(bitmap);
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        if (mipmapped)
            canvas.drawImageRect(mipmapped, SkRect::MakeWH(SkScalar(width), SkScalar(height)), sampling, &paint);
        return result;
    }
    if (!source.scalePixels(target, sampling))
        return {};
    return result;
}

Bitmap Bitmap::copy(const base::Rect& rect) const
{
    if (isNull() || rect.isEmpty())
        return {};
    Bitmap result(rect.width(), rect.height(), m_format);
    if (result.isNull())
        return {};
    const base::Rect inside = rect.intersected(this->rect());
    const size_t bytes = size_t(bytesPerPixel(m_format));
    for (int y = inside.y(); y < inside.bottom(); ++y) {
        std::memcpy(result.scanLine(y - rect.y()) + size_t(inside.x() - rect.x()) * bytes,
                    constScanLine(y) + size_t(inside.x()) * bytes, size_t(inside.width()) * bytes);
    }
    return result;
}

Bitmap Bitmap::mirrored(bool horizontal, bool vertical) const
{
    if (isNull() || (!horizontal && !vertical))
        return *this;
    Bitmap result(m_width, m_height, m_format);
    if (result.isNull())
        return {};
    const size_t bytes = size_t(bytesPerPixel(m_format));
    for (int y = 0; y < m_height; ++y) {
        const uint8_t* source = constScanLine(vertical ? m_height - 1 - y : y);
        uint8_t* target = result.scanLine(y);
        if (!horizontal) {
            std::memcpy(target, source, size_t(m_width) * bytes);
            continue;
        }
        for (int x = 0; x < m_width; ++x)
            std::memcpy(target + size_t(x) * bytes, source + size_t(m_width - 1 - x) * bytes, bytes);
    }
    return result;
}

std::optional<std::string> Bitmap::encodePng() const
{
    if (isNull())
        return std::nullopt;
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, skPixmap(*this), {}))
        return std::nullopt;
    const sk_sp<SkData> data = stream.detachAsData();
    return std::string(static_cast<const char*>(data->data()), data->size());
}

bool Bitmap::savePng(const std::string& path) const
{
    if (isNull())
        return false;
    SkFILEWStream stream(path.c_str());
    return stream.isValid() && SkPngEncoder::Encode(&stream, skPixmap(*this), {});
}

bool Bitmap::operator==(const Bitmap& other) const
{
    if (m_width != other.m_width || m_height != other.m_height || m_format != other.m_format)
        return false;
    if (isNull())
        return true;
    const size_t row = size_t(m_width) * size_t(bytesPerPixel(m_format));
    for (int y = 0; y < m_height; ++y) {
        if (std::memcmp(constScanLine(y), other.constScanLine(y), row) != 0)
            return false;
    }
    return true;
}

base::Size scaledSize(base::Size size, base::Size box, Bitmap::Scaling scaling)
{
    int width = 0;
    int height = 0;
    scaledSize(size.width(), size.height(), box.width(), box.height(),
               scaling == Bitmap::Scaling::Stretch ? ImageScaling::Stretch
                       : scaling == Bitmap::Scaling::Cover ? ImageScaling::Cover
                                                           : ImageScaling::Fit,
               &width, &height);
    return {width, height};
}

} // namespace cutefish::ui
