// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/platform_window.h"

#include <array>
#include <cmath>

#include "include/core/SkSurface.h"

#include "ui/application.h"
#include "ui/embedded_platform_window.h"
#include "ui/text_context.h"
#include "ui/window.h"
#include "wayland/connection.h"
#include "wayland/wayland_window.h"

namespace cutefish::ui {

// Tests pretend a compositor that draws glass.
static bool headlessGlassAvailable = false;

namespace {

class HeadlessWindow final : public PlatformWindow
{
public:
    using PlatformWindow::PlatformWindow;

    // A status item: configured at the size it shows at, in a light bar.
    HeadlessWindow(Window* window, StatusItemClient* statusItem)
        : PlatformWindow(window)
        , m_statusItem(statusItem)
    {
    }

    bool isHeadless() const override { return true; }
    void setTitle(const std::string&) override {}
    void setAppId(const std::string&) override {}
    void setMinimumSize(int, int) override {}
    void setMaximumSize(int, int) override {}
    void setMaximized(bool maximized) override
    {
        m_states.maximized = maximized;
        notifyConfigure(0, 0, m_states);
    }
    void setFullScreen(bool fullScreen) override
    {
        m_states.fullScreen = fullScreen;
        notifyConfigure(0, 0, m_states);
    }
    void minimize() override {}
    void setTransientParent(PlatformWindow*, bool) override {}
    void startMove() override { ++m_moveRequests; }
    int moveRequests() const override { return m_moveRequests; }
    void setCursorShape(CursorShape) override {}
    void setBlur(const RectF&, double) override {}
    void setGlass(const std::optional<GlassMaterial>& material) override { m_glass = material; }
    bool glassAvailable() const override { return headlessGlassAvailable; }
    std::optional<GlassMaterial> requestedGlass() const override { return m_glass; }
    void setTextInput(const TextInputState*) override {}

    void show(int width, int height) override
    {
        m_states.activated = !m_statusItem;
        if (m_statusItem) {
            m_statusItem->statusItemAppearance(false, 0xd9000000);
            m_statusItem->statusItemHighlight(m_highlight);
            m_statusItem->statusItemLimits(0);
        }
        notifyConfigure(width, height, m_states);
    }

    void setStatusItemHighlight(bool highlighted) override
    {
        m_highlight = highlighted;
        if (m_statusItem)
            m_statusItem->statusItemHighlight(highlighted);
    }

    // A chain of buffers taken in turn, so frames repaint what changed since
    // the buffer's last frame as on screen.
    bool present(SizeF size, double scale, bool, const PaintFunction& paint) override
    {
        const int width = int(std::lround(size.width * scale));
        const int height = int(std::lround(size.height * scale));
        if (width != m_buffers[0].width() || height != m_buffers[0].height()) {
            for (size_t i = 0; i < m_buffers.size(); ++i) {
                m_buffers[i] = Image(width, height);
                m_ages[i] = 0;
            }
        }
        const size_t index = m_next;
        m_next = (m_next + 1) % m_buffers.size();
        Image& buffer = m_buffers[index];
        const SkImageInfo info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        const SkSurfaceProps properties = Application::instance()->text().surfaceProps();
        const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, buffer.bits(), size_t(width) * 4,
                                                                &properties);
        if (!surface)
            return false;
        paint(surface->getCanvas(), m_ages[index]);
        for (int& age : m_ages) {
            if (age > 0)
                ++age;
        }
        m_ages[index] = 1;
        m_presented = &buffer;
        return true;
    }

    const Image* presentedImage() const override { return m_presented; }

private:
    WindowStates m_states;
    std::array<Image, 3> m_buffers;
    std::array<int, 3> m_ages = {};
    size_t m_next = 0;
    const Image* m_presented = nullptr;
    int m_moveRequests = 0;
    std::optional<GlassMaterial> m_glass;
    StatusItemClient* m_statusItem = nullptr;
    bool m_highlight = false;
};

} // namespace

void PlatformWindow::setHeadlessGlassAvailable(bool available)
{
    headlessGlassAvailable = available;
}

std::unique_ptr<PlatformWindow> PlatformWindow::create(Window* window, PlatformWindow* parent,
                                                       const PopupPlacement& placement, bool grab)
{
    if (Application::instance()->platform() == Application::Platform::Headless)
        return std::make_unique<HeadlessWindow>(window);
    // Popups included: an embedded host draws what it is given and nothing else.
    if (Application::instance()->platform() == Application::Platform::Embedded) {
        return std::make_unique<EmbeddedPlatformWindow>(window, static_cast<EmbeddedPlatformWindow*>(parent),
                                                        placement, grab);
    }
    if (parent) {
        return std::make_unique<wayland::WaylandWindow>(
            window, static_cast<wayland::WaylandWindow*>(parent), placement, grab);
    }
    return std::make_unique<wayland::WaylandWindow>(window);
}

std::unique_ptr<PlatformWindow> PlatformWindow::createStatusItem(Window* window, StatusItemClient* client)
{
    if (statusItemsSupported())
        return std::make_unique<wayland::WaylandWindow>(window, client);
    return std::make_unique<HeadlessWindow>(window, client);
}

bool PlatformWindow::statusItemsSupported()
{
    Application* application = Application::instance();
    return application && application->platform() == Application::Platform::Wayland
            && application->connection().statusItems();
}

void PlatformWindow::notifyConfigure(int width, int height, const WindowStates& states)
{
    m_window->handleConfigure(width, height, states);
}

void PlatformWindow::notifyConfigureBounds(int width, int height)
{
    m_window->handleConfigureBounds(width, height);
}

void PlatformWindow::notifyInteractiveResize(bool active)
{
    m_window->handleInteractiveResize(active);
}

void PlatformWindow::notifyFullScreenReveal(int offset, int durationMs)
{
    if (onFullScreenReveal)
        onFullScreenReveal(offset, durationMs);
}

void PlatformWindow::notifyScaleChanged(double scale)
{
    m_window->handleScaleChanged(scale);
}

void PlatformWindow::notifyCloseRequested()
{
    m_window->handleCloseRequested();
}

void PlatformWindow::notifyFrameDone()
{
    m_window->handleFrameDone();
}

void PlatformWindow::notifyPresented(uint64_t timestampNs, uint32_t refreshNs)
{
    m_window->handlePresented(timestampNs, refreshNs);
}

void PlatformWindow::notifyDiscarded()
{
    m_window->handleDiscarded();
}

void PlatformWindow::notifyPointerEnter(double x, double y)
{
    m_window->handlePointerEnter(x, y);
}

void PlatformWindow::notifyPointerLeave()
{
    m_window->handlePointerLeave();
}

void PlatformWindow::notifyPointerMotion(double x, double y)
{
    m_window->handlePointerMotion(x, y);
}

void PlatformWindow::notifyPointerButton(MouseButton button, bool pressed, uint32_t modifiers)
{
    m_window->handlePointerButton(button, pressed, modifiers);
}

void PlatformWindow::notifyPointerScroll(const ScrollEvent& event)
{
    m_window->handlePointerScroll(event);
}

void PlatformWindow::notifyKeyboardEnter()
{
    m_window->handleKeyboardFocus(true);
}

void PlatformWindow::notifyKeyboardLeave()
{
    m_window->handleKeyboardFocus(false);
}

void PlatformWindow::notifyKey(const KeyEvent& event, bool pressed)
{
    m_window->handleKey(event, pressed);
}

void PlatformWindow::notifyHold()
{
    m_window->handleHold();
}

void PlatformWindow::notifyGesture(const GestureEvent& event)
{
    m_window->handleGesture(event);
}

void PlatformWindow::notifyTouchDown(int32_t id, double x, double y)
{
    m_window->handleTouchDown(id, x, y);
}

void PlatformWindow::notifyTouchMotion(int32_t id, double x, double y)
{
    m_window->handleTouchMotion(id, x, y);
}

void PlatformWindow::notifyTouchUp(int32_t id)
{
    m_window->handleTouchUp(id);
}

void PlatformWindow::notifyTouchCancel()
{
    m_window->handleTouchCancel();
}

void PlatformWindow::notifyTextInput(const TextInputEvent& event)
{
    m_window->handleTextInput(event);
}

void PlatformWindow::notifyWorkArea(const RectF& area)
{
    m_window->handleWorkArea(area);
}

void PlatformWindow::notifyScreenPosition(PointF position)
{
    m_window->handleScreenPosition(position);
}

PlatformWindow* PlatformWindow::platformOf(Window* window)
{
    return window ? window->m_platform.get() : nullptr;
}

uint32_t PlatformWindow::notifyDragEnter(double x, double y, DropOffer* offer, uint32_t modifiers)
{
    m_window->handleDragEnter(x, y, offer, modifiers);
    return m_window->m_dropAction;
}

uint32_t PlatformWindow::notifyDragMotion(double x, double y, uint32_t modifiers)
{
    return m_window->handleDragMotion(x, y, modifiers);
}

void PlatformWindow::notifyDragLeave()
{
    m_window->handleDragLeave();
}

uint32_t PlatformWindow::notifyDrop()
{
    return m_window->handleDrop();
}

} // namespace cutefish::ui
