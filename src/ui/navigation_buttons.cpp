// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/navigation_buttons.h"

#include "ui/header_button.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_controls.h"

namespace cutefish::ui {

static constexpr double kSegment = 26;
static constexpr double kGlyph = 16;
// The divider stops short of the plate's top and bottom by this much.
static constexpr double kDividerInset = 6;

NavigationButtons::NavigationButtons()
{
    setMovesWindow(true);
    m_back = addChild<HeaderButton>(std::string(), kSegment);
    m_back->setGlyph("chevron-left", kGlyph);
    m_back->setSegment(true);
    m_forward = addChild<HeaderButton>(std::string(), kSegment);
    m_forward->setGlyph("chevron-right", kGlyph);
    m_forward->setSegment(true);
}

SizeF NavigationButtons::sizeHint() const
{
    return {2 * kSegment, kSegment};
}

RectF NavigationButtons::visualRect() const
{
    return window_controls::visualRect(localRect(), window_controls::State {});
}

void NavigationButtons::layout()
{
    const RectF bounds = localRect();
    const double half = bounds.width / 2;
    m_back->setGeometry({0, 0, half, bounds.height});
    m_forward->setGeometry({half, 0, bounds.width - half, bounds.height});
}

void NavigationButtons::paint(Painter& painter)
{
    const Window* window = this->window();
    const bool dark = window ? window->headerDarkMode() : Theme::current().darkMode;
    const RectF bounds = localRect();
    window_controls::paintGroupPlate(painter, bounds, dark);
    const Color edge = dark ? Color::rgb(0xFFFFFF) : Color::rgb(0x000000);
    const double x = bounds.width / 2;
    painter.drawVerticalLine(x, bounds.y + kDividerInset, bounds.height - 2 * kDividerInset,
                             edge.withAlpha(dark ? 0.14 : 0.12));
}

} // namespace cutefish::ui
