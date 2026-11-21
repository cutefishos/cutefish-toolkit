// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/platform_window.h"
#include "ui/window.h"

namespace cutefish::ui {

void Window::setBackgroundBlur(const RectF& rect, double radius)
{
    m_platform->setBlur(rect, radius);
    requestPaint();
}

void Window::setBackgroundBlurShape(const Path& path)
{
    m_platform->setBlurShape(path);
    // The shape is surface state the next commit carries.
    scheduleFrame();
}

void Window::setSubsurfaceBlur(const RectF& rect, double radius)
{
    m_platform->setSubsurfaceBlur(rect, radius);
    // Surface state the next commit carries.
    scheduleFrame();
}

void Window::setSubsurfaceBlurShape(const Path& path)
{
    m_platform->setSubsurfaceBlurShape(path);
    scheduleFrame();
}

void Window::setSubsurfaceBlurFill(const BackdropFill& fill)
{
    m_platform->setSubsurfaceBlurFill(fill);
    scheduleFrame();
}

void Window::setBackgroundGlass(const std::optional<GlassMaterial>& material)
{
    m_platform->setGlass(material);
    scheduleFrame();
}

bool Window::backgroundGlassAvailable() const
{
    return m_platform->glassAvailable();
}

void Window::setTransientForForeign(const std::string& handle, bool modal)
{
    m_platform->setForeignParent(handle, modal);
}

bool Window::setDesktopRole(const std::string& outputName)
{
    return m_platform->setDesktopRole(outputName);
}

void Window::setDesktopOutput(const std::string& outputName)
{
    m_platform->setShellOutput(outputName);
}

void Window::activateDesktop()
{
    m_platform->activateShellSurface();
}

void Window::handleWorkArea(const RectF& area)
{
    if (area == m_workArea)
        return;
    m_workArea = area;
    if (onWorkAreaChanged)
        onWorkAreaChanged();
}

SizeF Window::availableSize() const
{
    if (m_bounds.width > 0 && m_bounds.height > 0)
        return m_bounds;
    return screenSize();
}

void Window::setResizeAroundCenter(bool aroundCenter)
{
    m_platform->setResizeAroundCenter(aroundCenter);
}

void Window::setSkipTaskbar(bool skip)
{
    m_platform->setSkipTaskbar(skip);
}

void Window::setAlwaysOnTop(bool onTop)
{
    m_platform->setAlwaysOnTop(onTop);
}

void Window::setLevel(WindowLevel level)
{
    m_platform->setLevel(level);
}

void Window::setRestoreId(const std::string& id)
{
    m_platform->setRestoreId(id);
}

void Window::setPlacementAnchor(const std::optional<PlacementAnchor>& anchor)
{
    m_platform->setPlacementAnchor(anchor);
}

void Window::setBodyShape(const Path& path)
{
    m_platform->setBodyShape(path);
}

void Window::setCastsShadow(bool castsShadow)
{
    m_platform->setCastsShadow(castsShadow);
}

void Window::setOpacity(double opacity)
{
    m_platform->setOpacity(opacity);
}

void Window::setFullScreenBars(int height, bool push)
{
    m_platform->setFullScreenBars(height, push);
}

void Window::setFullScreenRevealHandler(std::function<void(int offset, int durationMs)> handler)
{
    m_platform->onFullScreenReveal = std::move(handler);
}

void Window::setPrepareFrameHandler(std::function<void()> handler)
{
    m_platform->onPrepareFrame = std::move(handler);
}

bool Window::setScreenPosition(PointF position)
{
    return m_platform->setScreenPosition(position);
}

void Window::handleScreenPosition(PointF position)
{
    if (m_screenPosition == position)
        return;
    m_screenPosition = position;
    if (onScreenPositionChanged)
        onScreenPositionChanged();
}

void Window::setUserResizable(bool resizable)
{
    m_platform->setUserResizable(resizable);
}

} // namespace cutefish::ui
