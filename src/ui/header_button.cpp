// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/header_button.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "include/core/SkImage.h"

#include "ui/icon_loader.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/window.h"

namespace cutefish::ui {

// The artwork is drawn on a 24-pixel grid; any other size would be soft.
static constexpr double kArtwork = 24;
static constexpr double kWindowControlPlate = 14;
static constexpr double kWindowControlArtwork = 14;

HeaderButton::HeaderButton(std::string artwork, double size)
    : m_artwork(std::move(artwork))
    , m_size(size)
{
    setMovesWindow(true);
}

void HeaderButton::setArtwork(std::string artwork)
{
    m_artwork = std::move(artwork);
    requestPaint();
}

void HeaderButton::setGlyph(std::string name, double size)
{
    m_glyph = std::move(name);
    m_glyphSize = size;
    requestPaint();
}

void HeaderButton::setChecked(bool checked)
{
    if (m_checked == checked)
        return;
    m_checked = checked;
    requestPaint();
}

void HeaderButton::setHighlighted(bool highlighted)
{
    if (m_highlighted == highlighted)
        return;
    m_highlighted = highlighted;
    requestPaint();
}

void HeaderButton::setPlateAlphas(PlateAlphas light, PlateAlphas dark)
{
    m_light = light;
    m_dark = dark;
    requestPaint();
}

void HeaderButton::setGlyphColor(std::optional<Color> color)
{
    if (color == m_glyphColor)
        return;
    m_glyphColor = color;
    requestPaint();
}

void HeaderButton::setWindowControl(bool windowControl)
{
    if (m_windowControl == windowControl)
        return;
    m_windowControl = windowControl;
    requestPaint();
}

void HeaderButton::setGroupHovered(bool hovered)
{
    if (m_groupHovered == hovered)
        return;
    m_groupHovered = hovered;
    requestPaint();
}

void HeaderButton::paintWindowControl(Painter& painter)
{
    const Window* window = this->window();
    const bool dark = window ? window->headerDarkMode() : Theme::current().darkMode;
    const Color tint = ColorProvider::forMode(dark).get(ColorId::HeaderButtonTint);
    const RectF bounds = localRect();
    const auto centred = [&](double side) {
        return painter.snappedRect({std::round((bounds.width - side) / 2), std::round((bounds.height - side) / 2),
                                    side, side});
    };
    const Rect plate = centred(kWindowControlPlate);
    const bool pressed = m_pressed || m_checked;
    const bool active = window && window->isActive();
    const std::string artworkName = currentArtwork();
    // Coloured while the window is active, grey otherwise.
    Color fill = tint.withAlpha((dark ? 0.30 : 0.20) + (pressed ? 0.12 : 0));
    Color ink = tint;
    if (active && isEnabled()) {
        const Color base = artworkName == "close" ? Color::rgb(0xEE5A6A)
                : artworkName == "minimize" ? Color::rgb(0xF4B63F)
                                            : Color::rgb(0x3CC68A);
        fill = pressed ? Color{uint8_t(base.r * 0.8), uint8_t(base.g * 0.8), uint8_t(base.b * 0.8), 255} : base;
        ink = Color::rgb(0x000000).withAlpha(0.6);
    }
    painter.save();
    painter.multiplyOpacity(isEnabled() ? 1.0 : 0.35);
    painter.fillDeviceRoundedRect(plate, plate.width / 2.0, fill);
    if (isEnabled() && !artworkName.empty() && (m_hovered || m_groupHovered || pressed)) {
        const Rect icon = centred(kWindowControlArtwork);
        sk_sp<SkImage> image = IconLoader::instance().glyph("window-control-" + artworkName, icon.width, ink);
        painter.drawImage(image.get(), icon.x, icon.y);
    }
    painter.restore();
}

void HeaderButton::paint(Painter& painter)
{
    if (m_windowControl) {
        paintWindowControl(painter);
        return;
    }
    const Window* window = this->window();
    // The header's mode, which a window may set apart from the theme.
    const bool dark = window ? window->headerDarkMode() : Theme::current().darkMode;
    const Color tint = ColorProvider::forMode(dark).get(ColorId::HeaderButtonTint);
    const Rect box = painter.deviceRect(localRect());
    // The icon at whole logical pixels in the button, then snapped onto the
    // device grid as Icon snaps it.
    const double iconSize = m_glyph.empty() ? kArtwork : m_glyphSize;
    const RectF bounds = localRect();
    const Rect icon = painter.snappedRect({std::round((bounds.width - iconSize) / 2),
                                          std::round((bounds.height - iconSize) / 2), iconSize, iconSize});

    // The plate keeps the icon's parity, so both stay centred on the same pixel.
    const bool pressed = m_pressed || m_checked;
    if (isEnabled() && (pressed || m_hovered || m_highlighted)) {
        const PlateAlphas& alphas = dark ? m_dark : m_light;
        const double alpha = pressed ? alphas.pressed : alphas.hovered;
        const int margin = int(std::lround(m_size * 0.1 * painter.scale()));
        int plate = std::min(box.width, box.height) - 2 * margin;
        if ((plate - icon.width) % 2 != 0)
            plate -= 1;
        painter.fillDeviceRoundedRect({icon.x + (icon.width - plate) / 2, icon.y + (icon.height - plate) / 2,
                                       plate, plate},
                                      plate / 2.0, tint.withAlpha(alpha));
    }

    const std::string artworkName = currentArtwork();
    if (artworkName.empty() && m_glyph.empty())
        return;
    const double opacity = !isEnabled() ? 0.35 : (window && window->isActive()) ? 1.0 : 0.6;
    const int side = icon.width;
    const Color ink = m_glyphColor.value_or(tint);
    sk_sp<SkImage> image = m_glyph.empty()
            ? IconLoader::instance().glyph("window-" + artworkName, side, ink)
            : IconLoader::instance().glyph(m_glyph, side, ink);
    painter.save();
    painter.multiplyOpacity(opacity);
    painter.drawImage(image.get(), icon.x, icon.y);
    painter.restore();
}

void HeaderButton::pointerEnter()
{
    m_hovered = true;
    requestPaint();
}

void HeaderButton::pointerLeave()
{
    m_hovered = false;
    requestPaint();
}

void HeaderButton::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    m_pressed = true;
    requestPaint();
}

void HeaderButton::pointerRelease(const PointerEvent& event)
{
    if (!m_pressed)
        return;
    m_pressed = false;
    requestPaint();
    if (localRect().contains(event.position) && onClicked)
        onClicked();
}

} // namespace cutefish::ui
