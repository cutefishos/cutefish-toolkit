// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/canvas_2d.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

namespace cutefish::ui {

static constexpr uint64_t invalidModifier = 0x00ffffffffffffffULL;

// The GL objects of an image no longer drawn; they go once the canvas's
// context is current again.
struct RetiredTexture
{
    sk_sp<SkImage> image;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};

static std::vector<RetiredTexture>& retiredTextures()
{
    static std::vector<RetiredTexture> retired;
    return retired;
}

static void releaseRetired(GrDirectContext* context)
{
    static const auto destroyImage
            = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto& retired = retiredTextures();
    if (retired.empty())
        return;
    for (RetiredTexture& texture : retired) {
        texture.image.reset();
        if (texture.texture)
            glDeleteTextures(1, &texture.texture);
        if (texture.eglImage != EGL_NO_IMAGE_KHR && destroyImage)
            destroyImage(texture.display, texture.eglImage);
    }
    retired.clear();
    context->resetContext(kTextureBinding_GrGLBackendState);
}

// The alpha channel of formats with an X in its place is padding.
static bool isOpaque(uint32_t fourcc)
{
    return (fourcc & 0xff) == 'X' || ((fourcc >> 8) & 0xff) == 'X';
}

struct DmaBufImage::Data
{
    int width = 0;
    int height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = invalidModifier;
    std::vector<Plane> planes;

    // The texture, for the context it was made in.
    GrDirectContext* context = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    sk_sp<SkImage> image;
    bool failed = false;

    ~Data()
    {
        retire();
        for (const Plane& plane : planes)
            close(plane.fd);
    }

    void retire()
    {
        if (image || texture || eglImage != EGL_NO_IMAGE_KHR)
            retiredTextures().push_back({std::move(image), display, eglImage, texture});
        image = nullptr;
        texture = 0;
        eglImage = EGL_NO_IMAGE_KHR;
        context = nullptr;
    }

    SkImage* textureImage(GrDirectContext* current)
    {
        if (context == current && image)
            return image.get();
        if (failed)
            return nullptr;
        retire();
        if (!import(current)) {
            failed = true;
            retire();
            return nullptr;
        }
        context = current;
        return image.get();
    }

    bool import(GrDirectContext* current)
    {
        static const auto createImage
                = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        static const auto targetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        display = eglGetCurrentDisplay();
        if (!createImage || !targetTexture || display == EGL_NO_DISPLAY || planes.empty() || planes.size() > 4)
            return false;

        static const EGLint planeKeys[4][5] = {
            {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
             EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
             EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
             EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
             EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
        };
        std::vector<EGLint> attributes = {
            EGL_WIDTH, width, EGL_HEIGHT, height, EGL_LINUX_DRM_FOURCC_EXT, EGLint(fourcc),
        };
        for (size_t i = 0; i < planes.size(); ++i) {
            attributes.insert(attributes.end(), {planeKeys[i][0], planes[i].fd, planeKeys[i][1],
                                                 EGLint(planes[i].offset), planeKeys[i][2],
                                                 EGLint(planes[i].stride)});
            if (modifier != invalidModifier)
                attributes.insert(attributes.end(), {planeKeys[i][3], EGLint(modifier & 0xffffffff),
                                                     planeKeys[i][4], EGLint(modifier >> 32)});
        }
        attributes.push_back(EGL_NONE);
        eglImage = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
        if (eglImage == EGL_NO_IMAGE_KHR)
            return false;

        while (glGetError() != GL_NO_ERROR) {
        }
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        targetTexture(GL_TEXTURE_EXTERNAL_OES, eglImage);
        const bool bound = glGetError() == GL_NO_ERROR;
        current->resetContext(kTextureBinding_GrGLBackendState);
        if (!bound)
            return false;

        const GrGLTextureInfo info = {GL_TEXTURE_EXTERNAL_OES, texture, GL_RGBA8};
        const GrBackendTexture backend = GrBackendTextures::MakeGL(width, height, skgpu::Mipmapped::kNo, info);
        image = SkImages::BorrowTextureFrom(current, backend, kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
                                            isOpaque(fourcc) ? kOpaque_SkAlphaType : kPremul_SkAlphaType, nullptr);
        return image != nullptr;
    }
};

DmaBufImage::DmaBufImage() = default;

DmaBufImage::DmaBufImage(int width, int height, uint32_t fourcc, uint64_t modifier, const std::vector<Plane>& planes)
{
    if (width <= 0 || height <= 0 || planes.empty())
        return;
    auto data = std::make_shared<Data>();
    data->width = width;
    data->height = height;
    data->fourcc = fourcc;
    data->modifier = modifier;
    for (const Plane& plane : planes) {
        const int fd = dup(plane.fd);
        if (fd < 0)
            return;
        data->planes.push_back({fd, plane.offset, plane.stride});
    }
    d = std::move(data);
}

DmaBufImage::~DmaBufImage() = default;
DmaBufImage::DmaBufImage(const DmaBufImage&) = default;
DmaBufImage& DmaBufImage::operator=(const DmaBufImage&) = default;

int DmaBufImage::width() const
{
    return d ? d->width : 0;
}

int DmaBufImage::height() const
{
    return d ? d->height : 0;
}

bool Canvas2D::drawDmaBufImage(const DmaBufImage& image, const RectF& rect, bool mirrored)
{
    if (!m_canvas || image.isNull())
        return false;
    GrDirectContext* context = GrAsDirectContext(m_canvas->recordingContext());
    if (!context)
        return false;
    releaseRetired(context);
    SkImage* texture = image.d->textureImage(context);
    if (!texture)
        return false;
    drawImage(texture, rect, mirrored);
    return true;
}

} // namespace cutefish::ui
