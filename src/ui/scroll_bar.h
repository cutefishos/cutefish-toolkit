// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "ui/view.h"

namespace cutefish::ui {

class Animation;

// A scroll bar: a thin handle over the edge of the view, shown while
// the view moves and while hovered, fading out a second after.
class ScrollBar : public View
{
public:
    enum class Orientation {
        Horizontal,
        Vertical,
    };

    // Across the bar, padding included.
    static constexpr double thickness = 8;
    // The handle's gap to the bar's sides and, unless padded, its track ends.
    static constexpr double inset = 1;

    explicit ScrollBar(Orientation orientation);
    ~ScrollBar() override;

    // Lengths along the bar's direction; the position may overshoot.
    void setRange(double viewport, double content, double position);
    void setMoving(bool moving);
    // Shown whenever the content overflows, as ScrollBar.AlwaysOn.
    void setAlwaysShown(bool shown);
    // Before and after the handle's track, along the bar, as ScrollBar's
    // topPadding and bottomPadding; 1 unless set.
    void setPadding(double start, double end);
    bool isNeeded() const { return m_content > m_viewport; }
    // The handle drawn for a dark or a light background whatever the theme,
    // unset follows the theme.
    void setDarkMode(std::optional<bool> dark);

    bool acceptsPointer() const override { return isNeeded(); }

    // The content position the bar was dragged or paged to.
    std::function<void(double position)> onPositionRequested;

protected:
    void paint(Painter& painter) override;
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const PointerEvent& event) override;
    void pointerMove(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;

private:
    double along(PointF point) const;
    double trackLength() const;
    double handleLength() const;
    double handleStart() const;
    void updateShown();
    void fadeTo(double opacity);

    Orientation m_orientation;
    double m_viewport = 0;
    double m_content = 0;
    double m_position = 0;
    bool m_moving = false;
    bool m_alwaysShown = false;
    std::optional<bool> m_darkMode;
    bool m_hovered = false;
    bool m_dragging = false;
    double m_dragOffset = 0;
    double m_startPadding;
    double m_endPadding;
    double m_opacity = 0;
    double m_fadeFrom = 0;
    double m_fadeTo = 0;
    int m_hideTimer = 0;
    std::unique_ptr<Animation> m_fade;
};

} // namespace cutefish::ui
