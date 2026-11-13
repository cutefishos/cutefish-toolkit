// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/image.h"

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"

#include "ui/image_decoder.h"

namespace cutefish::ui {

Image::Image(int width, int height)
    : m_width(width)
    , m_height(height)
    , m_pixels(size_t(width) * size_t(height))
{
}

// 0xAARRGGBB words are BGRA bytes on little-endian machines.
static SkImageInfo pixelInfo(int width, int height)
{
    return SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
}

Image Image::fromFile(const std::string& path)
{
    ImageRequest request;
    request.path = path;
    request.autoTransform = true;
    const DecodedImage decoded = decodeImage(request);
    if (!decoded.image)
        return {};
    Image image(decoded.image->width(), decoded.image->height());
    const SkImageInfo info = pixelInfo(image.m_width, image.m_height);
    if (!decoded.image->readPixels(info, image.m_pixels.data(), size_t(image.m_width) * 4, 0, 0))
        return {};
    return image;
}

bool Image::savePng(const std::string& path) const
{
    if (isNull())
        return false;
    const SkPixmap pixmap(pixelInfo(m_width, m_height), m_pixels.data(), size_t(m_width) * 4);
    SkFILEWStream stream(path.c_str());
    return stream.isValid() && SkPngEncoder::Encode(&stream, pixmap, {});
}

std::string Image::encodePng() const
{
    if (isNull())
        return {};
    const SkPixmap pixmap(pixelInfo(m_width, m_height), m_pixels.data(), size_t(m_width) * 4);
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, pixmap, {}))
        return {};
    const sk_sp<SkData> data = stream.detachAsData();
    return std::string(static_cast<const char*>(data->data()), data->size());
}

} // namespace cutefish::ui
