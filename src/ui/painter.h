// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ui/geometry.h"
#include "ui/path.h"

class SkCanvas;
class SkRegion;
class SkImage;
class SkPath;

namespace cutefish::ui {

class Image;

enum class Corners {
    Circular,
    // The compositor's window corners, for large panels; controls stay circular.
    Continuous,
};

// RoundedRectangle's corners unless an item sets them: the theme's.
Corners themeCorners();

// A rounded rect, and the parallel curve of its shape `inset` beyond it (inside
// when negative), which keeps frames, rings and thumbnails concentric with
// continuous corners.
struct RoundedShape
{
    double radius = 0;
    double inset = 0;
    Corners corners = Corners::Circular;
};

// Paints in logical coordinates onto a canvas in device pixels. Every edge
// is rounded to the device grid at its absolute position, each on its own,
// so boxes that share a logical edge share a device edge and nothing
// straddles a pixel. Line widths are given in device pixels.
class Painter
{
public:
    Painter(SkCanvas* canvas, double scale);

    double scale() const { return m_scale; }
    SkCanvas* canvas() const { return m_canvas; }

    // Saves and restores the offset, the clip and the opacity.
    void save();
    void restore();
    void translate(PointF offset);
    // Turns what is drawn from here on by the degrees, clockwise, about a
    // point in logical coordinates; inside save() and restore(). Edges are
    // still snapped before the turn, so draw smooth content (images, paths).
    void rotate(double degrees, PointF centre);
    PointF offset() const { return m_offset; }
    // Multiplies into what is drawn from here on.
    void multiplyOpacity(double opacity) { m_opacity *= opacity; }
    double opacity() const { return m_opacity; }
    // Intersects the clip with the rect, edges on device pixels.
    void clip(const RectF& rect);
    // The clip's bounds in logical coordinates from the offset: where
    // anything drawn can still show.
    RectF clipBounds() const;
    // The separate parts of the clip a frame repaints, in the same
    // coordinates; the clip's bounds when it is one part.
    std::vector<RectF> clipRects() const;
    // The device pixels this frame repaints, untransformed, which
    // clipRects() splits the clip by.
    void setDamage(const SkRegion* damage) { m_damage = damage; }
    // Intersects the clip with a rounded rect, antialiased, for content cut
    // to a card's or a thumbnail's corners; the rect snapped as a shape.
    void clipRoundedRect(const RectF& rect, double radius, Corners corners = Corners::Circular);

    // Each edge on its own, as a GPU rasterises a plain rectangle: halves
    // round down. Rounded shapes and icons snap instead.
    int toDevice(double logical) const;
    Rect deviceRect(const RectF& rect) const;
    // The whole device pixels a logical border takes, at least one.
    int borderWidth(double logical) const;
    // The first edge on the next whole device pixel, and as many pixels as the
    // rect encloses, at least one.
    Rect snappedRect(const RectF& rect) const;

    void fillRect(const RectF& rect, Color color);
    // The rounded rectangle most controls draw with: the
    // shape snapped, the inset and a border of logical width in whole device
    // pixels (at least one), and its shader's coverage. The border lies
    // inside the shape's outline.
    void drawRoundedRect(const RectF& rect, const RoundedShape& shape, Color fill, double borderWidth = 0,
                         Color border = {});
    void fillRoundedRect(const RectF& rect, double radius, Color color,
                         Corners corners = Corners::Circular);
    void fillRoundedRect(const RectF& rect, double radius, const LinearGradient& gradient,
                         Corners corners = Corners::Circular);
    // A rectangle with a radius, which the switch, slider handle and scroll
    // bar use: antialiased where it lies, not snapped, with a border of
    // logical width inside its outline.
    void drawSmoothRectangle(const RectF& rect, double radius, Color fill, double borderWidth = 0,
                            Color border = {});
    // Exactly the device pixels given, for shapes centred by their own rules.
    void fillDeviceRoundedRect(const Rect& rect, double radiusPx, Color color);
    // An antialiased border of any width inside exactly the device pixels given.
    void strokeDeviceRoundedRect(const Rect& rect, double radiusPx, Color color, double widthPx);
    // A border inside the rect, as RoundedRectangle's.
    void strokeRoundedRect(const RectF& rect, double radius, Color color, int widthPx = 1,
                           Corners corners = Corners::Circular);
    void fillEllipse(const RectF& rect, Color color);
    // How far past its rect a shadow of this blur shows.
    static double shadowReach(double sigma) { return 3 * sigma; }
    // A blurred copy of the rounded rect, sigma in logical pixels, under what follows.
    void drawShadow(const RectF& rect, double radius, double sigma, Color color, PointF offset = {});
    void drawHorizontalLine(double x, double y, double width, Color color, int widthPx = 1);
    void drawVerticalLine(double x, double y, double height, Color color, int widthPx = 1);
    // Antialiased and not snapped, for glyph-like strokes.
    void drawLine(PointF from, PointF to, Color color, double widthPx);
    // At whole device pixels.
    void drawImage(const SkImage* image, int x, int y);
    // Scaled smoothly into the rect, its edges on device pixels, as a pixmap
    // drawn into a smaller or larger item; averaged when shrunk.
    void drawImage(const SkImage* image, const RectF& rect);
    // An Image, 0xAARRGGBB words not premultiplied, the same way.
    void drawImage(const Image& image, const RectF& rect);

    // Paths are antialiased and not snapped; widths are logical.
    void fillPath(const Path& path, Color color);
    void fillPath(const Path& path, const LinearGradient& gradient);
    void strokePath(const Path& path, Color color, double width, LineCap cap = LineCap::Round,
                    LineJoin join = LineJoin::Round);

    // An SVG file rasterised at the device size of the rect, which it fills;
    // a single-colour drawing takes the tint where it has coverage.
    void drawSvg(const std::string& file, const RectF& rect, std::optional<Color> tint = std::nullopt);

private:
    SkPath toSkPath(const Path& path) const;

    struct State
    {
        PointF offset;
        double opacity;
    };

    SkCanvas* m_canvas;
    double m_scale;
    PointF m_offset;
    double m_opacity = 1;
    std::vector<State> m_saved;
    const SkRegion* m_damage = nullptr;
};

} // namespace cutefish::ui
