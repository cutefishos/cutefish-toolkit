// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "include/core/SkRefCnt.h"

class GrGLExtensions;
struct GrGLInterface;

namespace cutefish::ui::gpu {

// Skia's GL interface for the current EGL context, every context the
// toolkit draws with made through it.
sk_sp<const GrGLInterface> makeGLInterface();

// Takes out the extensions that let Skia's shaders read the pixels they
// draw over in place, so it copies them out first instead.
void removeInPlaceDestinationReads(GrGLExtensions& extensions);

} // namespace cutefish::ui::gpu
