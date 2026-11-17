// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/window_controls.h"

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

static constexpr double kPlate = 14;
static constexpr double kArtwork = 14;
static constexpr double kRim = 0.5;

// A dialog's close control: a plate barely apart from the window, lit a
// little in its middle and along the inside of its top and bottom edges, a
// hairline rim darkest at the sides, and a faint shadow under it.
static void paintDialogClose(Painter& painter, const RectF& bounds, const State& state)
{
    // A square of whole device pixels, so the plate comes out round.
    const double scale = painter.scale();
    const Rect device = painter.snappedRect({bounds.x + std::round((bounds.width - kButton) / 2),
                                             bounds.y + std::round((bounds.height - kButton) / 2), kButton, kButton});
    const PointF offset = painter.offset();
    const RectF plate {device.x / scale - offset.x, device.y / scale - offset.y, device.width / scale,
                       device.height / scale};
    const double radius = plate.width / 2;
    const Color white = Color::rgb(0xFFFFFF);
    const Color black = Color::rgb(0x000000);
    const auto vertical = [&plate](std::vector<GradientStop> stops) {
        return LinearGradient {{plate.x, plate.y}, {plate.x, plate.bottom()}, std::move(stops)};
    };

    painter.save();
    painter.multiplyOpacity(state.enabled ? 1.0 : 0.35);
    painter.drawOuterShadow(plate, radius, Corners::Circular, plate.width * 0.1,
                            black.withAlpha(state.dark ? 0.2 : 0.04), {0, plate.width * 0.06});

    painter.save();
    painter.clipRoundedRect(plate, radius);
    // Lightest in the middle of its lower half.
    const double body = state.dark ? 0.25 : 1.0;
    painter.fillRoundedRect(plate, radius,
                            LinearGradient {{plate.x, plate.y},
                                            {plate.right(), plate.y},
                                            {{0, white.withAlpha(body * 0.04)},
                                             {0.5, white.withAlpha(body * 0.2)},
                                             {1, white.withAlpha(body * 0.04)}}});
    painter.fillRoundedRect(plate, radius,
                            vertical({{0.3, white.withAlpha(0)}, {0.75, white.withAlpha(body * 0.32)}}));
    const double shine = state.dark ? 0.3 : 1.0;
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
    if (state.enabled && (state.hovered || state.pressed)) {
        const Color overlay = state.dark ? white : black;
        painter.fillRoundedRect(plate, radius, vertical({{0, overlay.withAlpha(state.pressed ? 0.08 : 0.04)},
                                                         {1, overlay.withAlpha(state.pressed ? 0.08 : 0.04)}}));
    }
    painter.restore();

    // One device pixel wide.
    const double hairline = 1 / scale;
    Path rim;
    rim.setFillRule(Path::FillRule::EvenOdd);
    rim.addEllipse(plate);
    rim.addEllipse(plate.adjusted(hairline, hairline, -hairline, -hairline));
    const Color edge = state.dark ? white : black;
    painter.fillPath(rim, vertical({{0, edge.withAlpha(state.dark ? 0.2 : 0.1)},
                                    {0.5, edge.withAlpha(state.dark ? 0.12 : 0.32)},
                                    {1, edge.withAlpha(state.dark ? 0.08 : 0.14)}}));

    const Color ink = state.dark ? white : Color::rgb(0x0C0C0C);
    sk_sp<SkImage> image = IconLoader::instance().glyph("window-control-dialog-close", device.width,
                                                        ink.withAlpha(state.active || state.hovered ? 1.0 : 0.5));
    painter.drawImage(image.get(), device.x, device.y);
    painter.restore();
}

void paint(Painter& painter, const RectF& bounds, const State& state)
{
    if (state.artwork == "dialog-close") {
        paintDialogClose(painter, bounds, state);
        return;
    }
    const Color tint = ColorProvider::forMode(state.dark).get(ColorId::HeaderButtonTint);
    const auto centred = [&](double side) {
        return painter.snappedRect({bounds.x + std::round((bounds.width - side) / 2),
                                    bounds.y + std::round((bounds.height - side) / 2), side, side});
    };
    const Rect plate = centred(kPlate);
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
        const Rect icon = centred(kArtwork);
        sk_sp<SkImage> image = IconLoader::instance().glyph("window-control-" + state.artwork, icon.width, ink);
        painter.drawImage(image.get(), icon.x, icon.y);
    }
    painter.restore();
}

Bitmap render(const State& state, double scale, Bitmap::Format format)
{
    const int side = int(std::lround(kButton * scale));
    Bitmap bitmap(side, side, format);
    if (bitmap.isNull())
        return bitmap;
    SkBitmap target;
    const SkImageInfo info = SkImageInfo::Make(side, side, skColorType(format), kPremul_SkAlphaType);
    if (!target.installPixels(info, bitmap.bits(), bitmap.stride()))
        return {};
    SkCanvas canvas(target);
    Painter painter(&canvas, scale);
    paint(painter, {0, 0, kButton, kButton}, state);
    return bitmap;
}

} // namespace cutefish::ui::window_controls
