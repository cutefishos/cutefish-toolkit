// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/raster_image.h"

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"

namespace cutefish::ui {

RasterImage::RasterImage(int width, int height, std::vector<uint32_t> pixels, bool premultiplied)
{
    if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height))
        return;
    auto* owned = new std::vector<uint32_t>(std::move(pixels));
    sk_sp<SkData> data = SkData::MakeWithProc(
            owned->data(), owned->size() * 4,
            [](const void*, void* context) { delete static_cast<std::vector<uint32_t>*>(context); }, owned);
    // 0xAARRGGBB words are BGRA bytes on a little-endian machine.
    const SkImageInfo info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                               premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
    sk_sp<SkImage> image = SkImages::RasterFromData(info, std::move(data), size_t(width) * 4);
    if (image)
        m_image = std::shared_ptr<const SkImage>(image.release(), [](const SkImage* image) { image->unref(); });
}

int RasterImage::width() const
{
    return m_image ? m_image->width() : 0;
}

int RasterImage::height() const
{
    return m_image ? m_image->height() : 0;
}

const SkImage* RasterImage::skImage() const
{
    return m_image.get();
}

} // namespace cutefish::ui
