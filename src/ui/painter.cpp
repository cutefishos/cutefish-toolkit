// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/painter.h"
#include "ui/path_private.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <utility>

#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRegion.h"
#include "include/effects/SkGradient.h"

#include "ui/icon_loader.h"
#include "ui/image.h"
#include "ui/smooth_rectangle.h"
#include "ui/rounded_path.h"
#include "ui/rounded_shape.h"
#include "ui/theme.h"

namespace cutefish::ui {

static SkRect toSkRect(const Rect& rect)
{
    return SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height);
}

static SkPaint makePaint(Color color, double opacity, bool antialias)
{
    SkPaint paint;
    paint.setColor(color.withAlpha(color.a / 255.0 * opacity).argb());
    paint.setAntiAlias(antialias);
    return paint;
}

Painter::Painter(SkCanvas* canvas, double scale)
    : m_canvas(canvas)
    , m_scale(scale)
{
}

void Painter::save()
{
    m_saved.push_back({m_offset, m_opacity});
    m_canvas->save();
}

void Painter::restore()
{
    m_offset = m_saved.back().offset;
    m_opacity = m_saved.back().opacity;
    m_saved.pop_back();
    m_canvas->restore();
}

void Painter::clip(const RectF& rect)
{
    m_canvas->clipRect(toSkRect(deviceRect(rect)));
}

// The clip in the painter's device pixels, before the canvas's own matrix
// (a layer's texture offset).
static SkIRect untransformedClip(SkCanvas* canvas)
{
    SkMatrix inverse;
    const SkIRect device = canvas->getDeviceClipBounds();
    if (!canvas->getTotalMatrix().invert(&inverse))
        return device;
    return inverse.mapRect(SkRect::Make(device)).roundOut();
}

RectF Painter::clipBounds() const
{
    const SkIRect device = untransformedClip(m_canvas);
    return {device.left() / m_scale - m_offset.x, device.top() / m_scale - m_offset.y, device.width() / m_scale,
            device.height() / m_scale};
}

std::vector<RectF> Painter::clipRects() const
{
    if (!m_damage)
        return {clipBounds()};
    SkRegion parts(*m_damage);
    parts.op(untransformedClip(m_canvas), SkRegion::kIntersect_Op);
    // A region keeps its rects in horizontal bands, cutting a column at every
    // band another part starts or ends; the columns are joined again.
    std::vector<SkIRect> columns;
    for (SkRegion::Iterator it(parts); !it.done(); it.next()) {
        const SkIRect& rect = it.rect();
        const auto above = std::find_if(columns.begin(), columns.end(), [&rect](const SkIRect& column) {
            return column.left() == rect.left() && column.right() == rect.right() && column.bottom() == rect.top();
        });
        if (above != columns.end())
            above->fBottom = rect.bottom();
        else
            columns.push_back(rect);
    }
    std::vector<RectF> rects;
    for (const SkIRect& device : columns) {
        rects.push_back({device.left() / m_scale - m_offset.x, device.top() / m_scale - m_offset.y,
                         device.width() / m_scale, device.height() / m_scale});
    }
    return rects;
}

void Painter::clipRoundedRect(const RectF& rect, double radius, Corners corners)
{
    const SkRect device = toSkRect(snappedRect(rect));
    const SkScalar r = SkScalar(radius * m_scale);
    if (corners == Corners::Continuous)
        m_canvas->clipPath(roundedRectPath(device, r, true), true);
    else
        m_canvas->clipRRect(SkRRect::MakeRectXY(device, r, r), true);
}

void Painter::translate(PointF offset)
{
    m_offset.x += offset.x;
    m_offset.y += offset.y;
}

void Painter::rotate(double degrees, PointF centre)
{
    m_canvas->rotate(SkScalar(degrees), SkScalar((m_offset.x + centre.x) * m_scale),
                     SkScalar((m_offset.y + centre.y) * m_scale));
}

// Where a GPU rasterises a plain rectangle: a pixel is covered when
// its centre is, so an edge on a pixel centre goes to the lower side.
int Painter::toDevice(double logical) const
{
    return int(std::ceil(logical * m_scale - 0.5));
}

Rect Painter::deviceRect(const RectF& rect) const
{
    const int left = toDevice(m_offset.x + rect.x);
    const int top = toDevice(m_offset.y + rect.y);
    const int right = toDevice(m_offset.x + rect.right());
    const int bottom = toDevice(m_offset.y + rect.bottom());
    return {left, top, right - left, bottom - top};
}

int Painter::borderWidth(double logical) const
{
    return std::max(1, int(std::lround(logical * m_scale)));
}

void Painter::fillRect(const RectF& rect, Color color)
{
    m_canvas->drawRect(toSkRect(deviceRect(rect)), makePaint(color, m_opacity, false));
}

Corners themeCorners()
{
    return Theme::current().continuousCorners ? Corners::Continuous : Corners::Circular;
}

Rect Painter::snappedRect(const RectF& rect) const
{
    constexpr double kSlack = 1e-4;
    const auto snapped = [this](double begin, double extent) {
        return std::pair{int(std::ceil(begin * m_scale - kSlack)),
                         std::max(int(std::floor(extent * m_scale + kSlack)), 1)};
    };
    const auto [x, width] = snapped(m_offset.x + rect.x, rect.width);
    const auto [y, height] = snapped(m_offset.y + rect.y, rect.height);
    return {x, y, width, height};
}

static SkColor4f premultiplied(Color color, double opacity)
{
    const float a = float(color.a / 255.0 * opacity);
    return {float(color.r / 255.0) * a, float(color.g / 255.0) * a, float(color.b / 255.0) * a, a};
}

void Painter::drawRoundedRect(const RectF& rect, const RoundedShape& shape, Color fill, double borderWidth,
                              Color border)
{
    const bool paintsFill = fill.a > 0;
    const bool paintsBorder = borderWidth > 0 && border.a > 0;
    if (rect.width <= 0 || rect.height <= 0 || (!paintsFill && !paintsBorder))
        return;

    // The shape snaps, so a ring around a field lands where the field does;
    // the inset and the border are whole pixels.
    const double inset = std::round(shape.inset * m_scale);
    const double borderPixels = borderWidth > 0 ? std::max(std::round(borderWidth * m_scale), 1.0) : 0;
    const Rect snapped = snappedRect({rect.x - shape.inset, rect.y - shape.inset, rect.width + 2 * shape.inset,
                                      rect.height + 2 * shape.inset});
    const SkIRect quad = SkIRect::MakeXYWH(snapped.x, snapped.y, snapped.width, snapped.height)
                                 .makeInset(int(inset), int(inset));
    drawRoundedShape(m_canvas, {quad, float(std::max(shape.radius, 0.0) * m_scale), float(inset), float(borderPixels),
                                shape.corners == Corners::Continuous, premultiplied(fill, m_opacity),
                                premultiplied(paintsBorder ? border : Color{0, 0, 0, 0}, m_opacity)});
}

void Painter::fillRoundedRect(const RectF& rect, double radius, Color color, Corners corners)
{
    drawRoundedRect(rect, {radius, 0, corners}, color);
}

void Painter::fillDeviceRoundedRect(const Rect& rect, double radiusPx, Color color)
{
    const SkScalar r = SkScalar(radiusPx);
    m_canvas->drawRRect(SkRRect::MakeRectXY(toSkRect(rect), r, r), makePaint(color, m_opacity, true));
}

void Painter::strokeDeviceRoundedRect(const Rect& rect, double radiusPx, Color color, double widthPx)
{
    SkPaint paint = makePaint(color, m_opacity, true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(SkScalar(widthPx));
    const SkScalar half = SkScalar(widthPx / 2);
    const SkScalar r = std::max(SkScalar(radiusPx) - half, SkScalar(0));
    m_canvas->drawRRect(SkRRect::MakeRectXY(toSkRect(rect).makeInset(half, half), r, r), paint);
}

void Painter::strokeRoundedRect(const RectF& rect, double radius, Color color, int widthPx, Corners corners)
{
    drawRoundedRect(rect, {radius, 0, corners}, Color{0, 0, 0, 0}, widthPx / m_scale, color);
}

static sk_sp<SkShader> makeShader(const LinearGradient& gradient, PointF offset, double scale, double opacity)
{
    std::vector<SkColor4f> colors;
    std::vector<float> positions;
    for (const GradientStop& stop : gradient.stops) {
        SkColor4f color = SkColor4f::FromColor(stop.color.argb());
        color.fA *= float(opacity);
        colors.push_back(color);
        positions.push_back(float(stop.position));
    }
    const SkPoint points[2] = {
        {SkScalar((offset.x + gradient.start.x) * scale), SkScalar((offset.y + gradient.start.y) * scale)},
        {SkScalar((offset.x + gradient.end.x) * scale), SkScalar((offset.y + gradient.end.y) * scale)},
    };
    const SkGradient::Colors spec(SkSpan<const SkColor4f>(colors), SkSpan<const float>(positions), SkTileMode::kClamp);
    return SkShaders::LinearGradient(points, SkGradient(spec, SkGradient::Interpolation()));
}

void Painter::fillRoundedRect(const RectF& rect, double radius, const LinearGradient& gradient, Corners corners)
{
    if (gradient.stops.empty())
        return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(makeShader(gradient, m_offset, m_scale, m_opacity));
    const SkRect device = toSkRect(snappedRect(rect));
    const SkScalar r = SkScalar(radius * m_scale);
    if (corners == Corners::Continuous)
        m_canvas->drawPath(roundedRectPath(device, r, true), paint);
    else
        m_canvas->drawRRect(SkRRect::MakeRectXY(device, r, r), paint);
}

SkPath Painter::toSkPath(const Path& path) const
{
    return PathAccess::toSkPath(path, m_scale, m_offset);
}

void Painter::fillPath(const Path& path, Color color)
{
    m_canvas->drawPath(toSkPath(path), makePaint(color, m_opacity, true));
}

void Painter::fillPath(const Path& path, const LinearGradient& gradient)
{
    if (gradient.stops.empty())
        return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(makeShader(gradient, m_offset, m_scale, m_opacity));
    m_canvas->drawPath(toSkPath(path), paint);
}

void Painter::strokePath(const Path& path, Color color, double width, LineCap cap, LineJoin join)
{
    SkPaint paint = makePaint(color, m_opacity, true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(SkScalar(width * m_scale));
    switch (cap) {
    case LineCap::Butt:
        paint.setStrokeCap(SkPaint::kButt_Cap);
        break;
    case LineCap::Round:
        paint.setStrokeCap(SkPaint::kRound_Cap);
        break;
    case LineCap::Square:
        paint.setStrokeCap(SkPaint::kSquare_Cap);
        break;
    }
    switch (join) {
    case LineJoin::Miter:
        paint.setStrokeJoin(SkPaint::kMiter_Join);
        break;
    case LineJoin::Round:
        paint.setStrokeJoin(SkPaint::kRound_Join);
        break;
    case LineJoin::Bevel:
        paint.setStrokeJoin(SkPaint::kBevel_Join);
        break;
    }
    m_canvas->drawPath(toSkPath(path), paint);
}

void Painter::drawSvg(const std::string& file, const RectF& rect, std::optional<Color> tint)
{
    const Rect device = deviceRect(rect);
    if (device.width <= 0 || device.height <= 0)
        return;
    const sk_sp<SkImage> image = IconLoader::instance().svg(file, device.width, device.height, tint ? &*tint : nullptr);
    drawImage(image.get(), device.x, device.y);
}

void Painter::drawSmoothRectangle(const RectF& rect, double radius, Color fill, double borderWidth, Color border)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;
    // A radius of half a pixel or less draws square corners.
    if (std::min(std::min(rect.width, rect.height) * 0.4999, radius) > 0.5) {
        drawSmoothRoundedRectangle(m_canvas, {m_offset.x + rect.x, m_offset.y + rect.y, rect.width, rect.height},
                                  radius, m_scale, fill, border.a > 0 ? borderWidth : 0, border, float(m_opacity));
        return;
    }
    const SkRect device = SkRect::MakeXYWH(SkScalar((m_offset.x + rect.x) * m_scale), SkScalar((m_offset.y + rect.y) * m_scale),
                                           SkScalar(rect.width * m_scale), SkScalar(rect.height * m_scale));
    const SkScalar r = SkScalar(std::clamp(radius, 0.0, std::min(rect.width, rect.height) / 2) * m_scale);
    const SkRRect outer = SkRRect::MakeRectXY(device, r, r);
    const SkScalar width = SkScalar(std::max(borderWidth, 0.0) * m_scale);
    if (width <= 0 || border.a == 0) {
        m_canvas->drawRRect(outer, makePaint(fill, m_opacity, true));
        return;
    }
    const SkScalar innerRadius = std::max<SkScalar>(0, r - width);
    const SkRRect inner = SkRRect::MakeRectXY(device.makeInset(width, width), innerRadius, innerRadius);
    if (fill.a > 0)
        m_canvas->drawRRect(inner, makePaint(fill, m_opacity, true));
    m_canvas->drawDRRect(outer, inner, makePaint(border, m_opacity, true));
}

void Painter::fillEllipse(const RectF& rect, Color color)
{
    m_canvas->drawOval(toSkRect(deviceRect(rect)), makePaint(color, m_opacity, true));
}

void Painter::drawShadow(const RectF& rect, double radius, double sigma, Color color,
                         PointF offset)
{
    SkPaint paint = makePaint(color, m_opacity, true);
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, SkScalar(sigma * m_scale)));
    const RectF shifted = {rect.x + offset.x, rect.y + offset.y, rect.width, rect.height};
    const SkScalar r = SkScalar(radius * m_scale);
    m_canvas->drawRRect(SkRRect::MakeRectXY(toSkRect(deviceRect(shifted)), r, r), paint);
}

void Painter::drawHorizontalLine(double x, double y, double width, Color color, int widthPx)
{
    const int left = toDevice(m_offset.x + x);
    const int right = toDevice(m_offset.x + x + width);
    const int top = toDevice(m_offset.y + y);
    m_canvas->drawRect(toSkRect({left, top, right - left, widthPx}), makePaint(color, m_opacity, false));
}

void Painter::drawVerticalLine(double x, double y, double height, Color color, int widthPx)
{
    const int left = toDevice(m_offset.x + x);
    const int top = toDevice(m_offset.y + y);
    const int bottom = toDevice(m_offset.y + y + height);
    m_canvas->drawRect(toSkRect({left, top, widthPx, bottom - top}), makePaint(color, m_opacity, false));
}

void Painter::drawImage(const SkImage* image, const RectF& rect)
{
    if (!image)
        return;
    const Rect device = deviceRect(rect);
    if (device.width <= 0 || device.height <= 0)
        return;
    SkPaint paint;
    paint.setAlphaf(float(m_opacity));
    const bool shrinks = device.width < image->width() || device.height < image->height();
    const SkSamplingOptions sampling = shrinks ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                               : SkSamplingOptions(SkFilterMode::kLinear);
    m_canvas->drawImageRect(image, toSkRect(device), sampling, &paint);
}

// Copies of the Image pixels drawn lately, by where the pixels were: one
// drawn every frame is copied, and uploaded to the GPU, once while its
// pixels stay the same.
static constexpr size_t kImageCopiesBytes = size_t(8) * 1024 * 1024;
static constexpr size_t kImageCopies = 16;

namespace {

struct ImageCopy
{
    const uint32_t* source;
    sk_sp<SkImage> image;
};

} // namespace

static sk_sp<SkImage> imageCopy(const Image& image)
{
    // 0xAARRGGBB words are BGRA bytes on a little-endian machine.
    const size_t rowBytes = size_t(image.width()) * 4;
    const SkPixmap pixels(SkImageInfo::Make(image.width(), image.height(), kBGRA_8888_SkColorType,
                                            kUnpremul_SkAlphaType),
                          image.bits(), rowBytes);
    const size_t bytes = rowBytes * size_t(image.height());
    if (bytes > kImageCopiesBytes)
        return SkImages::RasterFromPixmapCopy(pixels);

    // Most recently drawn first.
    thread_local std::list<ImageCopy> copies;
    for (auto it = copies.begin(); it != copies.end(); ++it) {
        SkPixmap kept;
        if (it->source != image.bits() || it->image->width() != image.width()
            || it->image->height() != image.height() || !it->image->peekPixels(&kept))
            continue;
        bool same = true;
        for (int y = 0; y < image.height() && same; ++y)
            same = std::memcmp(kept.addr32(0, y), pixels.addr32(0, y), rowBytes) == 0;
        if (!same) {
            copies.erase(it);
            break;
        }
        copies.splice(copies.begin(), copies, it);
        return copies.front().image;
    }
    sk_sp<SkImage> copy = SkImages::RasterFromPixmapCopy(pixels);
    if (!copy)
        return nullptr;
    copies.push_front({image.bits(), copy});
    size_t kept = 0;
    size_t count = 0;
    for (auto it = copies.begin(); it != copies.end();) {
        kept += size_t(it->image->width()) * size_t(it->image->height()) * 4;
        if (++count > kImageCopies || kept > kImageCopiesBytes)
            it = copies.erase(it);
        else
            ++it;
    }
    return copy;
}

void Painter::drawImage(const Image& image, const RectF& rect)
{
    if (image.isNull())
        return;
    const sk_sp<SkImage> pixels = imageCopy(image);
    drawImage(pixels.get(), rect);
}

void Painter::drawImage(const SkImage* image, int x, int y)
{
    if (!image)
        return;
    SkPaint paint;
    paint.setAlphaf(float(m_opacity));
    m_canvas->drawImage(image, SkScalar(x), SkScalar(y), SkSamplingOptions(), &paint);
}

void Painter::drawLine(PointF from, PointF to, Color color, double widthPx)
{
    SkPaint paint = makePaint(color, m_opacity, true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(SkScalar(widthPx));
    paint.setStrokeCap(SkPaint::kRound_Cap);
    m_canvas->drawLine(SkScalar((m_offset.x + from.x) * m_scale),
                       SkScalar((m_offset.y + from.y) * m_scale),
                       SkScalar((m_offset.x + to.x) * m_scale),
                       SkScalar((m_offset.y + to.y) * m_scale), paint);
}

} // namespace cutefish::ui
