// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/scroll_bar.h"

#include <algorithm>
#include <chrono>

#include "base/event_loop.h"
#include "ui/animation.h"
#include "ui/application.h"
#include "ui/painter.h"
#include "ui/theme.h"

namespace cutefish::ui {

// Two lines of the default font, so long content keeps a handle to grab.
static constexpr double kMinimumHandleLength = 32;
static constexpr auto kFadeDuration = std::chrono::milliseconds(250);
static constexpr auto kHideDelay = std::chrono::milliseconds(1000);

ScrollBar::ScrollBar(Orientation orientation)
    : m_orientation(orientation)
    , m_startPadding(inset)
    , m_endPadding(inset)
{
}

ScrollBar::~ScrollBar()
{
    if (m_hideTimer)
        Application::instance()->loop().stopTimer(m_hideTimer);
}

void ScrollBar::setRange(double viewport, double content, double position)
{
    if (viewport == m_viewport && content == m_content && position == m_position)
        return;
    m_viewport = viewport;
    m_content = content;
    m_position = position;
    if (m_opacity > 0)
        requestPaint();
    updateShown();
}

void ScrollBar::setPadding(double start, double end)
{
    m_startPadding = start;
    m_endPadding = end;
    requestPaint();
}

void ScrollBar::setAlwaysShown(bool shown)
{
    m_alwaysShown = shown;
    updateShown();
}

void ScrollBar::setMoving(bool moving)
{
    m_moving = moving;
    updateShown();
}

double ScrollBar::along(PointF point) const
{
    return m_orientation == Orientation::Vertical ? point.y : point.x;
}

double ScrollBar::trackLength() const
{
    const double length = m_orientation == Orientation::Vertical ? geometry().height
                                                                   : geometry().width;
    return std::max(0.0, length - m_startPadding - m_endPadding);
}

double ScrollBar::handleLength() const
{
    const double track = trackLength();
    if (m_content <= 0)
        return track;
    return std::min(track, std::max(kMinimumHandleLength, track * m_viewport / m_content));
}

double ScrollBar::handleStart() const
{
    const double range = m_content - m_viewport;
    const double fraction = range > 0 ? std::clamp(m_position / range, 0.0, 1.0) : 0;
    return m_startPadding + (trackLength() - handleLength()) * fraction;
}

void ScrollBar::setDarkMode(std::optional<bool> dark)
{
    if (dark == m_darkMode)
        return;
    m_darkMode = dark;
    requestPaint();
}

void ScrollBar::paint(Painter& painter)
{
    if (m_opacity <= 0 || !isNeeded())
        return;
    const Theme& theme = Theme::current();
    const ColorProvider& colors = ColorProvider::forMode(m_darkMode.value_or(theme.darkMode));
    const Color thumb = colors.get(m_dragging ? ColorId::ScrollBarThumbDragging : ColorId::ScrollBarThumb);
    const Color color = thumb.withAlpha(thumb.a / 255.0 * m_opacity);
    const double width = thickness - 2 * inset;
    RectF handle;
    if (m_orientation == Orientation::Vertical)
        handle = {inset, handleStart(), width, handleLength()};
    else
        handle = {handleStart(), inset, handleLength(), width};
    painter.drawSmoothRectangle(handle, theme.smallRadius, color);
}

void ScrollBar::pointerEnter()
{
    m_hovered = true;
    updateShown();
}

void ScrollBar::pointerLeave()
{
    m_hovered = false;
    updateShown();
}

void ScrollBar::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left || !isNeeded())
        return;
    const double point = along(event.position);
    const double start = handleStart();
    if (point >= start && point < start + handleLength()) {
        m_dragging = true;
        m_dragOffset = point - start;
        requestPaint();
        return;
    }
    // Outside the handle a press turns a page towards it.
    if (onPositionRequested)
        onPositionRequested(m_position + (point < start ? -m_viewport : m_viewport));
}

void ScrollBar::pointerMove(const PointerEvent& event)
{
    if (!m_dragging)
        return;
    const double travel = trackLength() - handleLength();
    if (travel <= 0)
        return;
    const double fraction = (along(event.position) - m_dragOffset - m_startPadding) / travel;
    if (onPositionRequested)
        onPositionRequested(std::clamp(fraction, 0.0, 1.0) * (m_content - m_viewport));
}

void ScrollBar::pointerRelease(const PointerEvent&)
{
    if (!m_dragging)
        return;
    m_dragging = false;
    requestPaint();
    updateShown();
}

void ScrollBar::updateShown()
{
    const bool shown = isNeeded() && (m_alwaysShown || m_moving || m_hovered || m_dragging);
    auto& loop = Application::instance()->loop();
    if (shown) {
        if (m_hideTimer) {
            loop.stopTimer(m_hideTimer);
            m_hideTimer = 0;
        }
        fadeTo(1);
        return;
    }
    // A bar that never started to show stays hidden.
    if (m_opacity <= 0 || m_hideTimer || m_fadeTo == 0)
        return;
    m_hideTimer = loop.startTimer(kHideDelay, false, [this] {
        m_hideTimer = 0;
        fadeTo(0);
    });
}

void ScrollBar::fadeTo(double opacity)
{
    if (m_fade && m_fade->window() != window())
        m_fade.reset();
    if (opacity == m_fadeTo && (m_opacity == opacity || (m_fade && m_fade->isRunning())))
        return;
    if (!window()) {
        m_opacity = opacity;
        m_fadeTo = opacity;
        return;
    }
    if (!m_fade) {
        m_fade = std::make_unique<Animation>(window());
        m_fade->setDuration(kFadeDuration);
        m_fade->setEasing(Animation::Easing::Linear);
        m_fade->onUpdate = [this](double value) {
            m_opacity = m_fadeFrom + (m_fadeTo - m_fadeFrom) * value;
            requestPaint();
        };
    }
    m_fadeFrom = m_opacity;
    m_fadeTo = opacity;
    m_fade->start();
}

} // namespace cutefish::ui
