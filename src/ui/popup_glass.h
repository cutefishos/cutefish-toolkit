// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>

#include "base/signal.h"
#include "ui/window.h"

namespace cutefish::ui {

// The glass of menus and drop-downs: the dock's material, as
// cutefish-compositor's shell makes it. `tint` is a tint slider's position
// (0 More Clear, 1 More Tinted, 0.5 the default): a grey tint that
// thickens, the lens fading out and the blur growing toward More Tinted.
// `blur` is a blur slider's position (0 Sharper, 1 Softer, 0.3 the
// default) that scales the blur, a tenth to tenfold.
GlassMaterial popupGlass(bool darkMode, double tint, double blur);
// The same at the shell's blur (Appearance/GlassBlur).
GlassMaterial popupGlass(bool darkMode, double tint);
// The same at the shell's tint and blur (Settings > Appearance > Glass
// effect), followed while the program runs; frosted (Appearance/GlassFrosted)
// the heavy frost menus had before, without a lens.
GlassMaterial popupGlass(bool darkMode);
// The shell's tint and blur, and whether its glass is frosted.
double shellGlassTint();
double shellGlassBlur();
bool shellGlassFrosted();
// Counts up with every change of the three, for those who cache a material.
uint64_t shellGlassGeneration();
// Emitted when the tint, the blur or the frosted setting changes.
base::Signal<>& shellGlassTintChanged();
// A window's glass: the dock's material, rim included. Windows that ask for
// the plain blur get it at the shell's tint and blur, unless the shell's
// glass is frosted (Window::setBackgroundBlur).
GlassMaterial windowGlass(bool darkMode, double tint);
// The same tinted with a colour and opacity of the program's own, such as
// the terminal's theme background at its opacity setting, instead of the
// shell's grey; the shell's tint still shapes the lens and its blur still
// applies, and the rim fades as the opacity rises. For
// Window::setBackgroundGlass; re-send it on shellGlassTintChanged().
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
    // The theme and the shell's glass generation the material was sent for.
    bool m_sent = false;
    uint64_t m_theme = 0;
    uint64_t m_generation = 0;
};

} // namespace cutefish::ui
