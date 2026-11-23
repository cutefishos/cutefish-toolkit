// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

namespace cutefish::ui {

class Window;

// A window frame the toolkit drew with the GPU: the window, its size in
// device pixels, and whether it drew into a texture of its own (see
// setBackdropWindowsOffscreen).
struct DrawnFrame
{
    const Window* window = nullptr;
    int width = 0;
    int height = 0;
    bool offscreen = false;
};

// Called once each such frame's drawing is issued and before it is swapped,
// on the thread that drew it with the frame's context current, so a host can
// put a fence after the frame's work that the swap then submits. One handler
// for the process; null removes it.
using FrameDrawnHandler = void (*)(const DrawnFrame& frame);
void setFrameDrawnHandler(FrameDrawnHandler handler);

// Windows with views that read what lies under them (underlays, backdrops)
// draw into a texture of their own, which then goes to the window's buffer
// whole, so they read a texture rather than the window's own framebuffer;
// false draws them straight into the buffer, as other windows do. True by
// default.
void setBackdropWindowsOffscreen(bool offscreen);
bool backdropWindowsOffscreen();

} // namespace cutefish::ui
