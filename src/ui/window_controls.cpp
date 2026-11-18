// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/window_controls.h"

#include <algorithm>
#include <cmath>
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

// The plate under a control or a group of them: barely apart from the
// window, lit a little in its middle and along the inside of its top and
// bottom edges, a hairline rim darkest at the sides, and a faint shadow under
// it. `overlay` darkens it, or lightens it in dark mode, by that alpha.
static void paintPlateBody(Painter& painter, const RectF& plate, bool dark, double overlay)
{
    const double radius = plate.height / 2;
    const Color white = Color::rgb(0xFFFFFF);
    const Color black = Color::rgb(0x000000);
    const auto vertical = [&plate](std::vector<GradientStop> stops) {
        return LinearGradient {{plate.x, plate.y}, {plate.x, plate.bottom()}, std::move(stops)};
    };

    painter.drawOuterShadow(plate, radius, Corners::Circular, shadowSigma(plate.height),
                            black.withAlpha(dark ? 0.2 : 0.04), {0, shadowOffset(plate.height)});

    painter.save();
    painter.clipRoundedRect(plate, radius);
    // Lightest in the middle of its lower half.
    const double body = dark ? 0.25 : 1.0;
    painter.fillRoundedRect(plate, radius,
                            LinearGradient {{plate.x, plate.y},
                                            {plate.right(), plate.y},
                                            {{0, white.withAlpha(body * 0.04)},
                                             {0.5, white.withAlpha(body * 0.2)},
                                             {1, white.withAlpha(body * 0.04)}}});
    painter.fillRoundedRect(plate, radius,
                            vertical({{0.3, white.withAlpha(0)}, {0.75, white.withAlpha(body * 0.32)}}));
    const double shine = dark ? 0.3 : 1.0;
    painter.fillRoundedRect(plate, radius,
                            vertical({{0, white.withAlpha(shine)},
                                      {0.035, white.withAlpha(shine)},
                                      {0.07, white.withAlpha(shine * 0.5)},
                                      {0.12, white.withAlpha(shine * 0.25)},
                                      {0.25, white.withAlpha(0)},
                                      {0.85, white.withAlpha(0)},
                                      {0.93, white.withAlpha(shine * 0.3)},
                                      {0.955, white.withAlpha(shine * 0.6)},
                                      {0.97, white.withAlpha(shine)},
                                      {1, white.withAlpha(shine)}}));
    if (overlay > 0) {
        const Color tint = dark ? white : black;
        painter.fillRoundedRect(plate, radius, vertical({{0, tint.withAlpha(overlay)}, {1, tint.withAlpha(overlay)}}));
    }
    painter.restore();

    // One device pixel wide.
    const double hairline = 1 / painter.scale();
    Path rim;
    rim.setFillRule(Path::FillRule::EvenOdd);
    if (plate.width == plate.height) {
        rim.addEllipse(plate);
        rim.addEllipse(plate.adjusted(hairline, hairline, -hairline, -hairline));
    } else {
        rim.addRoundedRect(plate, radius, radius);
        rim.addRoundedRect(plate.adjusted(hairline, hairline, -hairline, -hairline), radius - hairline,
                           radius - hairline);
    }
    const Color edge = dark ? white : black;
    painter.fillPath(rim, vertical({{0, edge.withAlpha(dark ? 0.2 : 0.1)},
                                    {0.5, edge.withAlpha(dark ? 0.12 : 0.32)},
                                    {1, edge.withAlpha(dark ? 0.08 : 0.14)}}));
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
