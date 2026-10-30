// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/canvas_2d.h"
#include "ui/bitmap_private.h"
#include "ui/path_private.h"

#include <algorithm>
#include <cmath>

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"

#include "ui/painter.h"
#include "ui/text_layout.h"

namespace cutefish::ui {

struct Canvas2D::Owned
{
    SkBitmap bitmap;
    std::unique_ptr<SkCanvas> canvas;
    SkMatrix base;
};

static SkMatrix toSkMatrix(const base::AffineTransform& t)
{
    return SkMatrix::MakeAll(SkScalar(t.m11()), SkScalar(t.m21()), SkScalar(t.dx()), SkScalar(t.m12()),
                             SkScalar(t.m22()), SkScalar(t.dy()), 0, 0, 1);
}

static SkColor4f toSkColor(const ColorF& color, double alpha)
{
    return {float(std::clamp(color.red, 0.0, 1.0)), float(std::clamp(color.green, 0.0, 1.0)),
            float(std::clamp(color.blue, 0.0, 1.0)), float(std::clamp(color.alpha * alpha, 0.0, 1.0))};
}

static SkRect toSkRect(const RectF& rect)
{
    return SkRect::MakeXYWH(SkScalar(rect.x), SkScalar(rect.y), SkScalar(rect.width), SkScalar(rect.height));
}

static SkBlendMode toSkBlendMode(Canvas2D::Composite mode)
{
    using Mode = Canvas2D::Composite;
    switch (mode) {
    case Mode::SourceOver: return SkBlendMode::kSrcOver;
    case Mode::Multiply: return SkBlendMode::kMultiply;
    case Mode::Screen: return SkBlendMode::kScreen;
    case Mode::Overlay: return SkBlendMode::kOverlay;
    case Mode::Darken: return SkBlendMode::kDarken;
    case Mode::Lighten: return SkBlendMode::kLighten;
    case Mode::ColorDodge: return SkBlendMode::kColorDodge;
    case Mode::ColorBurn: return SkBlendMode::kColorBurn;
    case Mode::SoftLight: return SkBlendMode::kSoftLight;
    case Mode::HardLight: return SkBlendMode::kHardLight;
    case Mode::Difference: return SkBlendMode::kDifference;
    case Mode::Exclusion: return SkBlendMode::kExclusion;
    case Mode::Hue: return SkBlendMode::kHue;
    case Mode::Saturation: return SkBlendMode::kSaturation;
    case Mode::Color: return SkBlendMode::kColor;
    case Mode::Luminosity: return SkBlendMode::kLuminosity;
    case Mode::Clear: return SkBlendMode::kClear;
    case Mode::Copy: return SkBlendMode::kSrc;
    case Mode::SourceIn: return SkBlendMode::kSrcIn;
    case Mode::SourceOut: return SkBlendMode::kSrcOut;
    case Mode::SourceAtop: return SkBlendMode::kSrcATop;
    case Mode::DestinationOver: return SkBlendMode::kDstOver;
    case Mode::DestinationIn: return SkBlendMode::kDstIn;
    case Mode::DestinationOut: return SkBlendMode::kDstOut;
    case Mode::DestinationAtop: return SkBlendMode::kDstATop;
    case Mode::Xor: return SkBlendMode::kXor;
    case Mode::Lighter: return SkBlendMode::kPlus;
    }
    return SkBlendMode::kSrcOver;
}

static SkSamplingOptions toSampling(Canvas2D::Smoothing quality)
{
    switch (quality) {
    case Canvas2D::Smoothing::None:
        return SkSamplingOptions(SkFilterMode::kNearest);
    case Canvas2D::Smoothing::Low:
        return SkSamplingOptions(SkFilterMode::kLinear);
    case Canvas2D::Smoothing::Medium:
        return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest);
    case Canvas2D::Smoothing::High:
        return SkSamplingOptions(SkCubicResampler::Mitchell());
    }
    return {};
}

Canvas2D::Canvas2D(Painter& painter)
    : m_canvas(painter.canvas())
    , m_owned(std::make_unique<Owned>())
    , m_baseScale(painter.scale())
{
    m_owned->base = m_canvas->getTotalMatrix();
    m_owned->base.preTranslate(SkScalar(painter.offset().x * painter.scale()),
                               SkScalar(painter.offset().y * painter.scale()));
    m_owned->base.preScale(SkScalar(painter.scale()), SkScalar(painter.scale()));
    m_state.alpha = painter.opacity();
    m_initialSaveCount = m_canvas->save();
    applyMatrix();
}

Canvas2D::Canvas2D(Bitmap& bitmap)
    : m_owned(std::make_unique<Owned>())
{
    if (bitmap.isNull() || bitmap.format() == Bitmap::Format::Alpha8)
        return;
    const SkImageInfo info = SkImageInfo::Make(bitmap.width(), bitmap.height(), skColorType(bitmap.format()),
                                               kPremul_SkAlphaType);
    if (!m_owned->bitmap.installPixels(info, bitmap.bits(), bitmap.stride()))
        return;
    m_owned->canvas = std::make_unique<SkCanvas>(m_owned->bitmap);
    m_canvas = m_owned->canvas.get();
    m_owned->base = SkMatrix::I();
    m_initialSaveCount = m_canvas->save();
    applyMatrix();
}

Canvas2D::~Canvas2D()
{
    if (m_canvas)
        m_canvas->restoreToCount(m_initialSaveCount);
}

void Canvas2D::applyMatrix()
{
    if (!m_canvas)
        return;
    SkMatrix total = m_owned->base;
    total.preConcat(toSkMatrix(m_state.transform));
    m_canvas->setMatrix(total);
}

void Canvas2D::save()
{
    m_saved.push_back(m_state);
    m_state.groups = 0;
    if (m_canvas)
        m_canvas->save();
}

void Canvas2D::restore()
{
    if (m_saved.empty())
        return;
    while (m_state.groups > 0)
        endGroup();
    m_state = m_saved.back();
    m_saved.pop_back();
    if (m_canvas) {
        m_canvas->restore();
        applyMatrix();
    }
}

void Canvas2D::setTransform(const base::AffineTransform& transform)
{
    m_state.transform = transform;
    applyMatrix();
}

void Canvas2D::concat(const base::AffineTransform& transform)
{
    m_state.transform = transform * m_state.transform;
    applyMatrix();
}

void Canvas2D::translate(double dx, double dy)
{
    m_state.transform.translate(dx, dy);
    applyMatrix();
}

void Canvas2D::scale(double sx, double sy)
{
    m_state.transform.scale(sx, sy);
    applyMatrix();
}

void Canvas2D::rotate(double radians)
{
    m_state.transform.rotate(radians * 180 / M_PI);
    applyMatrix();
}

void Canvas2D::clipToRect(const RectF& rect)
{
    if (m_canvas)
        m_canvas->clipRect(toSkRect(rect), m_state.antialias);
}

void Canvas2D::clipToPath(const Path& path)
{
    if (m_canvas)
        m_canvas->clipPath(PathAccess::toSkPath(path), m_state.antialias);
}

RectF Canvas2D::clipBounds() const
{
    if (!m_canvas)
        return {};
    // The device bounds, which unlike the local ones are not outset.
    SkMatrix inverse;
    if (!m_canvas->getTotalMatrix().invert(&inverse))
        return {};
    const SkRect bounds = inverse.mapRect(SkRect::Make(m_canvas->getDeviceClipBounds()));
    return {bounds.x(), bounds.y(), bounds.width(), bounds.height()};
}

void Canvas2D::setLineDash(std::vector<double> lengths, double offset)
{
    m_state.dashOffset = offset;
    m_state.dashes = std::move(lengths);
}

void Canvas2D::setShadow(PointF offset, double blur, const ColorF& color)
{
    m_state.shadowOffset = offset;
    m_state.shadowBlur = blur;
    m_state.shadowColor = color;
}

// The paint for a fill or stroke in the current state; `draw` gets the
// canvas and the paint, first for the shadow when there is one.
void Canvas2D::drawWith(const std::function<void(SkCanvas*, const void*)>& draw, const ColorF& color,
                               bool stroke)
{
    if (!m_canvas)
        return;
    SkPaint paint;
    paint.setAntiAlias(m_state.antialias);
    paint.setColor4f(toSkColor(color, m_state.alpha));
    paint.setBlendMode(toSkBlendMode(m_state.composite));
    if (stroke) {
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(SkScalar(m_state.lineWidth));
        paint.setStrokeCap(m_state.lineCap == LineCap::Round    ? SkPaint::kRound_Cap
                           : m_state.lineCap == LineCap::Square ? SkPaint::kSquare_Cap
                                                                : SkPaint::kButt_Cap);
        paint.setStrokeJoin(m_state.lineJoin == LineJoin::Round   ? SkPaint::kRound_Join
                            : m_state.lineJoin == LineJoin::Bevel ? SkPaint::kBevel_Join
                                                                  : SkPaint::kMiter_Join);
        paint.setStrokeMiter(SkScalar(m_state.miterLimit));
        if (!m_state.dashes.empty()) {
            std::vector<SkScalar> intervals;
            for (double length : m_state.dashes)
                intervals.push_back(SkScalar(std::max(length, 0.0)));
            // Skia takes pairs; an odd pattern repeats to make them.
            if (intervals.size() % 2)
                intervals.insert(intervals.end(), intervals.begin(), intervals.end());
            paint.setPathEffect(SkDashPathEffect::Make(intervals, SkScalar(m_state.dashOffset)));
        }
    }
    if (m_state.shadowColor.alpha > 0 && (m_state.shadowBlur > 0 || m_state.shadowOffset.x != 0
                                           || m_state.shadowOffset.y != 0)) {
        SkPaint shadow = paint;
        shadow.setColor4f(toSkColor(m_state.shadowColor, m_state.alpha));
        const double sigma = m_state.shadowBlur * m_baseScale / 2;
        if (sigma > 0)
            shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, SkScalar(sigma), false));
        // The offset is in the canvas's own units, whatever the transform.
        const SkVector device = m_owned->base.mapVector(SkScalar(m_state.shadowOffset.x),
                                                         SkScalar(m_state.shadowOffset.y));
        m_canvas->save();
        SkMatrix moved = m_canvas->getTotalMatrix();
        moved.postTranslate(device.x(), device.y());
        m_canvas->setMatrix(moved);
        draw(m_canvas, &shadow);
        m_canvas->restore();
    }
    draw(m_canvas, &paint);
}

void Canvas2D::fillPath(const Path& path)
{
    const SkPath skPath = PathAccess::toSkPath(path);
    drawWith([&](SkCanvas* canvas, const void* paint) { canvas->drawPath(skPath, *static_cast<const SkPaint*>(paint)); },
             m_state.fill, false);
}

void Canvas2D::strokePath(const Path& path)
{
    const SkPath skPath = PathAccess::toSkPath(path);
    drawWith([&](SkCanvas* canvas, const void* paint) { canvas->drawPath(skPath, *static_cast<const SkPaint*>(paint)); },
             m_state.stroke, true);
}

void Canvas2D::fillRect(const RectF& rect)
{
    const SkRect skRect = toSkRect(rect);
    drawWith([&](SkCanvas* canvas, const void* paint) { canvas->drawRect(skRect, *static_cast<const SkPaint*>(paint)); },
             m_state.fill, false);
}

void Canvas2D::strokeRect(const RectF& rect)
{
    const SkRect skRect = toSkRect(rect);
    drawWith([&](SkCanvas* canvas, const void* paint) { canvas->drawRect(skRect, *static_cast<const SkPaint*>(paint)); },
             m_state.stroke, true);
}

void Canvas2D::clearRect(const RectF& rect)
{
    if (!m_canvas)
        return;
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kClear);
    paint.setAntiAlias(m_state.antialias);
    m_canvas->drawRect(toSkRect(rect), paint);
}

static SkGradient makeGradient(const std::vector<GradientStopF>& stops, double alpha, std::vector<SkColor4f>& colors,
                               std::vector<float>& positions, SkTileMode tile)
{
    for (const GradientStopF& stop : stops) {
        colors.push_back(toSkColor(stop.color, alpha));
        positions.push_back(float(std::clamp(stop.offset, 0.0, 1.0)));
    }
    const SkGradient::Colors spec(SkSpan<const SkColor4f>(colors), SkSpan<const float>(positions), tile);
    return SkGradient(spec, SkGradient::Interpolation());
}

void Canvas2D::fillLinearGradient(const std::vector<GradientStopF>& stops, PointF start, PointF end,
                                         bool padStart, bool padEnd)
{
    if (!m_canvas || stops.empty())
        return;
    std::vector<SkColor4f> colors;
    std::vector<float> positions;
    const SkPoint points[2] = {{SkScalar(start.x), SkScalar(start.y)}, {SkScalar(end.x), SkScalar(end.y)}};
    SkPaint paint;
    paint.setAntiAlias(m_state.antialias);
    paint.setBlendMode(toSkBlendMode(m_state.composite));
    paint.setShader(SkShaders::LinearGradient(points, makeGradient(stops, m_state.alpha, colors, positions,
                                                                   SkTileMode::kClamp)));
    m_canvas->save();
    // An end that does not pad cuts the plane across the axis there.
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double length = std::hypot(dx, dy);
    if (length > 0 && (!padStart || !padEnd)) {
        const double ux = dx / length;
        const double uy = dy / length;
        const double far = 1e6;
        const PointF from = padStart ? PointF{start.x - ux * far, start.y - uy * far} : start;
        const PointF to = padEnd ? PointF{end.x + ux * far, end.y + uy * far} : end;
        SkPathBuilder band;
        band.moveTo(SkScalar(from.x - uy * far), SkScalar(from.y + ux * far));
        band.lineTo(SkScalar(to.x - uy * far), SkScalar(to.y + ux * far));
        band.lineTo(SkScalar(to.x + uy * far), SkScalar(to.y - ux * far));
        band.lineTo(SkScalar(from.x + uy * far), SkScalar(from.y - ux * far));
        band.close();
        m_canvas->clipPath(band.detach(), m_state.antialias);
    }
    m_canvas->drawPaint(paint);
    m_canvas->restore();
}

void Canvas2D::fillRadialGradient(const std::vector<GradientStopF>& stops, PointF startCentre,
                                         double startRadius, PointF endCentre, double endRadius, bool padStart,
                                         bool padEnd)
{
    if (!m_canvas || stops.empty())
        return;
    std::vector<SkColor4f> colors;
    std::vector<float> positions;
    // Decal leaves both sides empty; clamping stands for either end padding.
    const SkTileMode tile = padStart || padEnd ? SkTileMode::kClamp : SkTileMode::kDecal;
    SkPaint paint;
    paint.setAntiAlias(m_state.antialias);
    paint.setBlendMode(toSkBlendMode(m_state.composite));
    paint.setShader(SkShaders::TwoPointConicalGradient(
        {SkScalar(startCentre.x), SkScalar(startCentre.y)}, SkScalar(std::max(startRadius, 0.0)),
        {SkScalar(endCentre.x), SkScalar(endCentre.y)}, SkScalar(std::max(endRadius, 0.0)),
        makeGradient(stops, m_state.alpha, colors, positions, tile)));
    m_canvas->drawPaint(paint);
}

struct RetainedBitmap::Data
{
    sk_sp<SkImage> image;
};

RetainedBitmap::RetainedBitmap() = default;

RetainedBitmap::RetainedBitmap(const Bitmap& bitmap)
{
    if (bitmap.isNull())
        return;
    sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(skPixmap(bitmap));
    if (image)
        d = std::make_shared<const Data>(Data {std::move(image)});
}

RetainedBitmap::~RetainedBitmap() = default;
RetainedBitmap::RetainedBitmap(const RetainedBitmap&) = default;
RetainedBitmap& RetainedBitmap::operator=(const RetainedBitmap&) = default;

int RetainedBitmap::width() const
{
    return d ? d->image->width() : 0;
}

int RetainedBitmap::height() const
{
    return d ? d->image->height() : 0;
}

size_t RetainedBitmap::byteCount() const
{
    return d ? size_t(d->image->width()) * size_t(d->image->height()) * 4 : 0;
}

void Canvas2D::drawBitmap(const Bitmap& bitmap, const RectF& rect, bool mirrored)
{
    if (!m_canvas || bitmap.isNull())
        return;
    drawImage(SkImages::RasterFromPixmapCopy(skPixmap(bitmap)).get(), rect, mirrored);
}

void Canvas2D::drawBitmap(const RetainedBitmap& bitmap, const RectF& rect, bool mirrored)
{
    if (!m_canvas || bitmap.isNull())
        return;
    drawImage(bitmap.d->image.get(), rect, mirrored);
}

void Canvas2D::drawImage(SkImage* image, const RectF& rect, bool mirrored)
{
    if (!image)
        return;
    SkPaint paint;
    paint.setAntiAlias(m_state.antialias);
    paint.setAlphaf(float(std::clamp(m_state.alpha, 0.0, 1.0)));
    paint.setBlendMode(toSkBlendMode(m_state.composite));
    m_canvas->save();
    if (mirrored) {
        m_canvas->translate(0, SkScalar(rect.y * 2 + rect.height));
        m_canvas->scale(1, -1);
    }
    m_canvas->drawImageRect(image, toSkRect(rect), toSampling(m_state.smoothing), &paint);
    m_canvas->restore();
}

void Canvas2D::beginGroup()
{
    if (!m_canvas)
        return;
    SkPaint paint;
    paint.setAlphaf(float(std::clamp(m_state.alpha, 0.0, 1.0)));
    paint.setBlendMode(toSkBlendMode(m_state.composite));
    // The group casts the shadow set when it began, once for all it draws.
    // The filter works in the layer's space; the offset and blur are in the
    // canvas's own units, as for a single drawing.
    SkMatrix inverse;
    if (m_state.shadowColor.alpha > 0
        && (m_state.shadowBlur > 0 || m_state.shadowOffset.x != 0 || m_state.shadowOffset.y != 0)
        && m_canvas->getTotalMatrix().invert(&inverse)) {
        const SkVector device = m_owned->base.mapVector(SkScalar(m_state.shadowOffset.x),
                                                         SkScalar(m_state.shadowOffset.y));
        const SkVector local = inverse.mapVector(device.x(), device.y());
        const double scale = deviceScale();
        const double sigma = scale > 0 ? m_state.shadowBlur * m_baseScale / 2 / scale : 0;
        paint.setImageFilter(SkImageFilters::DropShadow(local.x(), local.y(), SkScalar(sigma), SkScalar(sigma),
                                                        toSkColor(m_state.shadowColor, 1), nullptr, nullptr));
    }
    save();
    m_canvas->saveLayer(nullptr, &paint);
    m_state.alpha = 1;
    m_state.composite = Composite::SourceOver;
    m_state.shadowColor = {0, 0, 0, 0};
    // restore() of this save ends the group first.
    m_saved.back().groups++;
    m_state.groups = 0;
}

void Canvas2D::endGroup()
{
    if (!m_canvas || m_saved.empty() || m_saved.back().groups == 0)
        return;
    m_saved.back().groups--;
    // The group's layer, then the save under it.
    m_canvas->restore();
    m_state = m_saved.back();
    m_saved.pop_back();
    m_canvas->restore();
    applyMatrix();
}

double Canvas2D::deviceScale() const
{
    SkMatrix total = m_owned->base;
    total.preConcat(toSkMatrix(m_state.transform));
    const double determinant = double(total.getScaleX()) * total.getScaleY() - double(total.getSkewX()) * total.getSkewY();
    return std::sqrt(std::abs(determinant));
}

void Canvas2D::drawText(TextLayout& layout, PointF origin)
{
    if (!m_canvas)
        return;
    const double scale = deviceScale();
    if (scale <= 0)
        return;
    SkMatrix total = m_owned->base;
    total.preConcat(toSkMatrix(m_state.transform));
    const SkPoint device = total.mapPoint({SkScalar(origin.x), SkScalar(origin.y)});
    m_canvas->save();
    m_canvas->resetMatrix();
    Painter painter(m_canvas, scale);
    painter.multiplyOpacity(m_state.alpha);
    // Text keeps to the device's pixels at the space's scale; a turned
    // space turns it about its origin, a mirrored one leaves it upright.
    const double determinant = double(total.getScaleX()) * total.getScaleY() - double(total.getSkewX()) * total.getSkewY();
    const bool turned = determinant > 0 && (total.getSkewX() != 0 || total.getSkewY() != 0 || total.getScaleX() < 0);
    if (turned) {
        SkMatrix placed = SkMatrix::Translate(device.x(), device.y());
        placed.preRotate(SkRadiansToDegrees(std::atan2(total.getSkewY(), total.getScaleX())));
        m_canvas->setMatrix(placed);
        layout.draw(painter, {0, 0});
    } else {
        PointF at {device.x() / scale, device.y() / scale};
        // The layout rounds its origin and its baseline offset each on
        // their own; a fractional origin then lands the baseline a pixel off.
        if (layout.lineCount() > 0) {
            const TextLayout::Line& first = layout.lineAt(0);
            const double baseline = (first.position.y + first.ascent) * scale;
            at.y = (std::round(device.y() + baseline) - std::round(baseline)) / scale;
        }
        layout.draw(painter, at);
    }
    m_canvas->restore();
    applyMatrix();
}

} // namespace cutefish::ui
