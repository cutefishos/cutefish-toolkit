// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

namespace cutefish::ui {

// A glass pane went into the frame being drawn on this thread.
void noteGlassPaneDrawn(double devicePixels, double blur);
// The frame drawn on this thread is about to be swapped; its context is
// current.
void noteFrameDrawn(int width, int height);

} // namespace cutefish::ui
