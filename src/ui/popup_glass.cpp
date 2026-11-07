// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/popup_glass.h"

#include "ui/theme.h"

namespace cutefish::ui {

GlassMaterial popupGlass(bool darkMode)
{
    GlassMaterial glass;
    glass.blurSigma = 6;
    glass.displacement = 20;
    glass.chroma = 0.03;
    glass.saturation = 1.2;
    // A white frost; the blur, not the tint, is what keeps text readable.
    if (darkMode) {
        glass.tintRed = 0.17;
        glass.tintGreen = 0.17;
        glass.tintBlue = 0.19;
    } else {
        glass.tintRed = 1;
        glass.tintGreen = 1;
        glass.tintBlue = 1;
    }
    glass.tintAlpha = 0.6;
    return glass;
}

bool PopupGlass::update(Window& window)
{
    if (!window.backgroundGlassAvailable())
        return false;
    const bool darkMode = Theme::current().darkMode;
    if (m_darkMode != darkMode) {
        m_darkMode = darkMode;
        window.setBackgroundGlass(popupGlass(darkMode));
    }
    return true;
}

void PopupGlass::prepare(Window& window)
{
    window.setBackgroundColor(Color::rgb(0x000000).withAlpha(0));
    window.setTranslucentSubpixelText(!window.backgroundGlassAvailable());
}

} // namespace cutefish::ui
