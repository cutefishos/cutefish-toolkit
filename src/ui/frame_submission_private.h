// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

namespace cutefish::ui {

class Window;

// The window's frame drawn on this thread is about to be swapped; its
// context is current.
void noteFrameDrawn(const Window* window, int width, int height, bool offscreen);

} // namespace cutefish::ui
