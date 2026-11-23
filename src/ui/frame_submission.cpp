// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/frame_submission.h"

#include <algorithm>
#include <atomic>

#include "ui/frame_submission_private.h"

namespace cutefish::ui {

static std::atomic<FrameDrawnHandler> drawnHandler {nullptr};
static thread_local DrawnFrame drawing;

void setFrameDrawnHandler(FrameDrawnHandler handler)
{
    drawnHandler.store(handler);
}

void noteGlassPaneDrawn(double devicePixels, double blur)
{
    // Frames drawn without a swap (headless, embedded) never report and so
    // never clear the count.
    if (drawing.glassPanes > (1 << 20))
        drawing = {};
    ++drawing.glassPanes;
    drawing.glassPixels += devicePixels;
    drawing.largestBlur = std::max(drawing.largestBlur, blur);
}

void noteFrameDrawn(int width, int height)
{
    DrawnFrame frame = drawing;
    drawing = {};
    frame.width = width;
    frame.height = height;
    if (const FrameDrawnHandler handler = drawnHandler.load())
        handler(frame);
}

} // namespace cutefish::ui
