// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/rounded_shape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRegion.h"
#include "include/effects/SkRuntimeEffect.h"

#include "ui/embedded_files.h"

namespace cutefish::ui {

namespace {

struct Vec
{
    float x;
    float y;
};

Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y}; }
Vec operator*(Vec a, Vec b) { return {a.x * b.x, a.y * b.y}; }
Vec operator/(Vec a, Vec b) { return {a.x / b.x, a.y / b.y}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s}; }

Vec absolute(Vec a) { return {std::abs(a.x), std::abs(a.y)}; }
float length(Vec a) { return std::sqrt(a.x * a.x + a.y * a.y); }
Vec normalize(Vec a) { return a * (1.0f / length(a)); }
float minimum(Vec a) { return std::min(a.x, a.y); }

// roundedrectangle.frag, function for function, in device pixels.
class Coverage
{
public:
    Coverage(Vec shapeSize, float radius, bool continuous)
        : m_half(shapeSize * 0.5f)
        , m_continuous(continuous)
    {
        if (continuous) {
            m_r = {std::clamp(radius * 1.5f, 0.0f, m_half.x), std::clamp(radius * 1.5f, 0.0f, m_half.y)};
        } else {
            const float r = std::clamp(radius, 0.0f, std::min(m_half.x, m_half.y));
            m_r = {r, r};
        }
    }

    float at(Vec p, float inset) const
    {
        const float distance = roundedDistance(p) + inset;
        if (distance < -1)
            return 1;
        if (distance > 1)
            return 0;
        const Vec corner = absolute(p - m_half) - m_half + m_r;
        if (minimum(m_r) <= 0 || minimum(corner) < -0.5f)
            return tangentCoverage(p, inset, 1);
        return 0.25f * (tangentCoverage(p + Vec{-0.25f, -0.25f}, inset, 0.5f)
                        + tangentCoverage(p + Vec{0.25f, -0.25f}, inset, 0.5f)
                        + tangentCoverage(p + Vec{-0.25f, 0.25f}, inset, 0.5f)
                        + tangentCoverage(p + Vec{0.25f, 0.25f}, inset, 0.5f));
    }

private:
    Vec cornerNormal(Vec corner) const
    {
        if (!m_continuous)
            return normalize(corner);
        const Vec q = corner / m_r;
        return normalize(q * q / m_r);
    }

    float roundedDistance(Vec p) const
    {
        const Vec edge = absolute(p - m_half) - m_half;
        const Vec d = edge + m_r;
        if (minimum(m_r) <= 0 || minimum(d) <= 0)
            return std::max(edge.x, edge.y);
        if (!m_continuous)
            return length(d) - m_r.x;
        const Vec q = d / m_r;
        const Vec q2 = q * q;
        const float l = std::pow(q2.x * q.x + q2.y * q.y, 1.0f / 3.0f);
        return (l - 1) * l * l / length(q2 / m_r);
    }

    float tangentCoverage(Vec p, float inset, float pixelSize) const
    {
        const Vec edge = absolute(p - m_half) - m_half;
        const Vec corner = edge + m_r;
        Vec normal = edge.x > edge.y ? Vec{1, 0} : Vec{0, 1};
        if (minimum(m_r) > 0 && minimum(corner) > 0)
            normal = cornerNormal(corner);
        const float t = -roundedDistance(p) - inset;
        const Vec extent = absolute(normal) * (0.5f * pixelSize);
        const float a = std::max(extent.x, extent.y);
        const float b = std::min(extent.x, extent.y);
        if (b < 0.001f)
            return std::clamp(0.5f + t / (2 * a), 0.0f, 1.0f);
        const float u = std::abs(t);
        if (u >= a + b)
            return t > 0 ? 1 : 0;
        if (u <= a - b)
            return 0.5f + t / (2 * a);
        const float tail = (a + b - u) * (a + b - u) / (8 * a * b);
        return t > 0 ? 1 - tail : tail;
    }

    Vec m_half;
    Vec m_r;
    bool m_continuous;
};

uint8_t channel(float value)
{
    return uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255));
}

// The coverage of a block of the quad, inside the outline and inside the
// border, as the shader computes it; colours apply per draw.
void cover(const RoundedShapeSpec& spec, const Coverage& coverage, SkIRect block, float* outer, float* inner,
           size_t stride)
{
    for (int y = block.top(); y < block.bottom(); ++y) {
        for (int x = block.left(); x < block.right(); ++x) {
            const Vec p{x + 0.5f + spec.inset, y + 0.5f + spec.inset};
            const size_t index = size_t(y - block.top()) * stride + size_t(x - block.left());
            outer[index] = coverage.at(p, spec.inset);
            inner[index] = spec.borderWidth > 0 ? coverage.at(p, spec.inset + spec.borderWidth) : outer[index];
        }
    }
}

// The coverage of a shape's corners, or of all of a small one, by geometry
// alone: colours and opacity change every frame of a fade.
struct CoverageEntry
{
    int width;
    int height;
    float radius;
    float inset;
    float borderWidth;
    bool continuous;
    std::vector<float> outer;
    std::vector<float> inner;
};

} // namespace

// The pixels next to a corner that the corner's curve or its antialiasing
// reaches; beyond them every edge is straight and crisp.
static int cornerBlock(const RoundedShapeSpec& spec)
{
    const float extent = spec.continuous ? spec.radius * 1.5f : spec.radius;
    return std::max({int(std::ceil(extent - spec.inset)) + 2, int(spec.borderWidth), 1});
}

// The rounded-rectangle coverage again, as a shader for GPU canvases, which
// run it on the corner blocks at no CPU cost. Lengths in device pixels.
static sk_sp<SkShader> makeShader(const RoundedShapeSpec& spec)
{
    static const sk_sp<SkRuntimeEffect> effect = [] {
        SkRuntimeEffect::Result result = SkRuntimeEffect::MakeForShader(SkString(embeddedText("rounded_shape.sksl").c_str()));
        if (!result.effect)
            fprintf(stderr, "cutefish-toolkit: %s\n", result.errorText.c_str());
        return result.effect;
    }();
    if (!effect)
        return nullptr;
    SkRuntimeShaderBuilder builder(effect);
    builder.uniform("quadOrigin") = SkV2{SkScalar(spec.quad.left()), SkScalar(spec.quad.top())};
    builder.uniform("size") = SkV2{SkScalar(spec.quad.width()), SkScalar(spec.quad.height())};
    builder.uniform("radius") = spec.radius;
    builder.uniform("shapeInset") = spec.inset;
    builder.uniform("borderWidth") = spec.borderWidth;
    builder.uniform("continuous") = spec.continuous ? 1.0f : 0.0f;
    builder.uniform("fillColor") = spec.fill;
    builder.uniform("borderColor") = spec.border;
    return builder.makeShader();
}

void drawRoundedShape(SkCanvas* canvas, const RoundedShapeSpec& spec)
{
    const SkIRect& quad = spec.quad;
    if (quad.isEmpty())
        return;
    const int block = cornerBlock(spec);
    const bool whole = 2 * block >= quad.width() || 2 * block >= quad.height();
    // The corner blocks' places in the quad, and in the cached coverage.
    const SkIRect corners[4] = {
        SkIRect::MakeXYWH(0, 0, block, block),
        SkIRect::MakeXYWH(quad.width() - block, 0, block, block),
        SkIRect::MakeXYWH(0, quad.height() - block, block, block),
        SkIRect::MakeXYWH(quad.width() - block, quad.height() - block, block, block),
    };

    if (canvas->recordingContext()) {
        SkPaint paint;
        paint.setShader(makeShader(spec));
        if (whole) {
            canvas->drawIRect(quad, paint);
            return;
        }
        // The four corners in one draw, which sets the shader up once.
        SkIRect placed[4];
        for (int i = 0; i < 4; ++i)
            placed[i] = corners[i].makeOffset(quad.left(), quad.top());
        SkRegion region;
        region.setRects(placed, 4);
        canvas->drawRegion(region, paint);
    } else {
        // Small shapes cached whole; large ones by their corners, whatever
        // their size, since their curves never meet.
        const int width = whole ? quad.width() : 2 * block;
        const int height = whole ? quad.height() : 2 * block;
        const int keyWidth = whole ? quad.width() : -1;
        const int keyHeight = whole ? quad.height() : -1;
        static std::vector<CoverageEntry> cache;
        auto cached = std::find_if(cache.begin(), cache.end(), [&](const CoverageEntry& entry) {
            return entry.width == keyWidth && entry.height == keyHeight && entry.radius == spec.radius
                    && entry.inset == spec.inset && entry.borderWidth == spec.borderWidth
                    && entry.continuous == spec.continuous;
        });
        if (cached == cache.end()) {
            CoverageEntry entry{keyWidth, keyHeight, spec.radius, spec.inset, spec.borderWidth, spec.continuous};
            entry.outer.resize(size_t(width) * size_t(height));
            entry.inner.resize(entry.outer.size());
            const Vec shapeSize{std::max(quad.width() + 2 * spec.inset, 0.0f),
                                std::max(quad.height() + 2 * spec.inset, 0.0f)};
            const Coverage coverage(shapeSize, spec.radius, spec.continuous);
            const size_t stride = size_t(width);
            if (whole) {
                cover(spec, coverage, SkIRect::MakeWH(width, height), entry.outer.data(), entry.inner.data(), stride);
            } else {
                const size_t offsets[4] = {0, size_t(block), size_t(block) * stride, size_t(block) * stride + size_t(block)};
                for (int i = 0; i < 4; ++i) {
                    cover(spec, coverage, corners[i], entry.outer.data() + offsets[i], entry.inner.data() + offsets[i],
                          stride);
                }
            }
            constexpr size_t kCacheSize = 64;
            if (cache.size() == kCacheSize)
                cache.pop_back();
            cache.insert(cache.begin(), std::move(entry));
            cached = cache.begin();
        } else {
            std::rotate(cache.begin(), cached, cached + 1);
            cached = cache.begin();
        }

        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
        auto* pixels = static_cast<uint8_t*>(bitmap.getPixels());
        const size_t rowBytes = bitmap.rowBytes();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t index = size_t(y) * size_t(width) + size_t(x);
                const float inner = cached->inner[index];
                const float rim = cached->outer[index] - inner;
                uint8_t* pixel = pixels + size_t(y) * rowBytes + size_t(x) * 4;
                pixel[0] = channel(spec.fill.fR * inner + spec.border.fR * rim);
                pixel[1] = channel(spec.fill.fG * inner + spec.border.fG * rim);
                pixel[2] = channel(spec.fill.fB * inner + spec.border.fB * rim);
                pixel[3] = channel(spec.fill.fA * inner + spec.border.fA * rim);
            }
        }
        bitmap.setImmutable();
        const sk_sp<SkImage> image = bitmap.asImage();
        if (whole) {
            canvas->drawImage(image, SkScalar(quad.left()), SkScalar(quad.top()));
            return;
        }
        const SkIPoint sources[4] = {{0, 0}, {block, 0}, {0, block}, {block, block}};
        for (int i = 0; i < 4; ++i) {
            canvas->drawImageRect(
                image, SkRect::Make(SkIRect::MakeXYWH(sources[i].x(), sources[i].y(), block, block)),
                SkRect::Make(corners[i].makeOffset(quad.left(), quad.top())), SkSamplingOptions(), nullptr,
                SkCanvas::kStrict_SrcRectConstraint);
        }
    }

    const int border = int(spec.borderWidth);
    const auto fillRect = [canvas](int left, int top, int right, int bottom, const SkColor4f& color) {
        if (color.fA <= 0 || right <= left || bottom <= top)
            return;
        // Back to the 8-bit straight colour it was given as, so flat areas blend
        // as every other fill does.
        const auto byte = [](float value) { return U8CPU(std::lround(std::clamp(value, 0.0f, 1.0f) * 255)); };
        SkPaint paint;
        paint.setColor(SkColorSetARGB(byte(color.fA), byte(color.fR / color.fA), byte(color.fG / color.fA),
                                      byte(color.fB / color.fA)));
        canvas->drawIRect(SkIRect::MakeLTRB(left, top, right, bottom), paint);
    };
    const int innerLeft = quad.left() + block;
    const int innerRight = quad.right() - block;
    const int innerTop = quad.top() + block;
    const int innerBottom = quad.bottom() - block;
    fillRect(innerLeft, quad.top(), innerRight, quad.top() + border, spec.border);
    fillRect(innerLeft, quad.top() + border, innerRight, innerTop, spec.fill);
    fillRect(innerLeft, innerBottom, innerRight, quad.bottom() - border, spec.fill);
    fillRect(innerLeft, quad.bottom() - border, innerRight, quad.bottom(), spec.border);
    fillRect(quad.left(), innerTop, quad.left() + border, innerBottom, spec.border);
    fillRect(quad.left() + border, innerTop, quad.right() - border, innerBottom, spec.fill);
    fillRect(quad.right() - border, innerTop, quad.right(), innerBottom, spec.border);
}

} // namespace cutefish::ui
