// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/header_button_group.h"

#include "ui/header_button.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_controls.h"

namespace cutefish::ui {

// The divider stops short of the plate's top and bottom by this much.
static constexpr double kDividerInset = 6;

HeaderButtonGroup::HeaderButtonGroup()
{
    setMovesWindow(true);
}

HeaderButton* HeaderButtonGroup::addButton(std::string glyph, double glyphSize)
{
    auto* button = addChild<HeaderButton>(std::string(), kSegment);
    button->setGlyph(std::move(glyph), glyphSize);
    button->setSegment(true);
    m_buttons.push_back(button);
    return button;
}

SizeF HeaderButtonGroup::sizeHint() const
{
    int visible = 0;
    for (const HeaderButton* button : m_buttons) {
        if (button->isVisible())
            ++visible;
    }
    return {visible * kSegment, kSegment};
}

RectF HeaderButtonGroup::visualRect() const
{
    return window_controls::visualRect(localRect(), window_controls::State {});
}

void HeaderButtonGroup::layout()
{
    const RectF bounds = localRect();
    const SizeF hint = sizeHint();
    const double width = hint.width > 0 ? bounds.width * kSegment / hint.width : 0;
    double x = 0;
    for (HeaderButton* button : m_buttons) {
        if (!button->isVisible())
            continue;
        button->setGeometry({x, 0, width, bounds.height});
        x += width;
    }
}

void HeaderButtonGroup::paint(Painter& painter)
{
    const Window* window = this->window();
    const bool dark = window ? window->headerDarkMode() : Theme::current().darkMode;
    const RectF bounds = localRect();
    window_controls::paintGroupPlate(painter, bounds, dark);
    const Color edge = (dark ? Color::rgb(0xFFFFFF) : Color::rgb(0x000000)).withAlpha(dark ? 0.14 : 0.12);
    bool first = true;
    for (const HeaderButton* button : m_buttons) {
        if (!button->isVisible())
            continue;
        if (!first)
            painter.drawVerticalLine(button->geometry().x, bounds.y + kDividerInset,
                                     bounds.height - 2 * kDividerInset, edge);
        first = false;
    }
}

} // namespace cutefish::ui
