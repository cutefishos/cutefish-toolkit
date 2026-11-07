// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "base/transform.h"
#include "ui/bitmap.h"
#include "ui/geometry.h"
#include "ui/path.h"

class SkCanvas;
class SkImage;

namespace cutefish::ui {

class Painter;
class TextLayout;

// A bitmap copied once to be drawn many times: a GPU canvas uploads it on
// its first draw and keeps the texture while any copy of the handle lives.
class RetainedBitmap
{
public:
    RetainedBitmap();
    explicit RetainedBitmap(const Bitmap& bitmap);
    ~RetainedBitmap();
    RetainedBitmap(const RetainedBitmap&);
    RetainedBitmap& operator=(const RetainedBitmap&);

    bool isNull() const { return !d; }
    int width() const;
    int height() const;
    // The pixels the handle keeps, for the bytes it costs.
    size_t byteCount() const;
    // Copies the rect of `bitmap`, which has this one's size, in; a GPU
    // canvas uploads only that part again. Every copy of the handle shows
    // it. Any thread.
    void update(const Bitmap& bitmap, const Rect& rect);

private:
    friend class Canvas2D;
    struct Data;
    std::shared_ptr<Data> d;
};

// A picture another process draws into a DMA-BUF, such as a web page's
// frame, which a GPU canvas draws without reading it into memory. The owner
// may draw into the buffer again between draws. A canvas that paints in
// software, or a driver that cannot take the buffer, does not draw it, and
// the caller then reads the pixels itself.
class DmaBufImage
{
public:
    struct Plane
    {
        int fd = -1;
        uint32_t offset = 0;
        uint32_t stride = 0;
    };

    DmaBufImage();
    // The descriptors are duplicated. fourcc is a DRM format code, and
    // modifier the DRM format modifier, invalid when the layout is implicit.
    DmaBufImage(int width, int height, uint32_t fourcc, uint64_t modifier, const std::vector<Plane>& planes);
    ~DmaBufImage();
    DmaBufImage(const DmaBufImage&);
    DmaBufImage& operator=(const DmaBufImage&);

    bool isNull() const { return !d; }
    int width() const;
    int height() const;

private:
    friend class Canvas2D;
    struct Data;
    std::shared_ptr<Data> d;
};

// A colour with floating-point components, 0 to 1, not premultiplied.
struct ColorF
{
    double red = 0;
    double green = 0;
    double blue = 0;
    double alpha = 1;

    bool operator==(const ColorF&) const = default;
};

struct GradientStopF
{
    double offset = 0;
    ColorF color;
};

// Free-form drawing for views that draw their own content, such as charts
// or documents: paths filled and stroked through a transform, inside clips,
// with gradients, images, shadows, compositing modes and groups, much as a
// web canvas draws. Nothing is snapped to the device grid; text alone goes
// on whole device pixels, as labels do.
//
// State changes last until restore() returns to an earlier save(); every
// save() is matched before the canvas goes.
class Canvas2D
{
public:
    enum class Composite {
        SourceOver,
        Multiply,
        Screen,
        Overlay,
        Darken,
        Lighten,
        ColorDodge,
        ColorBurn,
        SoftLight,
        HardLight,
        Difference,
        Exclusion,
        Hue,
        Saturation,
        Color,
        Luminosity,
        Clear,
        Copy,
        SourceIn,
        SourceOut,
        SourceAtop,
        DestinationOver,
        DestinationIn,
        DestinationOut,
        DestinationAtop,
        Xor,
        Lighter,
    };

    enum class Smoothing {
        None,
        Low,
        Medium,
        High,
    };

    // Over what the painter draws: units are its logical pixels from its
    // origin, y down.
    explicit Canvas2D(Painter& painter);
    // Over a bitmap in Argb32Premultiplied or Rgba8888Premultiplied: units
    // are its pixels, y down. `bitmap` must outlive the canvas.
    explicit Canvas2D(Bitmap& bitmap);
    ~Canvas2D();

    Canvas2D(const Canvas2D&) = delete;
    Canvas2D& operator=(const Canvas2D&) = delete;

    bool isValid() const { return m_canvas != nullptr; }

    void save();
    void restore();

    // From the coordinates drawn in to the canvas's own units.
    const base::AffineTransform& transform() const { return m_state.transform; }
    void setTransform(const base::AffineTransform& transform);
    // Applies before the current transform.
    void concat(const base::AffineTransform& transform);
    void translate(double dx, double dy);
    void scale(double sx, double sy);
    // Clockwise on screen for positive angles.
    void rotate(double radians);
    // Device pixels per unit, under the current transform.
    double deviceScale() const;

    void clipToRect(const RectF& rect);
    void clipToPath(const Path& path);
    // The clip's bounds in the current coordinates.
    RectF clipBounds() const;

    void setGlobalAlpha(double alpha) { m_state.alpha = alpha; }
    double globalAlpha() const { return m_state.alpha; }
    void setComposite(Composite composite) { m_state.composite = composite; }
    void setAntialias(bool antialias) { m_state.antialias = antialias; }
    void setImageSmoothing(Smoothing smoothing) { m_state.smoothing = smoothing; }

    void setFillColor(const ColorF& color) { m_state.fill = color; }
    void setStrokeColor(const ColorF& color) { m_state.stroke = color; }
    const ColorF& fillColor() const { return m_state.fill; }
    const ColorF& strokeColor() const { return m_state.stroke; }
    void setLineWidth(double width) { m_state.lineWidth = width; }
    void setLineCap(LineCap cap) { m_state.lineCap = cap; }
    void setLineJoin(LineJoin join) { m_state.lineJoin = join; }
    void setMiterLimit(double limit) { m_state.miterLimit = limit; }
    // Alternating dash and gap lengths, starting `offset` into the pattern;
    // empty for solid lines.
    void setLineDash(std::vector<double> lengths, double offset = 0);
    double lineWidth() const { return m_state.lineWidth; }

    // A shadow under what is filled and stroked from here on. The offset
    // and blur are in the canvas's own units, untouched by the transform; a
    // transparent colour turns it off.
    void setShadow(PointF offset, double blur, const ColorF& color);

    void fillPath(const Path& path);
    void strokePath(const Path& path);
    void fillRect(const RectF& rect);
    void strokeRect(const RectF& rect);
    // Transparent pixels, whatever the composite mode.
    void clearRect(const RectF& rect);

    // Fills the clip with a gradient from `start` to `end`; beyond an end
    // the colours carry on only when told to.
    void fillLinearGradient(const std::vector<GradientStopF>& stops, PointF start, PointF end, bool padStart,
                            bool padEnd);
    // Between the circle at `startCentre` of `startRadius` and the one at
    // `endCentre`.
    void fillRadialGradient(const std::vector<GradientStopF>& stops, PointF startCentre, double startRadius,
                            PointF endCentre, double endRadius, bool padStart, bool padEnd);

    // Scaled into the rect, its first row at the rect's top, or at its
    // bottom when mirrored.
    void drawBitmap(const Bitmap& bitmap, const RectF& rect, bool mirrored = false);
    void drawBitmap(const RetainedBitmap& bitmap, const RectF& rect, bool mirrored = false);
    // False when this canvas cannot draw it; nothing is drawn then.
    bool drawDmaBufImage(const DmaBufImage& image, const RectF& rect, bool mirrored = false);

    // Drawing from here on is a group, composited as one at the matching
    // end with the global alpha, composite mode and shadow of the begin.
    void beginGroup();
    void endGroup();

    // The layout's lines with their top left at `origin`, mapped through the
    // transform, upright and on whole device pixels. Lay the layout out at
    // deviceScale(). Its colours are multiplied by the global alpha.
    void drawText(TextLayout& layout, PointF origin);

private:
    struct State
    {
        base::AffineTransform transform;
        double alpha = 1;
        Composite composite = Composite::SourceOver;
        bool antialias = true;
        Smoothing smoothing = Smoothing::Medium;
        ColorF fill = {0, 0, 0, 1};
        ColorF stroke = {0, 0, 0, 1};
        double lineWidth = 1;
        LineCap lineCap = LineCap::Butt;
        LineJoin lineJoin = LineJoin::Miter;
        double miterLimit = 10;
        double dashOffset = 0;
        std::vector<double> dashes;
        PointF shadowOffset;
        double shadowBlur = 0;
        ColorF shadowColor = {0, 0, 0, 0};
        // Groups begun at this level, for the matching ends.
        int groups = 0;
    };
    struct Owned;

    void applyMatrix();
    void drawImage(SkImage* image, const RectF& rect, bool mirrored);
    void drawWith(const std::function<void(SkCanvas*, const void* paint)>& draw, const ColorF& color,
                  bool stroke);

    SkCanvas* m_canvas = nullptr;
    std::unique_ptr<Owned> m_owned;
    double m_baseScale = 1;
    int m_initialSaveCount = 0;
    State m_state;
    std::vector<State> m_saved;
};

} // namespace cutefish::ui
