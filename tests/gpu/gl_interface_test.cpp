// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h"
#include "src/gpu/ganesh/GrCaps.h"
#include "src/gpu/ganesh/GrContextThreadSafeProxyPriv.h"
#include "src/gpu/ganesh/GrShaderCaps.h"

#include "gpu/gl_interface.h"

using namespace cutefish::ui;

namespace {

const char* const kInPlaceReads[] = {"GL_EXT_shader_framebuffer_fetch", "GL_KHR_blend_equation_advanced_coherent"};

// A surfaceless GLES 3 context, current while the test runs.
class GlesContext : public testing::Test
{
protected:
    void SetUp() override
    {
        const auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        ASSERT_NE(getPlatformDisplay, nullptr);
        m_display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        ASSERT_NE(m_display, EGL_NO_DISPLAY);
        ASSERT_TRUE(eglInitialize(m_display, nullptr, nullptr));
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        m_context = eglCreateContext(m_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
        ASSERT_NE(m_context, EGL_NO_CONTEXT);
        ASSERT_TRUE(eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context));
    }

    void TearDown() override
    {
        if (m_display == EGL_NO_DISPLAY)
            return;
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_context != EGL_NO_CONTEXT)
            eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }

    // Skia's own interface, telling it the driver has the in-place reads.
    static sk_sp<GrGLInterface> withInPlaceReads()
    {
        sk_sp<const GrGLInterface> made = GrGLInterfaces::MakeEGL();
        if (!made)
            return nullptr;
        sk_sp<GrGLInterface> result(const_cast<GrGLInterface*>(made.release()));
        for (const char* name : kInPlaceReads)
            result->fExtensions.add(name);
        return result;
    }

    static const GrCaps* capsOf(GrDirectContext* context)
    {
        return context->threadSafeProxy()->priv().caps();
    }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
};

} // namespace

TEST_F(GlesContext, TheToolkitsInterfaceHasNoInPlaceReads)
{
    const sk_sp<const GrGLInterface> interface = gpu::makeGLInterface();
    ASSERT_NE(interface, nullptr);
    for (const char* name : kInPlaceReads)
        EXPECT_FALSE(interface->hasExtension(name)) << name;
}

TEST_F(GlesContext, SkiaReadsNothingInPlaceWithoutThem)
{
    // Where the driver has them, Skia takes them.
    const sk_sp<GrDirectContext> with = GrDirectContexts::MakeGL(withInPlaceReads());
    ASSERT_NE(with, nullptr);
    EXPECT_TRUE(capsOf(with.get())->shaderCaps()->fFBFetchSupport);
    EXPECT_TRUE(capsOf(with.get())->advancedCoherentBlendEquationSupport());

    sk_sp<GrGLInterface> interface = withInPlaceReads();
    ASSERT_NE(interface, nullptr);
    gpu::removeInPlaceDestinationReads(interface->fExtensions);
    const sk_sp<GrDirectContext> without = GrDirectContexts::MakeGL(interface);
    ASSERT_NE(without, nullptr);
    EXPECT_FALSE(capsOf(without.get())->shaderCaps()->fFBFetchSupport);
    EXPECT_FALSE(capsOf(without.get())->advancedCoherentBlendEquationSupport());
}
