// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "gpu/gl_interface.h"

#include "include/gpu/ganesh/gl/GrGLExtensions.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h"

namespace cutefish::ui::gpu {

void removeInPlaceDestinationReads(GrGLExtensions& extensions)
{
    // Frames read in place through these go with GPU resets on some
    // drivers, so Skia takes the copying path it takes where they are missing.
    for (const char* name : {"GL_EXT_shader_framebuffer_fetch", "GL_NV_shader_framebuffer_fetch",
                             "GL_ARM_shader_framebuffer_fetch", "GL_KHR_blend_equation_advanced_coherent",
                             "GL_NV_blend_equation_advanced_coherent"})
        extensions.remove(name);
}

sk_sp<const GrGLInterface> makeGLInterface()
{
    sk_sp<const GrGLInterface> made = GrGLInterfaces::MakeEGL();
    if (!made)
        return nullptr;
    // Each call assembles a new interface, held nowhere else yet.
    removeInPlaceDestinationReads(const_cast<GrGLInterface*>(made.get())->fExtensions);
    return made;
}

} // namespace cutefish::ui::gpu
