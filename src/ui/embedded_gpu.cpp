// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/embedded_gpu.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cstdio>
#include <algorithm>
#include <cstring>

#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

#include "gpu/gl_interface.h"
#include "ui/embedded_gpu_private.h"

namespace cutefish::ui {

EmbeddedGpu::Private::~Private() = default;

EmbeddedGpu::EmbeddedGpu()
    : d(std::make_unique<Private>())
{
}

EmbeddedGpu::~EmbeddedGpu()
{
    d->batch.reset();
    // An abandoned context frees nothing through GL.
    if (d->context && !d->abandoned && eglGetCurrentContext() != d->egl)
        d->context->abandonContext();
}

std::unique_ptr<EmbeddedGpu> EmbeddedGpu::createForCurrentContext()
{
    const EGLContext current = eglGetCurrentContext();
    if (current == EGL_NO_CONTEXT)
        return nullptr;
    std::unique_ptr<EmbeddedGpu> gpu(new EmbeddedGpu);
    gpu->d->egl = current;
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    gpu->d->es3 = version && std::strncmp(version, "OpenGL ES ", 10) == 0 && version[10] >= '3';
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    gpu->d->externalTextures = extensions && std::strstr(extensions, "GL_OES_EGL_image_external");
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &gpu->d->units);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &gpu->d->attributes);
    // Skia binds no more than a few units; the walk stays short.
    gpu->d->units = std::min(gpu->d->units, 16);
    gpu->d->context = GrDirectContexts::MakeGL(gpu::makeGLInterface());
    if (!gpu->d->context) {
        fprintf(stderr, "cutefish-toolkit: Skia cannot use the host's GL context\n");
        return nullptr;
    }
    return gpu;
}

void EmbeddedGpu::abandon()
{
    if (d->context && !d->abandoned)
        d->context->abandonContext();
    d->abandoned = true;
}

void EmbeddedGpu::release()
{
    if (d->context && !d->abandoned) {
        if (eglGetCurrentContext() == d->egl)
            d->context->releaseResourcesAndAbandonContext();
        else
            d->context->abandonContext();
    }
    d->abandoned = true;
}

bool EmbeddedGpu::isAbandoned() const
{
    return d->abandoned || !d->context || d->context->abandoned();
}

void EmbeddedGpu::purgeResources()
{
    if (!isAbandoned() && eglGetCurrentContext() == d->egl)
        d->context->purgeUnlockedResources(GrPurgeResourceOptions::kAllResources);
}

EmbeddedGpu::CacheUsage EmbeddedGpu::cacheUsage() const
{
    CacheUsage usage;
    if (isAbandoned())
        return usage;
    d->context->getResourceCacheUsage(&usage.resources, &usage.bytes);
    usage.purgeableBytes = d->context->getResourceCachePurgeableBytes();
    return usage;
}

void EmbeddedGpu::performDeferredCleanup(std::chrono::milliseconds unusedFor)
{
    if (!isAbandoned() && eglGetCurrentContext() == d->egl)
        d->context->performDeferredCleanup(unusedFor);
}

void EmbeddedGpu::setCacheLimit(size_t bytes)
{
    if (!isAbandoned())
        d->context->setResourceCacheLimit(bytes);
}

bool EmbeddedGpu::beginBatch()
{
    endBatch();
    d->batch = std::make_unique<EmbeddedGpuScope>(*this);
    if (!d->batch->isValid()) {
        d->batch.reset();
        return false;
    }
    return true;
}

void EmbeddedGpu::endBatch()
{
    d->batch.reset();
}

bool EmbeddedGpu::inBatch() const
{
    return d->batch != nullptr;
}

EmbeddedGpuScope::EmbeddedGpuScope(EmbeddedGpu& gpu)
    : m_gpu(gpu)
    , m_nested(gpu.d->batch != nullptr)
{
    if (gpu.isAbandoned() || eglGetCurrentContext() != gpu.d->egl)
        return;
    m_valid = true;
    if (m_nested)
        return;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_framebuffer);
    if (gpu.d->es3)
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFramebuffer);
    glGetIntegerv(GL_VIEWPORT, m_viewport);
    glGetIntegerv(GL_SCISSOR_BOX, m_scissor);
    m_scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    m_dither = glIsEnabled(GL_DITHER);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, m_clearColor);
    // The host drew since Skia last looked, as Chromium resets its
    // GrContext when another GL user had the shared context.
    gpu.d->context->resetContext();
}

EmbeddedGpuScope::~EmbeddedGpuScope()
{
    if (!m_valid || m_nested)
        return;
    GrDirectContext* context = m_gpu.d->context.get();
    context->flushAndSubmit(GrSyncCpu::kNo);
    if (context->abandoned()) {
        m_gpu.d->abandoned = true;
        return;
    }

    const bool es3 = m_gpu.d->es3;
    glUseProgram(0);
    if (es3)
        glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    // Skia may draw on vertex array 0 itself: instanced attributes there would
    // make the host's plain draws draw nothing.
    for (GLuint attribute = 0; attribute < GLuint(m_gpu.d->attributes); ++attribute) {
        glDisableVertexAttribArray(attribute);
        if (es3)
            glVertexAttribDivisor(attribute, 0);
        glVertexAttribPointer(attribute, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    if (es3) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (GLuint unit = GLuint(m_gpu.d->units); unit-- > 0;) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (m_gpu.d->externalTextures)
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        if (es3)
            glBindSampler(unit, 0);
    }
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    if (es3)
        glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    m_dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);

    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(m_framebuffer));
    if (es3)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(m_readFramebuffer));
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    glScissor(m_scissor[0], m_scissor[1], m_scissor[2], m_scissor[3]);
    m_scissorTest ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

GrDirectContext* EmbeddedGpuScope::context() const
{
    return m_gpu.d->context.get();
}

} // namespace cutefish::ui
