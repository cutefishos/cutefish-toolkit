// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

namespace cutefish::ui {

// A window frame the toolkit drew with the GPU: its size in device pixels,
// and the glass panes drawn in it, how many, the device pixels they cover
// and the largest blur among them.
struct DrawnFrame
{
    int width = 0;
    int height = 0;
    int glassPanes = 0;
    double glassPixels = 0;
    double largestBlur = 0;
};

// Called once each such frame's drawing is issued and before it is swapped,
// on the thread that drew it with the frame's context current, so a host can
// put a fence after the frame's work that the swap then submits. One handler
// for the process; null removes it.
using FrameDrawnHandler = void (*)(const DrawnFrame& frame);
void setFrameDrawnHandler(FrameDrawnHandler handler);

} // namespace cutefish::ui
