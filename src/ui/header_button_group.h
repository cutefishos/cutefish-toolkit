// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

#include "ui/view.h"

namespace cutefish::ui {

class HeaderButton;

// Header buttons on a single plate in the window controls' style, one square
// each, with a divider between neighbours. Hidden buttons give up their square.
class HeaderButtonGroup : public View
{
public:
    static constexpr double kSegment = 26;

    HeaderButtonGroup();

    // One of the toolkit's glyphs, such as "search" or "sidebar".
    HeaderButton* addButton(std::string glyph, double glyphSize = 16);
    const std::vector<HeaderButton*>& buttons() const { return m_buttons; }

    SizeF sizeHint() const override;
    RectF visualRect() const override;

protected:
    void layout() override;
    void paint(Painter& painter) override;

private:
    std::vector<HeaderButton*> m_buttons;
};

} // namespace cutefish::ui
