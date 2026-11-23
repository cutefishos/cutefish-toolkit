// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/frame_submission.h"

#include <atomic>

#include "ui/frame_submission_private.h"

namespace cutefish::ui {

static std::atomic<FrameDrawnHandler> drawnHandler {nullptr};
static std::atomic<bool> offscreenBackdrops {true};

void setFrameDrawnHandler(FrameDrawnHandler handler)
{
    drawnHandler.store(handler);
}

void setBackdropWindowsOffscreen(bool offscreen)
{
    offscreenBackdrops.store(offscreen);
}

bool backdropWindowsOffscreen()
{
    return offscreenBackdrops.load();
}

void noteFrameDrawn(const Window* window, int width, int height, bool offscreen)
{
    if (const FrameDrawnHandler handler = drawnHandler.load())
        handler({window, width, height, offscreen});
}

} // namespace cutefish::ui
