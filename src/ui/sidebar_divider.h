// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>

#include "ui/view.h"

namespace cutefish::ui {

// A sidebar's edge, which resizes it: a drag moves the edge within the
// width range, a double click puts back the default width. It is kWidth
// wide, centred on the edge, and shows the horizontal resize pointer.
class SidebarDivider : public View
{
public:
    static constexpr double kWidth = 9;

    SidebarDivider(double minimumWidth, double maximumWidth, double defaultWidth);

    double minimumWidth() const { return m_minimum; }
    double maximumWidth() const { return m_maximum; }
    double defaultWidth() const { return m_default; }
    // The width the sidebar is laid out at, which a drag starts from.
    void setSidebarWidth(double width) { m_width = width; }
    double sidebarWidth() const { return m_width; }

    // The width being dragged to, and when the drag ends, the width kept;
    // whole pixels within the range.
    std::function<void(double width)> onResized;
    std::function<void(double width)> onFinished;

    bool acceptsPointer() const override { return true; }
    CursorShape cursorShape() const override { return CursorShape::ResizeHorizontal; }

protected:
    void pointerPress(const PointerEvent& event) override;
    void pointerMove(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;

private:
    double clamped(double width) const;

    double m_minimum;
    double m_maximum;
    double m_default;
    double m_width;
    bool m_dragging = false;
    double m_startWidth = 0;
    double m_startX = 0;
};

} // namespace cutefish::ui
