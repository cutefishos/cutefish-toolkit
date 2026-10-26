// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/subsurface.h"

#include <EGL/egl.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <viewporter-client-protocol.h>

#include "gpu/egl_context.h"
#include "ui/application.h"
#include "ui/platform_window.h"
#include "ui/window.h"
#include "wayland/connection.h"
#include "wayland/wayland_window.h"

namespace cutefish::ui {

struct SubsurfacePrivate final : wayland::SubsurfaceClient
{
    void parentCommitting() override;
    void parentCommitted() override;
    void parentGone() override;
    // The device size the geometry takes at the window's scale.
    base::Size targetSize() const;

    static void frameDone(void* data, wl_callback* callback, uint32_t);
    static constexpr wl_callback_listener frameListener = {frameDone};

    Subsurface* q = nullptr;
    Window* window = nullptr;
    wayland::WaylandWindow* parent = nullptr;
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_surface* surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wp_viewport* viewport = nullptr;
    // The surface again, on the drawing thread's queue, for frame callbacks.
    wl_surface* surfaceOnQueue = nullptr;
    wl_event_queue* queue = nullptr;
    wl_callback* frameCallback = nullptr;
    std::atomic<bool> frameReady = true;
    // Readable after wakeUp().
    int wakeFd = -1;

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    wl_egl_window* eglWindow = nullptr;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    bool swapIntervalSet = false;

    // UI thread state.
    RectF geometry;
    PointF placedPosition = {-1e9, -1e9};
    // Synchronised for the window's commit under way.
    bool synchronized = false;

    // Shared with the drawing thread.
    mutable std::mutex mutex;
    std::condition_variable swapped;
    base::Size wanted;
    SizeF wantedLogical;
    // Counts the sizes handed over, and the last one swapped at.
    uint64_t wantedSerial = 0;
    uint64_t swappedSerial = 0;
    bool opaque = true;

    // Drawing thread state.
    base::Size eglSize;
    base::Size frameSize;
    SizeF frameLogical;
    uint64_t frameSerial = 0;
    SizeF committedLogical;
    bool committedOpaque = false;
};

void SubsurfacePrivate::frameDone(void* data, wl_callback* callback, uint32_t)
{
    auto* self = static_cast<SubsurfacePrivate*>(data);
    wl_callback_destroy(callback);
    self->frameCallback = nullptr;
    self->frameReady = true;
}

base::Size SubsurfacePrivate::targetSize() const
{
    const double scale = window ? window->scale() : 1.0;
    return {int(std::lround(geometry.width * scale)), int(std::lround(geometry.height * scale))};
}

void SubsurfacePrivate::parentCommitting()
{
    if (!subsurface)
        return;
    if (geometry.x != placedPosition.x || geometry.y != placedPosition.y) {
        wl_subsurface_set_position(subsurface, int(std::lround(geometry.x)), int(std::lround(geometry.y)));
        placedPosition = {geometry.x, geometry.y};
    }

    // Sizes reach the drawing thread only here, so a buffer of a new size
    // shows with the window's commit of that size, never before it. A scale
    // change moves the device size too.
    const base::Size target = targetSize();
    const SizeF logical = {geometry.width, geometry.height};
    std::unique_lock lock(mutex);
    if (target == wanted && logical == wantedLogical)
        return;
    // The first size has no picture to keep in step with.
    const bool waits = q->onResize && !target.isEmpty() && swappedSerial > 0;
    if (waits && !synchronized) {
        wl_subsurface_set_sync(subsurface);
        synchronized = true;
    }
    wanted = target;
    wantedLogical = logical;
    const uint64_t serial = ++wantedSerial;
    if (!waits)
        return;
    lock.unlock();
    q->onResize(target);
    lock.lock();
    swapped.wait_for(lock, std::chrono::milliseconds(q->resizeTimeoutMs),
                     [&] { return swappedSerial >= serial; });
}

void SubsurfacePrivate::parentCommitted()
{
    // The commit took the buffer; the next ones show as they are swapped.
    if (!synchronized)
        return;
    wl_subsurface_set_desync(subsurface);
    synchronized = false;
}

void SubsurfacePrivate::parentGone()
{
    // The wl_subsurface goes inert with its parent's surface.
    parent = nullptr;
    window = nullptr;
}

Subsurface::Subsurface(Window* window, Stacking stacking)
    : d(std::make_unique<SubsurfacePrivate>())
{
    d->q = this;
    d->wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    auto* application = Application::instance();
    if (application->platform() != Application::Platform::Wayland || !window->m_platform
        || window->m_platform->isHeadless() || window->m_platform->isEmbedded())
        return;
    auto* parent = static_cast<wayland::WaylandWindow*>(window->m_platform.get());
    wayland::Connection& connection = parent->connection();
    if (!connection.subcompositor() || !parent->shell())
        return;

    d->window = window;
    d->parent = parent;
    d->display = connection.display();
    d->compositor = connection.compositor();
    d->surface = wl_compositor_create_surface(connection.compositor());
    wl_surface* parentSurface = parent->shell()->surface();
    d->subsurface = wl_subcompositor_get_subsurface(connection.subcompositor(), d->surface, parentSurface);
    if (stacking == Stacking::Below)
        wl_subsurface_place_below(d->subsurface, parentSurface);
    else
        wl_subsurface_place_above(d->subsurface, parentSurface);
    wl_subsurface_set_desync(d->subsurface);
    d->viewport = wp_viewporter_get_viewport(connection.viewporter(), d->surface);

    // Pointer events stay with the window underneath.
    wl_region* empty = wl_compositor_create_region(connection.compositor());
    wl_surface_set_input_region(d->surface, empty);
    wl_region_destroy(empty);

    d->queue = wl_display_create_queue(d->display);
    d->surfaceOnQueue = static_cast<wl_surface*>(wl_proxy_create_wrapper(d->surface));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(d->surfaceOnQueue), d->queue);

    gpu::EglContext& gpu = parent->gpuContext();
    d->eglDisplay = gpu.display();
    d->eglConfig = gpu.config();
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    d->context = eglCreateContext(d->eglDisplay, d->eglConfig, EGL_NO_CONTEXT, attributes);
    if (d->context == EGL_NO_CONTEXT)
        fprintf(stderr, "cutefish-toolkit: cannot create a GLES 3 context for a subsurface\n");
    parent->addSubsurface(d.get());
}

Subsurface::~Subsurface()
{
    if (d->parent)
        d->parent->removeSubsurface(d.get());
    if (d->eglSurface != EGL_NO_SURFACE)
        eglDestroySurface(d->eglDisplay, d->eglSurface);
    if (d->context != EGL_NO_CONTEXT)
        eglDestroyContext(d->eglDisplay, d->context);
    if (d->eglWindow)
        wl_egl_window_destroy(d->eglWindow);
    if (d->frameCallback)
        wl_callback_destroy(d->frameCallback);
    if (d->surfaceOnQueue)
        wl_proxy_wrapper_destroy(d->surfaceOnQueue);
    if (d->viewport)
        wp_viewport_destroy(d->viewport);
    if (d->subsurface)
        wl_subsurface_destroy(d->subsurface);
    if (d->surface)
        wl_surface_destroy(d->surface);
    if (d->queue)
        wl_event_queue_destroy(d->queue);
    if (d->display)
        wl_display_flush(d->display);
    if (d->wakeFd >= 0)
        close(d->wakeFd);
}

bool Subsurface::isValid() const
{
    return d->subsurface && d->context != EGL_NO_CONTEXT;
}

void Subsurface::setGeometry(const RectF& rect)
{
    if (rect == d->geometry)
        return;
    d->geometry = rect;
    // Position and size land with the window's next commit.
    if (d->window)
        d->window->scheduleFrame();
}

RectF Subsurface::geometry() const
{
    return d->geometry;
}

void Subsurface::setOpaque(bool opaque)
{
    std::lock_guard lock(d->mutex);
    d->opaque = opaque;
}

void* Subsurface::getProcAddress(const char* name)
{
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void* Subsurface::eglDisplay() const
{
    return d->eglDisplay;
}

wl_display* Subsurface::waylandDisplay()
{
    auto* application = Application::instance();
    if (!application || application->platform() != Application::Platform::Wayland || !application->hasConnection())
        return nullptr;
    return application->connection().display();
}

bool Subsurface::makeCurrent()
{
    if (!isValid())
        return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    return makeCurrent(d->context);
}

bool Subsurface::makeCurrent(void* eglContext)
{
    if (!d->subsurface || eglContext == EGL_NO_CONTEXT)
        return false;
    if (d->eglSurface == EGL_NO_SURFACE) {
        // A context with no surface yet, for a renderer to set itself up.
        return eglMakeCurrent(d->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, eglContext);
    }
    if (!eglMakeCurrent(d->eglDisplay, d->eglSurface, d->eglSurface, eglContext))
        return false;
    if (!d->swapIntervalSet) {
        // Frames are paced by waitForFrame(), never inside a swap.
        eglSwapInterval(d->eglDisplay, 0);
        d->swapIntervalSet = true;
    }
    return true;
}

void Subsurface::wakeUp()
{
    const uint64_t one = 1;
    if (d->wakeFd >= 0 && write(d->wakeFd, &one, sizeof(one)) < 0) {
        // Full only when nobody waited for a long time; the wait ends anyway.
    }
}

void Subsurface::doneCurrent()
{
    if (d->eglDisplay != EGL_NO_DISPLAY)
        eglMakeCurrent(d->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

base::Size Subsurface::beginFrame()
{
    if (!isValid())
        return {};
    eglBindAPI(EGL_OPENGL_ES_API);
    return beginFrame(d->context);
}

base::Size Subsurface::beginFrame(void* eglContext)
{
    if (!d->subsurface || eglContext == EGL_NO_CONTEXT)
        return {};
    {
        std::lock_guard lock(d->mutex);
        d->frameSize = d->wanted;
        d->frameLogical = d->wantedLogical;
        d->frameSerial = d->wantedSerial;
    }
    const base::Size size = d->frameSize;
    if (size.isEmpty())
        return {};
    if (!d->eglWindow) {
        d->eglWindow = wl_egl_window_create(d->surface, size.width(), size.height());
        d->eglSurface = eglCreatePlatformWindowSurface(d->eglDisplay, d->eglConfig, d->eglWindow, nullptr);
        if (d->eglSurface == EGL_NO_SURFACE) {
            fprintf(stderr, "cutefish-toolkit: cannot create an EGL surface for a subsurface\n");
            return {};
        }
    } else if (!(size == d->eglSize)) {
        // Takes effect with the buffer drawn next, this frame's.
        wl_egl_window_resize(d->eglWindow, size.width(), size.height(), 0, 0);
    }
    d->eglSize = size;
    if (!makeCurrent(eglContext))
        return {};
    return size;
}

bool Subsurface::endFrame()
{
    if (d->eglSurface == EGL_NO_SURFACE)
        return false;
    bool opaque;
    {
        std::lock_guard lock(d->mutex);
        opaque = d->opaque;
    }
    // Surface state for the commit eglSwapBuffers makes.
    const bool resized = !(d->frameLogical == d->committedLogical);
    if (resized) {
        wp_viewport_set_destination(d->viewport, std::max(1, int(std::lround(d->frameLogical.width))),
                                    std::max(1, int(std::lround(d->frameLogical.height))));
    }
    if (resized || opaque != d->committedOpaque) {
        if (opaque) {
            wl_region* region = wl_compositor_create_region(d->compositor);
            wl_region_add(region, 0, 0, int(std::lround(d->frameLogical.width)),
                          int(std::lround(d->frameLogical.height)));
            wl_surface_set_opaque_region(d->surface, region);
            wl_region_destroy(region);
        } else {
            wl_surface_set_opaque_region(d->surface, nullptr);
        }
    }
    d->committedLogical = d->frameLogical;
    d->committedOpaque = opaque;

    if (!d->frameCallback) {
        d->frameCallback = wl_surface_frame(d->surfaceOnQueue);
        wl_callback_add_listener(d->frameCallback, &SubsurfacePrivate::frameListener, d.get());
    }
    d->frameReady = false;
    const bool done = eglSwapBuffers(d->eglDisplay, d->eglSurface);
    {
        std::lock_guard lock(d->mutex);
        d->swappedSerial = d->frameSerial;
    }
    d->swapped.notify_all();
    return done;
}

bool Subsurface::waitForFrame(int timeoutMs)
{
    if (!d->queue)
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    wl_display_dispatch_queue_pending(d->display, d->queue);
    while (!d->frameReady) {
        while (wl_display_prepare_read_queue(d->display, d->queue) != 0)
            wl_display_dispatch_queue_pending(d->display, d->queue);
        wl_display_flush(d->display);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptors[] = {{wl_display_get_fd(d->display), POLLIN, 0}, {d->wakeFd, POLLIN, 0}};
        const int ready = left.count() <= 0 ? 0 : poll(descriptors, 2, int(left.count()));
        if (ready <= 0 || (descriptors[1].revents & POLLIN)) {
            wl_display_cancel_read(d->display);
            wl_display_dispatch_queue_pending(d->display, d->queue);
            uint64_t count;
            while (read(d->wakeFd, &count, sizeof(count)) > 0) {
            }
            return d->frameReady;
        }
        if (wl_display_read_events(d->display) < 0)
            return false;
        wl_display_dispatch_queue_pending(d->display, d->queue);
    }
    return true;
}

} // namespace cutefish::ui
