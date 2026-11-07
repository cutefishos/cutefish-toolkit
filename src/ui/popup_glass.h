// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <optional>

#include "base/signal.h"
#include "ui/window.h"

namespace cutefish::ui {

// The glass of menus and drop-downs: the dock's material at `tint`, the
// position of a Glass effect slider (0 More Clear, 1 More Tinted, 0.5 the
// default): clear and refracting at 0, a frosted grey at 1.
GlassMaterial popupGlass(bool darkMode, double tint, double blur);
// The same at the shell's Blur intensity (Appearance/GlassBlur).
GlassMaterial popupGlass(bool darkMode, double tint);
// The same at the Appearance/GlassTint of the shell's settings
// (Settings > Appearance > Glass effect), followed while the program runs.
GlassMaterial popupGlass(bool darkMode);
// That tint, and the blur (0 sharpest to 1 softest).
double shellGlassTint();
double shellGlassBlur();
// Counts up with every change of either, for those who cache the material.
uint64_t shellGlassGeneration();
// Emitted when the tint or the blur in the file changes.
base::Signal<>& shellGlassTintChanged();
// A window's glass: the dock's material, rim included. Windows that ask for
// the plain blur get it at the shell's tint (Window::setBackgroundBlur).
GlassMaterial windowGlass(bool darkMode, double tint);
// The same tinted with a colour and opacity of the program's own, such as
// the terminal's theme background at its opacity setting, instead of the
// shell's tint; the shell's Blur intensity still applies, and the rim fades
// as the opacity rises. For Window::setBackgroundGlass; re-send it on
// shellGlassTintChanged().
GlassMaterial windowGlass(const Color& tint, double opacity);

// A popup's glass, kept in the theme's colours.
class PopupGlass
{
public:
    // Asks the window for the glass when the theme changed since; true when
    // the compositor draws it, and then the popup paints no fill of its own.
    bool update(Window& window);
    // A popup window's transparent background, before its first paint. Over
    // the glass the text sits on transparent pixels, where one alpha per
    // pixel cannot carry per-channel coverage, so it is greyscale there.
    static void prepare(Window& window);

private:
    std::optional<bool> m_darkMode;
    double m_tint = -1;
};

} // namespace cutefish::ui
