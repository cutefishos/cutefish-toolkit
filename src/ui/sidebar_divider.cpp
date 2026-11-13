// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/sidebar_divider.h"

#include <algorithm>
#include <cmath>

namespace cutefish::ui {

SidebarDivider::SidebarDivider(double minimumWidth, double maximumWidth, double defaultWidth)
    : m_minimum(minimumWidth)
    , m_maximum(maximumWidth)
    , m_default(defaultWidth)
    , m_width(defaultWidth)
{
}

double SidebarDivider::clamped(double width) const
{
    return std::clamp(std::round(width), m_minimum, m_maximum);
}

void SidebarDivider::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    if (event.clickCount == 2) {
        m_dragging = false;
        if (onFinished)
            onFinished(m_default);
        return;
    }
    m_dragging = true;
    m_startWidth = m_width;
    m_startX = mapToWindow(event.position).x;
}

void SidebarDivider::pointerMove(const PointerEvent& event)
{
    if (!m_dragging || !onResized)
        return;
    onResized(clamped(m_startWidth + mapToWindow(event.position).x - m_startX));
}

void SidebarDivider::pointerRelease(const PointerEvent& event)
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (onFinished)
        onFinished(clamped(m_startWidth + mapToWindow(event.position).x - m_startX));
}

} // namespace cutefish::ui
