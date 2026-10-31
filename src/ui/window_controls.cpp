// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/window_controls.h"

#include <cmath>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"

#include "ui/bitmap_private.h"
#include "ui/color_provider.h"
#include "ui/icon_loader.h"
#include "ui/painter.h"

namespace cutefish::ui::window_controls {

static constexpr double kPlate = 14;
static constexpr double kArtwork = 14;
static constexpr double kRim = 0.5;

void paint(Painter& painter, const RectF& bounds, const State& state)
{
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
