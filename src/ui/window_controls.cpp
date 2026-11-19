// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/window_controls.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"

#include "ui/bitmap_private.h"
#include "ui/color_provider.h"
#include "ui/icon_loader.h"
#include "ui/painter.h"
#include "ui/path.h"

namespace cutefish::ui::window_controls {

static constexpr double kColoredPlate = 14;
static constexpr double kArtwork = 14;
static constexpr double kRim = 0.5;

// The shadow, the glyph and the coloured dot keep their proportions to a
// control's side.
static double shadowSigma(double side)
{
    return side * 0.1;
}

static double shadowOffset(double side)
{
    return side * 0.06;
}

static double scaled(double length, double side)
{
    return std::round(length * side / kButton);
}

static double sideOf(const RectF& bounds)
{
    return std::min(bounds.width, bounds.height);
}

// What the dark plate's light adds to its body at a depth inside its top and
// bottom outline, in points; nothing past the last.
struct EdgeLight
{
    double depth;
    double added;
};

static constexpr EdgeLight kDarkTopLight[] = {{0.25, 58}, {0.75, 38}, {1.25, 14}, {1.75, 11}, {2.25, 8},
                                              {2.75, 7}, {3.25, 5.5}, {3.75, 4.5}, {4.25, 3}, {5.25, 3},
                                              {6.25, 2}, {7.25, 1}, {8, 0}};
static constexpr EdgeLight kDarkBottomLight[] = {{0.25, 82}, {0.75, 41}, {1.25, 11}, {1.75, 9}, {2.25, 7},
                                                 {2.75, 5}, {3.25, 4}, {4.25, 3}, {5.25, 2}, {6.25, 1},
                                                 {8, 0}};

template<size_t N>
static double edgeLightAt(const EdgeLight (&table)[N], double depth)
{
    if (depth <= table[0].depth)
        return table[0].added;
    for (size_t i = 1; i < N; ++i) {
        if (depth <= table[i].depth) {
            const double t = (depth - table[i - 1].depth) / (table[i].depth - table[i - 1].depth);
            return table[i - 1].added + t * (table[i].added - table[i - 1].added);
        }
    }
    return 0;
}

// What an edge lays on one device pixel: a grey, premultiplied, and its alpha.
struct EdgePixel
{
    double grey;
    double alpha;
};

// An edge of `plate` worked out per device pixel from the pixel's distance to
// the outline (negative inside), so it follows the curve smoothly at any
// scale. `shade` takes that distance, how upright the outline is there (one at
// the top and bottom, none at the sides) and how far down the plate the pixel
// lies (0 to 1); pixels more than `reach` device pixels inside are left alone.
template<typename Shade>
static void paintEdge(Painter& painter, const RectF& plate, double reach, Shade shade)
{
    const Rect device = painter.deviceRect(plate);
    const int width = device.width + 2, height = device.height + 2;
    SkBitmap edge;
    if (!edge.tryAllocPixels(SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType)))
        return;
    edge.eraseColor(SK_ColorTRANSPARENT);
    const double halfWidth = device.width / 2.0, halfHeight = device.height / 2.0;
    const double radius = halfHeight;
    const auto byte = [](double v) { return uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // From the middle, in device pixels, at the pixel's centre.
            const double px = x - 1 + 0.5 - halfWidth, py = y - 1 + 0.5 - halfHeight;
            const double qx = std::abs(px) - (halfWidth - radius), qy = std::abs(py) - (halfHeight - radius);
            double distance, upright;
            if (qx > 0 && qy > 0) {
                const double length = std::hypot(qx, qy);
                distance = length - radius;
                upright = qy * qy / (length * length);
            } else if (qx > qy) {
                distance = qx - radius;
                upright = 0;
            } else {
                distance = qy - radius;
                upright = 1;
            }
            if (distance > 1.5 || -distance > reach)
                continue;
            const EdgePixel shown = shade(distance, upright, (py + halfHeight) / device.height);
            uint8_t* pixel = static_cast<uint8_t*>(edge.getAddr(x, y));
            pixel[0] = pixel[1] = pixel[2] = byte(shown.grey);
            pixel[3] = byte(shown.alpha);
        }
    }
    edge.setImmutable();
    painter.drawImage(edge.asImage().get(), device.x - 1, device.y - 1);
}

// The dark plate's edge: a half point dark line just outside the sides,
// fading to none at the top and bottom as the outline turns, and light just
// inside the top and, brighter, the bottom, fading to none at the sides and
// gone a few points in.
static void paintDarkEdge(Painter& painter, const RectF& plate)
{
    const double scale = painter.scale();
    const double reach = kDarkBottomLight[std::size(kDarkBottomLight) - 1].depth * scale;
    const double lineWidth = 0.5 * scale;
    paintEdge(painter, plate, reach, [scale, lineWidth](double distance, double upright, double down) {
        // The line covers the band from the outline to half a point out; the
        // light, what lies inside the outline.
        const double band
                = std::clamp(std::min(distance + 0.5, lineWidth) - std::max(distance - 0.5, 0.0), 0.0, 1.0);
        const double inside = std::clamp(0.5 - distance, 0.0, 1.0);
        const double depth = std::max(-distance, 0.0) / scale;
        // Laid white adds what is asked to the body over a dark title bar,
        // about 50 near the top and 67 near the bottom.
        const double light = inside * upright
                * (down < 0.5 ? edgeLightAt(kDarkTopLight, depth) / (255 - 50)
                              : edgeLightAt(kDarkBottomLight, depth) / (255 - 67));
        const double dark = band * 0.65 * (1 - upright);
        // The line laid over the light.
        return EdgePixel {light * (1 - dark), dark + light * (1 - dark)};
    });
}

// The light plate's rim: a half point line just inside its outline,
// darkest at the middle of its height.
static void paintLightRim(Painter& painter, const RectF& plate)
{
    const double lineWidth = 0.5 * painter.scale();
    paintEdge(painter, plate, lineWidth + 1, [lineWidth](double distance, double, double down) {
        const double band
                = std::clamp(std::min(distance + 0.5, 0.0) - std::max(distance - 0.5, -lineWidth), 0.0, 1.0);
        const double alpha = down < 0.5 ? 0.1 + (0.32 - 0.1) * down / 0.5 : 0.32 + (0.14 - 0.32) * (down - 0.5) / 0.5;
        return EdgePixel {0, band * alpha};
    });
}

// The plate under a control or a group of them: barely apart from the
// window, lit a little in its middle. In the light it is lit along the inside
// of its top and bottom edges, has a hairline rim darkest at the sides and a
// faint shadow under it; in the dark it takes paintDarkEdge's edge and no
// shadow. `overlay` darkens it, or lightens it in dark mode, by that alpha.
static void paintPlateBody(Painter& painter, const RectF& plate, bool dark, double overlay)
{
    const double radius = plate.height / 2;
    const Color white = Color::rgb(0xFFFFFF);
    const Color black = Color::rgb(0x000000);
    const auto vertical = [&plate](std::vector<GradientStop> stops) {
        return LinearGradient {{plate.x, plate.y}, {plate.x, plate.bottom()}, std::move(stops)};
    };

    if (!dark)
        painter.drawOuterShadow(plate, radius, Corners::Circular, shadowSigma(plate.height), black.withAlpha(0.04),
                                {0, shadowOffset(plate.height)});

    // Filled square inside the round clip, so the outline's pixels take its
    // coverage once.
    painter.save();
    painter.clipRoundedRect(plate, radius);
    // Lightest in the middle of its lower half.
    const double body = dark ? 0.25 : 1.0;
    painter.fillRoundedRect(plate, 0,
                            LinearGradient {{plate.x, plate.y},
                                            {plate.right(), plate.y},
                                            {{0, white.withAlpha(body * 0.04)},
                                             {0.5, white.withAlpha(body * 0.2)},
                                             {1, white.withAlpha(body * 0.04)}}});
    painter.fillRoundedRect(plate, 0,
                            vertical({{0.3, white.withAlpha(0)}, {0.75, white.withAlpha(body * 0.32)}}));
    if (!dark) {
        painter.fillRoundedRect(plate, 0,
                                vertical({{0, white},
                                          {0.035, white},
                                          {0.07, white.withAlpha(0.5)},
                                          {0.12, white.withAlpha(0.25)},
                                          {0.25, white.withAlpha(0)},
                                          {0.85, white.withAlpha(0)},
                                          {0.93, white.withAlpha(0.3)},
                                          {0.955, white.withAlpha(0.6)},
                                          {0.97, white},
                                          {1, white}}));
    }
    if (overlay > 0) {
        const Color tint = dark ? white : black;
        painter.fillRoundedRect(plate, 0, vertical({{0, tint.withAlpha(overlay)}, {1, tint.withAlpha(overlay)}}));
    }
    painter.restore();

    if (dark)
        paintDarkEdge(painter, plate);
    else
        paintLightRim(painter, plate);
}

// `bounds` on whole device pixels, in logical coordinates.
static RectF snappedPlate(Painter& painter, const RectF& bounds)
{
    const double scale = painter.scale();
    const Rect device = painter.snappedRect(bounds);
    const PointF offset = painter.offset();
    return {device.x / scale - offset.x, device.y / scale - offset.y, device.width / scale, device.height / scale};
}

static void paintPlate(Painter& painter, const RectF& bounds, const State& state)
{
    // A square of whole device pixels, so the plate comes out round.
    const double side = sideOf(bounds);
    const RectF plate = snappedPlate(painter, {bounds.x + std::round((bounds.width - side) / 2),
                                               bounds.y + std::round((bounds.height - side) / 2), side, side});
    const Rect device = painter.deviceRect(plate);

    painter.save();
    painter.multiplyOpacity(state.enabled ? 1.0 : 0.35);
    const bool lit = state.enabled && (state.hovered || state.pressed);
    paintPlateBody(painter, plate, state.dark, lit ? (state.pressed ? 0.08 : 0.04) : 0);

    const Color ink = inkColor(state.dark, state.active || state.hovered);
    if (state.artwork == "dialog-close") {
        sk_sp<SkImage> image = IconLoader::instance().glyph("window-control-dialog-close", device.width, ink);
        painter.drawImage(image.get(), device.x, device.y);
    } else if (!state.artwork.empty()) {
        const double artwork = scaled(kArtwork, side);
        const Rect icon = painter.snappedRect({bounds.x + std::round((bounds.width - artwork) / 2),
                                               bounds.y + std::round((bounds.height - artwork) / 2), artwork,
                                               artwork});
        sk_sp<SkImage> image = IconLoader::instance().glyph("window-control-" + state.artwork, icon.width, ink);
        painter.drawImage(image.get(), icon.x, icon.y);
    }
    painter.restore();
}

static void paintColored(Painter& painter, const RectF& bounds, const State& state)
{
    const Color tint = ColorProvider::forMode(state.dark).get(ColorId::HeaderButtonTint);
    const auto centred = [&](double side) {
        return painter.snappedRect({bounds.x + std::round((bounds.width - side) / 2),
                                    bounds.y + std::round((bounds.height - side) / 2), side, side});
    };
    const double side = sideOf(bounds);
    const Rect plate = centred(scaled(kColoredPlate, side));
    // Coloured while the window is active or the pointer is on the row, grey
    // otherwise.
    Color fill = tint.withAlpha((state.dark ? 0.30 : 0.20) + (state.pressed ? 0.12 : 0));
    Color ink = tint;
    if ((state.active || state.hovered || state.pressed) && state.enabled) {
        const Color base = state.artwork == "close" ? Color::rgb(0xEE5A6A)
                : state.artwork == "minimize" ? Color::rgb(0xF4B63F)
                                              : Color::rgb(0x3CC68A);
        fill = state.pressed ? Color{uint8_t(base.r * 0.8), uint8_t(base.g * 0.8), uint8_t(base.b * 0.8), 255}
                             : base;
        ink = Color::rgb(0x000000).withAlpha(0.6);
    }
    painter.save();
    painter.multiplyOpacity(state.enabled ? 1.0 : 0.35);
    painter.fillDeviceRoundedRect(plate, plate.width / 2.0, fill);
    painter.strokeDeviceRoundedRect(plate, plate.width / 2.0, Color::rgb(0x000000).withAlpha(0.12),
                                    kRim * painter.scale());
    if (state.enabled && !state.artwork.empty() && (state.hovered || state.pressed)) {
        const Rect icon = centred(scaled(kArtwork, side));
        sk_sp<SkImage> image = IconLoader::instance().glyph("window-control-" + state.artwork, icon.width, ink);
        painter.drawImage(image.get(), icon.x, icon.y);
    }
    painter.restore();
}

void paint(Painter& painter, const RectF& bounds, const State& state)
{
    if (state.colored && state.artwork != "dialog-close")
        paintColored(painter, bounds, state);
    else
        paintPlate(painter, bounds, state);
}

Color inkColor(bool dark, bool strong)
{
    return (dark ? Color::rgb(0xFFFFFF) : Color::rgb(0x0C0C0C)).withAlpha(strong ? 1.0 : 0.5);
}

void paintGroupPlate(Painter& painter, const RectF& bounds, bool dark)
{
    paintPlateBody(painter, snappedPlate(painter, bounds), dark, 0);
}

RectF visualRect(const RectF& bounds, const State& state)
{
    if (state.colored && state.artwork != "dialog-close")
        return bounds;
    // A sigma and a half, past which the shadow is under a level; it stays
    // inside the gap to the next control.
    const double side = sideOf(bounds);
    const double reach = 1.5 * shadowSigma(side);
    return bounds.adjusted(-std::ceil(reach), -std::ceil(reach - shadowOffset(side)), std::ceil(reach),
                           std::ceil(reach + shadowOffset(side)));
}

Bitmap render(const State& state, double scale, Bitmap::Format format)
{
    const int side = int(std::lround((kButton + 2 * kShadowMargin) * scale));
    Bitmap bitmap(side, side, format);
    if (bitmap.isNull())
        return bitmap;
    SkBitmap target;
    const SkImageInfo info = SkImageInfo::Make(side, side, skColorType(format), kPremul_SkAlphaType);
    if (!target.installPixels(info, bitmap.bits(), bitmap.stride()))
        return {};
    SkCanvas canvas(target);
    Painter painter(&canvas, scale);
    paint(painter, {kShadowMargin, kShadowMargin, kButton, kButton}, state);
    return bitmap;
}

} // namespace cutefish::ui::window_controls
