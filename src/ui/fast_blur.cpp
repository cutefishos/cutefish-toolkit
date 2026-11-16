// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/fast_blur.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"

#include "ui/bitmap.h"
#include "ui/fast_blur_effect.h"
#include "ui/multi_effect_blur.h"
#include "ui/painter.h"

namespace cutefish::ui {

// calculateWeights: each level's share by how near it lies to the level of
// detail the radius asks for.
std::array<double, 6> fastBlurWeights(double radius)
{
    const double lod = std::sqrt(radius / 64.0) * 1.2 - 0.2;
    std::array<double, 6> weights;
    double sum = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double distance = std::fabs(lod - (0.1 + 0.2 * double(i)));
        weights[i] = distance >= 0.5 ? 0.0 : 1.0 - distance * 2.0;
        sum += weights[i];
    }
    for (double& weight : weights)
        weight /= sum;
    return weights;
}

// One level: the item of the logical size samples the level before it four
// times, a logical pixel away along one axis and 0.36 of one along the other,
// as fastblur_internal.vert places the taps. The texture holds eight bits.
static Texture fastBlurPass(const Texture& source, double logicalWidth, double logicalHeight, double scale)
{
    Texture result;
    result.width = textureLength(logicalWidth, scale);
    result.height = textureLength(logicalHeight, scale);
    result.pixels.assign(size_t(result.width) * size_t(result.height) * 4, 0.0f);
    const double ox = 1 / logicalWidth;
    const double oy = 1 / logicalHeight;
    const double offsets[4][2] = {
        {ox, oy * 0.36},
        {ox * 0.36, -oy},
        {-ox * 0.36, oy},
        {-ox, -oy * 0.36},
    };
    std::vector<Taps> columns;
    std::vector<Taps> rows;
    for (const auto& offset : offsets) {
        columns.emplace_back(result.width, source.width, offset[0]);
        rows.emplace_back(result.height, source.height, offset[1]);
    }
    for (int y = 0; y < result.height; ++y) {
        float* out = &result.pixels[size_t(y) * size_t(result.width) * 4];
        for (int x = 0; x < result.width; ++x, out += 4) {
            for (size_t k = 0; k < 4; ++k) {
                source.blend(columns[k].first[size_t(x)], columns[k].second[size_t(x)], columns[k].fraction[size_t(x)],
                             rows[k].first[size_t(y)], rows[k].second[size_t(y)], rows[k].fraction[size_t(y)], 0.25f,
                             out);
            }
            for (int c = 0; c < 4; ++c)
                out[c] = std::round(std::clamp(out[c], 0.0f, 1.0f) * 255.0f) / 255.0f;
        }
    }
    return result;
}

Bitmap fastBlur(const Bitmap& sourceBitmap, double logicalWidth, double logicalHeight, double radius, double scale)
{
    Texture source;
    source.width = sourceBitmap.width();
    source.height = sourceBitmap.height();
    source.pixels.resize(size_t(source.width) * size_t(source.height) * 4);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const uint32_t pixel = sourceBitmap.pixel(x, y);
            float* out = &source.pixels[(size_t(y) * size_t(source.width) + size_t(x)) * 4];
            out[0] = float((pixel >> 16) & 0xff) / 255.0f;
            out[1] = float((pixel >> 8) & 0xff) / 255.0f;
            out[2] = float(pixel & 0xff) / 255.0f;
            out[3] = float(pixel >> 24) / 255.0f;
        }
    }

    const std::array<double, 6> weights = fastBlurWeights(radius);
    int levels = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0)
            levels = int(i);
    }
    // The first level item is the effect's size rounded up to multiples of 32;
    // its texture is the source's own, so the passes start at the second.
    double itemWidth = std::ceil(logicalWidth / 32 - 1e-9) * 32;
    double itemHeight = std::ceil(logicalHeight / 32 - 1e-9) * 32;
    std::vector<Texture> blurs;
    for (int i = 0; i < levels; ++i) {
        itemWidth /= 2;
        itemHeight /= 2;
        blurs.push_back(fastBlurPass(i == 0 ? source : blurs.back(), itemWidth, itemHeight, scale));
    }
    const SkBitmap blurred = composite(source, blurs, weights, 0, source.height);
    Bitmap result(blurred.width(), blurred.height(), Bitmap::Format::Rgba8888Premultiplied);
    for (int y = 0; y < blurred.height(); ++y)
        std::memcpy(result.scanLine(y), blurred.getAddr(0, y), size_t(blurred.width()) * 4);
    return result.converted(Bitmap::Format::Argb32Premultiplied);
}

struct FastBlurredCopy::Cache
{
    sk_sp<SkImage> image;
    double scale = 0;
    SizeF size;
    PointF phase;
    bool stale = true;
};

FastBlurredCopy::FastBlurredCopy(double radius)
    : m_radius(radius)
    , m_cache(std::make_unique<Cache>())
{
    m_holder = addChild<View>();
    m_holder->setClipsChildren(true);
}

FastBlurredCopy::~FastBlurredCopy() = default;

void FastBlurredCopy::setContentView(std::unique_ptr<View> content)
{
    if (m_content)
        m_holder->takeChild(m_content);
    m_content = content.get();
    if (content)
        m_holder->addChild(std::move(content));
    contentChanged();
    requestLayout();
}

void FastBlurredCopy::contentChanged()
{
    m_cache->stale = true;
    requestPaint();
}

void FastBlurredCopy::setRadius(double radius)
{
    if (m_radius == radius)
        return;
    m_radius = radius;
    contentChanged();
}

void FastBlurredCopy::layout()
{
    m_holder->setGeometry({});
    if (m_content)
        m_content->setGeometry(localRect());
    m_cache->stale = true;
}

void FastBlurredCopy::paint(Painter& painter)
{
    if (!m_content)
        return;
    const double scale = painter.scale();
    const SizeF size = geometry().size();
    if (size.width <= 0 || size.height <= 0)
        return;
    // The image lands on whole device pixels; the content is drawn into it
    // at the remainder, so its text rounds onto the rows it would in place.
    const PointF origin = painter.offset();
    const double left = std::round(origin.x * scale);
    const double top = std::round(origin.y * scale);
    const PointF phase {origin.x - left / scale, origin.y - top / scale};
    Cache& cache = *m_cache;
    if (cache.stale || !cache.image || scale != cache.scale || !(size == cache.size) || !(phase == cache.phase)) {
        cache.stale = false;
        cache.scale = scale;
        cache.size = size;
        cache.phase = phase;
        cache.image.reset();
        // The texture is the item's: whole logical pixels, then the scale.
        const Bitmap grabbed = m_content->grab(scale, phase);
        Bitmap texture(textureLength(size.width, scale), textureLength(size.height, scale),
                       Bitmap::Format::Argb32Premultiplied);
        for (int y = 0; y < std::min(texture.height(), grabbed.height()); ++y) {
            for (int x = 0; x < std::min(texture.width(), grabbed.width()); ++x)
                texture.setPixel(x, y, grabbed.pixel(x, y));
        }
        const Bitmap blurred = fastBlur(texture, size.width, size.height, m_radius, scale);
        const SkImageInfo info = SkImageInfo::MakeN32Premul(blurred.width(), blurred.height());
        cache.image = SkImages::RasterFromPixmapCopy(SkPixmap(info, blurred.constBits(), blurred.stride()));
    }
    if (!cache.image)
        return;
    SkPaint paint;
    paint.setAlphaf(float(painter.opacity()));
    painter.canvas()->drawImage(cache.image, SkScalar(left), SkScalar(top), SkSamplingOptions(), &paint);
}

} // namespace cutefish::ui
