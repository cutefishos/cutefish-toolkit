// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>

#include "ui/bitmap.h"
#include "ui/geometry.h"

namespace cutefish::ui {

class Painter;

// The close, minimize and maximize controls at a window's start, for the
// toolkit's headers and for frames drawn outside a toolkit window.
namespace window_controls {

inline constexpr double kButton = 22;
inline constexpr double kSpacing = 6;
inline constexpr double kColoredSpacing = 1;
// Room around a control's square for a plate's shadow.
inline constexpr double kShadowMargin = 5;
// A dialog's lone close control sits as far from the top as from the start,
// in a title bar that leaves the same room under it; the title shares its line.
inline constexpr double kDialogCloseInset = 10;
inline constexpr double kDialogHeaderHeight = 2 * kDialogCloseInset + kButton;

struct State
{
    // "close", "minimize", "maximize" or "restore"; "dialog-close" is a
    // dialog's only control. Each is a light plate always showing its glyph.
    std::string artwork;
    // A small dot instead, red, yellow or green while the window is active
    // or the pointer is on the row, its glyph only under the pointer; never
    // for "dialog-close".
    bool colored = false;
    // The header's mode.
    bool dark = false;
    bool active = false;
    bool enabled = true;
    // The pointer over the control, or, when colored, over the row it sits in.
    bool hovered = false;
    bool pressed = false;
};

void paint(Painter& painter, const RectF& bounds, const State& state);
// What paint may cover for a control in `bounds`: a plate's shadow reaches
// past them.
RectF visualRect(const RectF& bounds, const State& state);

// The plates' glyph colour; `strong` while the window is active or the
// pointer is on the control.
Color inkColor(bool dark, bool strong);
// One plate across `bounds`, round at its ends, for buttons that share it;
// its shadow reaches as far past them as a control's does.
void paintGroupPlate(Painter& painter, const RectF& bounds, bool dark);

// A control `kButton` logical pixels square, `scale` device pixels to one,
// rendered on its own with `kShadowMargin` all round, into a bitmap of
// round((kButton + 2 * kShadowMargin) * scale) pixels.
Bitmap render(const State& state, double scale, Bitmap::Format format = Bitmap::Format::Argb32Premultiplied);

} // namespace window_controls

} // namespace cutefish::ui
