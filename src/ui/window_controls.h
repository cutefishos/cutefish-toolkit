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
inline constexpr double kSpacing = 1;

struct State
{
    // "close", "minimize", "maximize" or "restore".
    std::string artwork;
    // The header's mode.
    bool dark = false;
    bool active = false;
    bool enabled = true;
    // The pointer over the control or over the row it sits in.
    bool hovered = false;
    bool pressed = false;
};

void paint(Painter& painter, const RectF& bounds, const State& state);

// A control `kButton` logical pixels square, `scale` device pixels to one,
// rendered on its own into a bitmap of round(kButton * scale) pixels.
Bitmap render(const State& state, double scale, Bitmap::Format format = Bitmap::Format::Argb32Premultiplied);

} // namespace window_controls

} // namespace cutefish::ui
