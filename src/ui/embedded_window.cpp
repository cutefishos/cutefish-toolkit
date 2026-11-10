// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/embedded_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <utility>
#include <xkbcommon/xkbcommon.h>

#include "include/core/SkColorSpace.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

#include "ui/application.h"
#include "ui/embedded_gpu_private.h"
#include "ui/embedded_host.h"
#include "ui/embedded_platform_window.h"
#include "ui/layer.h"
#include "ui/text_context.h"
#include "ui/view.h"

namespace cutefish::ui {

static uint64_t monotonicNs()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
}

static EmbeddedHost* s_host = nullptr;

EmbeddedPlatformWindow::EmbeddedPlatformWindow(Window* window, EmbeddedPlatformWindow* parent,
                                               const PopupPlacement& placement, bool grab)
    : PlatformWindow(window)
    , m_popup(parent != nullptr)
    , m_placement(placement)
    , m_grab(grab)
{
}

EmbeddedPlatformWindow::~EmbeddedPlatformWindow() = default;

void EmbeddedPlatformWindow::show(int width, int height)
{
    notifyConfigure(width, height, m_states);
    // Configured first, so the host finds it sized and wanting a render.
    if (m_popup && owner && !m_popupShown) {
        m_popupShown = true;
        if (s_host)
            s_host->popupShown(owner);
    }
}

void EmbeddedPlatformWindow::hide()
{
    if (!m_popupShown)
        return;
    m_popupShown = false;
    if (s_host && owner)
        s_host->popupHidden(owner);
}

void EmbeddedPlatformWindow::setPopupPlacement(const PopupPlacement& placement, int, int)
{
    m_placement = placement;
    popupRepositioned();
}

void EmbeddedPlatformWindow::popupRepositioned()
{
    if (m_popupShown && s_host && owner)
        s_host->popupRepositioned(owner);
}

void EmbeddedPlatformWindow::activate()
{
    if (s_host && owner)
        s_host->activationRequested(owner);
}

void EmbeddedPlatformWindow::setBlur(const RectF& rect, double radius)
{
    if (rect == m_blurRect && radius == m_blurRadius)
        return;
    m_blurRect = rect;
    m_blurRadius = radius;
    if (s_host && owner)
        s_host->blurChanged(owner);
}

void EmbeddedPlatformWindow::setBlurShape(const Path& path)
{
    if (path == m_blurShape)
        return;
    m_blurShape = path;
    if (s_host && owner)
        s_host->blurChanged(owner);
}

// As over text-input-v3: only changes go out, and the cause
// stays with the input method until a state is sent.
void EmbeddedPlatformWindow::setTextInput(const TextInputState* state)
{
    if (state ? (m_textInput && *m_textInput == *state) : !m_textInput)
        return;
    if (state)
        m_textInput = *state;
    else
        m_textInput.reset();
    const bool fromInputMethod = inputMethodCause;
    inputMethodCause = false;
    if (s_host && owner)
        s_host->textInputChanged(owner, state, fromInputMethod);
}

bool EmbeddedPlatformWindow::startDrag(DragSource source)
{
    return s_host && owner && s_host->startDrag(owner, std::move(source));
}

void EmbeddedPlatformWindow::setCursorShape(CursorShape shape)
{
    if (onCursorShape)
        onCursorShape(shape);
}

void EmbeddedPlatformWindow::requestRender()
{
    if (m_renderRequested)
        return;
    m_renderRequested = true;
    if (onRenderRequested)
        onRenderRequested();
}

bool EmbeddedPlatformWindow::present(SizeF size, double scale, bool opaque, const PaintFunction& paint)
{
    const int width = int(std::lround(size.width * scale));
    const int height = int(std::lround(size.height * scale));
    if (width <= 0 || height <= 0)
        return false;
    // Glyphs over a translucent frame stay greyscale.
    const SkSurfaceProps properties = Application::instance()->text().surfaceProps(opaque);

    if (m_gpuTarget) {
        if (m_gpuTarget->width != width || m_gpuTarget->height != height)
            return false;
        const bool holdsFrame = m_gpuTarget->keepsLastFrame && m_lastTexture == m_gpuTarget->texture;
        // A texture kept since the last frame is the same GL object, so its
        // framebuffer is too; any other may reuse a deleted texture's name.
        if (!holdsFrame || !m_gpuSurface || m_gpuSurface->recordingContext() != m_gpuTarget->context
            || m_gpuSurface->width() != width || m_gpuSurface->height() != height || m_gpuSurfaceOpaque != opaque) {
            m_gpuSurface.reset();
            GrGLTextureInfo info;
            info.fTarget = 0x0DE1; // GL_TEXTURE_2D
            info.fID = m_gpuTarget->texture;
            info.fFormat = 0x8058; // GL_RGBA8
            const GrBackendTexture texture = GrBackendTextures::MakeGL(width, height, skgpu::Mipmapped::kNo, info);
            // Top-left origin: rows lie in the texture as an upload of the CPU frame would.
            m_gpuSurface = SkSurfaces::WrapBackendTexture(m_gpuTarget->context, texture, kTopLeft_GrSurfaceOrigin, 0,
                                                          kRGBA_8888_SkColorType, nullptr, &properties);
            m_gpuSurfaceOpaque = opaque;
        }
        if (!m_gpuSurface)
            return false;
        paint(m_gpuSurface->getCanvas(), holdsFrame ? 1 : 0);
        // Submitted once the render, or the host's batch, ends.
        m_gpuTarget->context->flush(m_gpuSurface.get());
        m_lastTexture = m_gpuTarget->texture;
        m_holdsFrame = false;
        return true;
    }

    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        m_pixels.assign(size_t(width) * size_t(height) * 4, 0);
        m_holdsFrame = false;
    }
    const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, m_pixels.data(), size_t(width) * 4,
                                                            &properties);
    if (!surface)
        return false;
    paint(surface->getCanvas(), m_holdsFrame ? 1 : 0);
    m_holdsFrame = true;
    m_lastTexture = 0;
    m_gpuSurface.reset();
    return true;
}

EmbeddedWindow::EmbeddedWindow()
{
    connectPlatform();
}

EmbeddedWindow::EmbeddedWindow(Window* parent, const PopupPlacement& placement, bool grab)
    : Window(parent, placement, grab)
{
    connectPlatform();
}

void EmbeddedWindow::setHost(EmbeddedHost* host)
{
    s_host = host;
}

EmbeddedHost* EmbeddedWindow::host()
{
    return s_host;
}

void EmbeddedWindow::connectPlatform()
{
    EmbeddedPlatformWindow* embedded = platform();
    if (!embedded) {
        fprintf(stderr, "EmbeddedWindow needs Application::Platform::Embedded\n");
        std::abort();
    }
    embedded->owner = this;
    // The shell's references at 1.0 match the unrounded layout: far fewer
    // pixels off than whole-pixel placement on every surface measured.
    m_hintedTextLayout = false;
    // What the host composites it over shows through.
    setBackgroundColor(Color{0, 0, 0, 0});
    embedded->onRenderRequested = [this] {
        if (onRenderRequested)
            onRenderRequested();
    };
    embedded->onCursorShape = [this](CursorShape shape) {
        if (shape == m_reportedShape)
            return;
        m_reportedShape = shape;
        if (onCursorShapeChanged)
            onCursorShapeChanged(shape);
    };
}

EmbeddedWindow::~EmbeddedWindow()
{
    // The host hears of these while the window is still whole.
    for (Window* popup : std::vector<Window*>(m_popups))
        popup->hide();
    if (EmbeddedPlatformWindow* embedded = platform()) {
        embedded->hide();
        embedded->owner = nullptr;
        embedded->onRenderRequested = nullptr;
        embedded->onCursorShape = nullptr;
    }
}

EmbeddedPlatformWindow* EmbeddedWindow::platform() const
{
    return m_platform && m_platform->isEmbedded() ? static_cast<EmbeddedPlatformWindow*>(m_platform.get())
                                                  : nullptr;
}

void EmbeddedWindow::setSize(SizeF size)
{
    m_shown = true;
    m_configured = true;
    if (size == m_size)
        return;
    m_size = size;
    m_layoutNeeded = true;
    damageWhole();
    scheduleFrame();
}

void EmbeddedWindow::setKeyboardFocus(bool focused)
{
    if (focused != m_keyboardFocus)
        handleKeyboardFocus(focused);
}

void EmbeddedWindow::setActive(bool active)
{
    if (active == m_states.activated)
        return;
    m_states.activated = active;
    Application::instance()->scheduleActiveUpdate();
    if (onStatesChanged)
        onStatesChanged();
}

EmbeddedWindow* EmbeddedWindow::popupParent() const
{
    return dynamic_cast<EmbeddedWindow*>(parentWindow());
}

const PopupPlacement& EmbeddedWindow::popupPlacement() const
{
    return platform()->placement();
}

bool EmbeddedWindow::isGrabbingPopup() const
{
    return platform()->isPopup() && platform()->grabs();
}

void EmbeddedWindow::setPopupGeometry(const RectF& geometry)
{
    m_popupGeometry = geometry;
    if (geometry.width > 0 && geometry.height > 0)
        setSize({geometry.width, geometry.height});
}

void EmbeddedWindow::requestClose()
{
    handleCloseRequested();
}

void EmbeddedWindow::closePopups()
{
    const std::vector<Window*> popups = m_popups;
    for (auto it = popups.rbegin(); it != popups.rend(); ++it) {
        // Closing one may have closed or destroyed others.
        if (std::find(m_popups.begin(), m_popups.end(), *it) == m_popups.end() || (*it)->isHidden())
            continue;
        if (auto* popup = dynamic_cast<EmbeddedWindow*>(*it)) {
            popup->closePopups();
            popup->requestClose();
        }
        // Off screen whatever its owner did, as a compositor unmaps a
        // dismissed popup.
        if (std::find(m_popups.begin(), m_popups.end(), *it) != m_popups.end())
            (*it)->hide();
    }
}

RectF EmbeddedWindow::blurRect() const
{
    return platform()->blurRect();
}

double EmbeddedWindow::blurRadius() const
{
    return platform()->blurRadius();
}

const Path& EmbeddedWindow::blurShape() const
{
    return platform()->blurShape();
}

void EmbeddedWindow::updateLayout()
{
    if (layoutIfNeeded() && m_pointerInside)
        updateHover();
}

bool EmbeddedWindow::needsRender() const
{
    return m_configured && m_contentView && (m_needsFrame || m_layoutNeeded || m_hoverStale || hasTickingClients());
}

void EmbeddedWindow::beginFrame(uint64_t frameTimeNs)
{
    if (!m_configured || !m_contentView)
        return;
    m_begunFrameNs = frameTimeNs ? frameTimeNs : monotonicNs();
    prepareFrame(m_begunFrameNs);
}

bool EmbeddedWindow::renderFrameAt(uint64_t frameTimeNs)
{
    const uint64_t begun = std::exchange(m_begunFrameNs, 0);
    if (begun && (!frameTimeNs || frameTimeNs == begun))
        return renderFrame(begun, true);
    return renderFrame(frameTimeNs ? frameTimeNs : monotonicNs());
}

const EmbeddedWindow::Frame& EmbeddedWindow::render(uint64_t frameTimeNs)
{
    EmbeddedPlatformWindow* embedded = platform();
    embedded->clearRenderRequest();
    m_frame.damage.clear();
    if (!m_configured || !m_contentView || !renderFrameAt(frameTimeNs))
        return m_frame;
    m_frame.pixels = embedded->pixels();
    m_frame.width = embedded->width();
    m_frame.height = embedded->height();
    m_frame.stride = size_t(embedded->width()) * 4;
    finishFrame();
    return m_frame;
}

const EmbeddedWindow::Frame* EmbeddedWindow::render(EmbeddedGpu& gpu, const GpuTexture& texture,
                                                    uint64_t frameTimeNs)
{
    if (!m_configured || !m_contentView || texture.width != pixelWidth() || texture.height != pixelHeight())
        return nullptr;
    EmbeddedGpuScope scope(gpu);
    if (!scope.isValid())
        return nullptr;
    EmbeddedPlatformWindow* embedded = platform();
    const EmbeddedPlatformWindow::GpuTarget target{scope.context(), texture.id, texture.width, texture.height,
                                                   texture.keepsLastFrame};
    embedded->setGpuTarget(&target);
    embedded->clearRenderRequest();
    const bool rendered = renderFrameAt(frameTimeNs);
    embedded->setGpuTarget(nullptr);
    if (!rendered)
        return nullptr;
    m_frame.pixels = nullptr;
    m_frame.width = texture.width;
    m_frame.height = texture.height;
    m_frame.stride = 0;
    m_frame.damage.clear();
    finishFrame();
    return &m_frame;
}

void EmbeddedWindow::finishFrame()
{
    if (!m_frameDamage->history.empty()) {
        for (SkRegion::Iterator it(m_frameDamage->history.front()); !it.done(); it.next()) {
            const SkIRect& rect = it.rect();
            m_frame.damage.push_back({rect.x(), rect.y(), rect.width(), rect.height()});
        }
    }
    // Running animations want the next frame as well.
    if (m_needsFrame || m_hoverStale || hasTickingClients())
        platform()->requestRender();
}

int EmbeddedWindow::pixelWidth() const
{
    return int(std::lround(m_size.width * m_scale));
}

int EmbeddedWindow::pixelHeight() const
{
    return int(std::lround(m_size.height * m_scale));
}

void EmbeddedWindow::pointerEnter(PointF position)
{
    if (m_hidden)
        return;
    handlePointerEnter(position.x, position.y);
}

void EmbeddedWindow::pointerMove(PointF position)
{
    if (m_hidden)
        return;
    if (!m_pointerInside)
        handlePointerEnter(position.x, position.y);
    else
        handlePointerMotion(position.x, position.y);
}

void EmbeddedWindow::pointerLeave()
{
    if (!m_pointerInside)
        return;
    handlePointerLeave();
    // Nothing in the window asks for a shape any more.
    if (m_reportedShape != CursorShape::Default) {
        m_reportedShape = CursorShape::Default;
        if (onCursorShapeChanged)
            onCursorShapeChanged(m_reportedShape);
    }
}

void EmbeddedWindow::pointerButton(MouseButton button, bool pressed, uint32_t modifiers)
{
    if (m_hidden)
        return;
    handlePointerButton(button, pressed, modifiers);
}

void EmbeddedWindow::scroll(const ScrollEvent& event)
{
    if (m_hidden)
        return;
    handlePointerScroll(event);
}

void EmbeddedWindow::key(const KeyEvent& event, bool pressed)
{
    if (m_hidden)
        return;
    KeyEvent normalized = event;
    normalized.key = event.key == XKB_KEY_ISO_Left_Tab ? XKB_KEY_Tab : xkb_keysym_to_lower(event.key);
    // Control characters are what Ctrl turns letters into, not text.
    if (normalized.text.size() == 1) {
        const auto first = static_cast<unsigned char>(normalized.text[0]);
        if (first < 0x20 || first == 0x7f)
            normalized.text.clear();
    }
    handleKey(normalized, pressed);
}

void EmbeddedWindow::textInput(const TextInputEvent& event)
{
    if (m_hidden)
        return;
    platform()->inputMethodCause = true;
    handleTextInput(event);
}

void EmbeddedWindow::touchDown(int32_t id, PointF position)
{
    if (m_hidden)
        return;
    handleTouchDown(id, position.x, position.y);
}

void EmbeddedWindow::touchMotion(int32_t id, PointF position)
{
    handleTouchMotion(id, position.x, position.y);
}

void EmbeddedWindow::touchUp(int32_t id)
{
    handleTouchUp(id);
}

void EmbeddedWindow::touchCancel()
{
    handleTouchCancel();
}

void EmbeddedWindow::gesture(const GestureEvent& event)
{
    if (m_hidden)
        return;
    handleGesture(event);
}

uint32_t EmbeddedWindow::dragEnter(PointF position, DropOffer* offer, uint32_t modifiers)
{
    if (m_hidden)
        return NoAction;
    handleDragEnter(position.x, position.y, offer, modifiers);
    return m_dropAction;
}

uint32_t EmbeddedWindow::dragMotion(PointF position, uint32_t modifiers)
{
    return handleDragMotion(position.x, position.y, modifiers);
}

void EmbeddedWindow::dragLeave()
{
    handleDragLeave();
}

uint32_t EmbeddedWindow::drop()
{
    return handleDrop();
}

} // namespace cutefish::ui
