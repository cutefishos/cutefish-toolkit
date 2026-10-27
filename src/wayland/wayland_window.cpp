// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "wayland/wayland_window.h"

#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <wayland-egl.h>

#include <cutefish-main-menu-v1-client-protocol.h>

#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

#include "gpu/egl_context.h"
#include "ui/application.h"
#include "ui/text_context.h"
#include "wayland/connection.h"
#include "wayland/data_device.h"
#include "wayland/main_menu.h"
#include "wayland/text_input.h"

namespace cutefish::ui::wayland {

WaylandWindow::WaylandWindow(Window* window)
    : WaylandWindow(window, Application::instance()->connection(), Application::instance()->gpu())
{
}

WaylandWindow::WaylandWindow(Window* window, Connection& connection, gpu::EglContext& context)
    : PlatformWindow(window)
    , m_connection(&connection)
    , m_gpu(&context)
    , m_shell(std::make_unique<ShellSurface>(connection, static_cast<ShellSurface::Delegate*>(this)))
{
}

WaylandWindow::WaylandWindow(Window* window, StatusItemClient* statusItem)
    : PlatformWindow(window)
    , m_connection(&Application::instance()->connection())
    , m_statusItem(statusItem)
    , m_gpu(&Application::instance()->gpu())
    , m_shell(std::make_unique<ShellSurface>(*m_connection, static_cast<ShellSurface::Delegate*>(this),
                                             ShellSurface::StatusItemRole {}))
{
}

WaylandWindow::~WaylandWindow()
{
    for (SubsurfaceClient* client : std::vector<SubsurfaceClient*>(m_subsurfaces))
        client->parentGone();
    auto& gpu = *m_gpu;
    if (m_eglSurface != EGL_NO_SURFACE) {
        gpu.makeCurrent(m_eglSurface);
        m_surface.reset();
        gpu.destroySurface(m_eglSurface);
    }
    if (m_eglWindow)
        wl_egl_window_destroy(m_eglWindow);
}

void WaylandWindow::setTitle(const std::string& title)
{
    if (m_shell)
        m_shell->setTitle(title);
}

void WaylandWindow::setAppId(const std::string& appId)
{
    if (m_shell)
        m_shell->setAppId(appId);
}

void WaylandWindow::setMinimumSize(int width, int height)
{
    if (m_shell)
        m_shell->setMinimumSize(width, height);
}

WaylandWindow::WaylandWindow(Window* window, WaylandWindow* parent,
                             const PopupPlacement& placement, bool grab)
    : PlatformWindow(window)
    , m_connection(parent->m_connection)
    , m_gpu(parent->m_gpu)
    , m_parent(parent)
    , m_placement(placement)
    , m_grab(grab)
{
}

void WaylandWindow::setMaximumSize(int width, int height)
{
    if (m_shell)
        m_shell->setMaximumSize(width, height);
}

void WaylandWindow::setMaximized(bool maximized)
{
    if (m_shell)
        m_shell->setMaximized(maximized);
}

void WaylandWindow::setFullScreen(bool fullScreen)
{
    if (m_shell)
        m_shell->setFullScreen(fullScreen);
}

void WaylandWindow::minimize()
{
    if (m_shell)
        m_shell->minimize();
}

void WaylandWindow::hideApplication()
{
    if (m_shell)
        m_shell->hideApplication();
}

void WaylandWindow::activate()
{
    if (m_shell)
        m_connection->activate(m_shell.get(), Application::instance()->applicationId());
}

void WaylandWindow::activateWithToken(const std::string& token)
{
    if (m_shell)
        m_connection->activateWithToken(m_shell.get(), token);
}

SizeF WaylandWindow::screenSize() const
{
    if (m_shell)
        return m_shell->outputSize();
    return m_connection->primaryOutputSize();
}

void WaylandWindow::exportHandle(std::function<void(const std::string&)> done)
{
    if (m_shell && !m_parent)
        m_connection->exportToplevel(m_shell.get(), std::move(done));
    else
        done({});
}

void WaylandWindow::requestActivationToken(std::function<void(const std::string&)> done)
{
    m_connection->requestActivationToken(m_shell.get(), std::move(done));
}

void WaylandWindow::setTransientParent(PlatformWindow* parent, bool modal)
{
    auto* waylandParent = static_cast<WaylandWindow*>(parent);
    if (m_shell)
        m_shell->setParent(waylandParent ? waylandParent->m_shell.get() : nullptr, modal);
}

void WaylandWindow::setRestoreSize(int width, int height)
{
    if (m_shell)
        m_shell->setRestoreSize(width, height);
}

void WaylandWindow::show(int width, int height)
{
    if (!m_shell && m_parent) {
        m_shell = std::make_unique<ShellSurface>(*m_connection,
                                                 static_cast<ShellSurface::Delegate*>(this),
                                                 m_parent->m_shell.get(), m_placement, width,
                                                 height, m_grab);
        m_shell->setBlur(m_blurRect, m_blurRadius);
        if (!m_blurShape.isEmpty())
            m_shell->setBlurShape(m_blurShape);
        if (m_glass)
            m_shell->setGlass(m_glass);
        if (m_opacity < 1)
            m_shell->setOpacity(m_opacity);
    }
    m_shell->map();
}

void WaylandWindow::setPopupPlacement(const PopupPlacement& placement, int width, int height)
{
    m_placement = placement;
    if (m_shell)
        m_shell->reposition(placement, width, height);
}

void WaylandWindow::setBlur(const RectF& rect, double radius)
{
    m_blurRect = rect;
    m_blurRadius = radius;
    if (m_shell)
        m_shell->setBlur(rect, radius);
}

void WaylandWindow::setBlurShape(const Path& path)
{
    if (path == m_blurShape)
        return;
    m_blurShape = path;
    if (m_shell)
        m_shell->setBlurShape(path);
}

void WaylandWindow::setGlass(const std::optional<GlassMaterial>& material)
{
    if (material == m_glass)
        return;
    m_glass = material;
    if (m_shell)
        m_shell->setGlass(material);
}

bool WaylandWindow::glassAvailable() const
{
    return m_connection->surfaceBlur() != nullptr;
}

void WaylandWindow::setResizeAroundCenter(bool aroundCenter)
{
    if (m_shell)
        m_shell->setResizeAroundCenter(aroundCenter);
}

void WaylandWindow::setSkipTaskbar(bool skip)
{
    if (m_shell)
        m_shell->setSkipTaskbar(skip);
}

void WaylandWindow::setAlwaysOnTop(bool onTop)
{
    if (m_shell)
        m_shell->setAlwaysOnTop(onTop);
}

void WaylandWindow::setRestoreId(const std::string& id)
{
    if (m_shell)
        m_shell->setRestoreId(id);
}

void WaylandWindow::setPlacementAnchor(const std::optional<PlacementAnchor>& anchor)
{
    if (m_shell)
        m_shell->setPlacementAnchor(anchor);
}

bool WaylandWindow::setDesktopRole(const std::string& outputName)
{
    return m_shell && m_shell->setDesktop(outputName);
}

void WaylandWindow::setShellOutput(const std::string& outputName)
{
    if (m_shell)
        m_shell->setShellOutput(outputName);
}

void WaylandWindow::activateShellSurface()
{
    if (m_shell)
        m_shell->activateShellSurface();
}

void WaylandWindow::workArea(const RectF& area)
{
    notifyWorkArea(area);
}

void WaylandWindow::screenPosition(PointF position)
{
    notifyScreenPosition(position);
}

void WaylandWindow::setForeignParent(const std::string& handle, bool modal)
{
    if (m_shell)
        m_shell->setForeignParent(handle, modal);
}

void WaylandWindow::addSubsurface(SubsurfaceClient* client)
{
    m_subsurfaces.push_back(client);
}

void WaylandWindow::removeSubsurface(SubsurfaceClient* client)
{
    std::erase(m_subsurfaces, client);
}

void WaylandWindow::startMove()
{
    if (m_shell)
        m_shell->startMove();
}

void WaylandWindow::setCursorShape(CursorShape shape)
{
    if (m_shell)
        m_shell->connection().setCursorShape(m_shell.get(), shape);
}

void WaylandWindow::setTextInput(const TextInputState* state)
{
    if (!m_shell)
        return;
    if (TextInput* input = m_shell->connection().textInput())
        input->update(m_shell.get(), state);
}

bool WaylandWindow::present(SizeF size, double scale, bool opaque, const PaintFunction& paint)
{
    if (!m_shell)
        return false;
    auto* application = Application::instance();
    auto& gpu = *m_gpu;
    const int width = int(std::lround(size.width * scale));
    const int height = int(std::lround(size.height * scale));
    if (!m_eglWindow) {
        m_eglWindow = wl_egl_window_create(m_shell->surface(), width, height);
        m_eglSurface = gpu.createSurface(m_eglWindow);
    } else if (width != m_bufferWidth || height != m_bufferHeight) {
        // Takes effect with the next buffer, which is the one drawn below.
        wl_egl_window_resize(m_eglWindow, width, height, 0, 0);
        m_surface.reset();
    }
    m_bufferWidth = width;
    m_bufferHeight = height;

    if (m_eglSurface == EGL_NO_SURFACE || !gpu.makeCurrent(m_eglSurface))
        return false;

    if (!m_surface) {
        GrGLFramebufferInfo framebuffer;
        framebuffer.fFBOID = 0;
        framebuffer.fFormat = GL_RGBA8;
        const GrBackendRenderTarget target =
            GrBackendRenderTargets::MakeGL(width, height, 0, gpu.stencilBits(), framebuffer);
        const SkSurfaceProps properties = application->text().surfaceProps();
        m_surface = SkSurfaces::WrapBackendRenderTarget(gpu.skiaContext(), target,
                                                        kBottomLeft_GrSurfaceOrigin,
                                                        kRGBA_8888_SkColorType, nullptr,
                                                        &properties);
        if (!m_surface)
            return false;
    }

    // Asked before drawing: the query fixes which buffer is the back one.
    const int bufferAge = gpu.bufferAge(m_eglSurface);
    const std::vector<Rect> damage = paint(m_surface->getCanvas(), bufferAge);
    gpu.skiaContext()->flushAndSubmit(m_surface.get());

    // Subsurface positions, and the buffers of their new sizes, land with
    // this commit.
    for (SubsurfaceClient* client : std::vector<SubsurfaceClient*>(m_subsurfaces))
        client->parentCommitting();
    m_shell->prepareCommit(int(size.width), int(size.height), opaque);
    gpu.swapBuffers(m_eglSurface, damage, height);
    for (SubsurfaceClient* client : std::vector<SubsurfaceClient*>(m_subsurfaces))
        client->parentCommitted();
    return true;
}

void WaylandWindow::configure(int width, int height, const WindowStates& states)
{
    // A status item's width is its own.
    if (m_statusItem && width <= 0)
        width = int(std::ceil(m_window->size().width));
    notifyConfigure(width, height, states);
}

void WaylandWindow::setBodyShape(const Path& path)
{
    if (m_shell)
        m_shell->setBodyShape(path);
}

void WaylandWindow::setCastsShadow(bool castsShadow)
{
    if (m_shell)
        m_shell->setCastsShadow(castsShadow);
}

void WaylandWindow::setOpacity(double opacity)
{
    m_opacity = opacity;
    if (m_shell)
        m_shell->setOpacity(opacity);
}

bool WaylandWindow::setScreenPosition(PointF position)
{
    return m_shell && m_shell->setScreenPosition(position);
}

void WaylandWindow::setUserResizable(bool resizable)
{
    if (m_shell)
        m_shell->setServerResize(resizable);
}

void WaylandWindow::setMainMenu(Menu* menu)
{
    if (!m_shell || !m_shell->toplevel() || !m_connection->mainMenus())
        return;
    std::shared_ptr<MainMenuMirror> mirror = m_connection->mainMenu(menu);
    if (mirror == m_mainMenu)
        return;
    m_mainMenu = std::move(mirror);
    cutefish_main_menu_manager_v1_set_main_menu(m_connection->mainMenus(), m_shell->surface(),
                                                m_mainMenu ? m_mainMenu->proxy() : nullptr);
}

void WaylandWindow::setStatusItemIdentifier(const std::string& identifier)
{
    if (m_shell)
        m_shell->setStatusItemIdentifier(identifier);
}

void WaylandWindow::setStatusItemHighlight(bool highlighted)
{
    if (m_shell)
        m_shell->setStatusItemHighlight(highlighted);
}

void WaylandWindow::statusItemLimits(int maxWidth)
{
    if (m_statusItem)
        m_statusItem->statusItemLimits(maxWidth);
}

void WaylandWindow::statusItemAppearance(bool dark, uint32_t foreground)
{
    if (m_statusItem)
        m_statusItem->statusItemAppearance(dark, foreground);
}

void WaylandWindow::statusItemHighlight(bool highlighted)
{
    if (m_statusItem)
        m_statusItem->statusItemHighlight(highlighted);
}

void WaylandWindow::statusItemPlaced(const RectF& rect)
{
    if (m_statusItem)
        m_statusItem->statusItemPlaced(rect);
}

void WaylandWindow::configureBounds(int width, int height)
{
    notifyConfigureBounds(width, height);
}

void WaylandWindow::interactiveResize(bool active)
{
    notifyInteractiveResize(active);
}

uint32_t WaylandWindow::dragEnter(double x, double y, DropOffer* offer, uint32_t modifiers)
{
    return notifyDragEnter(x, y, offer, modifiers);
}

uint32_t WaylandWindow::dragMotion(double x, double y, uint32_t modifiers)
{
    return notifyDragMotion(x, y, modifiers);
}

void WaylandWindow::dragLeave()
{
    notifyDragLeave();
}

uint32_t WaylandWindow::drop()
{
    return notifyDrop();
}

bool WaylandWindow::startDrag(DragSource source)
{
    DataDevice* device = m_connection->dataDevice();
    if (!m_shell || !device)
        return false;
    ShellSurface* attached = nullptr;
    if (source.attachedWindow) {
        auto* platform = static_cast<WaylandWindow*>(platformOf(source.attachedWindow));
        attached = platform ? platform->m_shell.get() : nullptr;
    }
    return device->startDrag(m_shell.get(), std::move(source), attached, m_window->scale());
}

void WaylandWindow::scaleChanged(double scale)
{
    notifyScaleChanged(scale);
}

void WaylandWindow::closeRequested()
{
    notifyCloseRequested();
}

void WaylandWindow::frameDone()
{
    notifyFrameDone();
}

void WaylandWindow::presented(uint64_t timestampNs, uint32_t refreshNs)
{
    notifyPresented(timestampNs, refreshNs);
}

void WaylandWindow::discarded()
{
    notifyDiscarded();
}

void WaylandWindow::pointerEnter(double x, double y)
{
    notifyPointerEnter(x, y);
}

void WaylandWindow::pointerLeave()
{
    notifyPointerLeave();
}

void WaylandWindow::pointerMotion(double x, double y)
{
    notifyPointerMotion(x, y);
}

void WaylandWindow::pointerButton(MouseButton button, bool pressed, uint32_t modifiers)
{
    notifyPointerButton(button, pressed, modifiers);
}

void WaylandWindow::pointerScroll(const ScrollEvent& event)
{
    notifyPointerScroll(event);
}

void WaylandWindow::keyboardEnter()
{
    notifyKeyboardEnter();
}

void WaylandWindow::keyboardLeave()
{
    notifyKeyboardLeave();
}

void WaylandWindow::key(const KeyEvent& event, bool pressed)
{
    notifyKey(event, pressed);
}

void WaylandWindow::pointerHold()
{
    notifyHold();
}

void WaylandWindow::touchDown(int32_t id, double x, double y)
{
    notifyTouchDown(id, x, y);
}

void WaylandWindow::touchMotion(int32_t id, double x, double y)
{
    notifyTouchMotion(id, x, y);
}

void WaylandWindow::touchUp(int32_t id)
{
    notifyTouchUp(id);
}

void WaylandWindow::touchCancel()
{
    notifyTouchCancel();
}


void WaylandWindow::textInput(const TextInputEvent& event)
{
    notifyTextInput(event);
}

} // namespace cutefish::ui::wayland
