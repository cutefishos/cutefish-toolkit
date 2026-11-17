// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/view.h"

namespace cutefish::ui {

class HeaderButton;

// Back and forward for a header, one square each of a single plate in the
// window controls' style.
class NavigationButtons : public View
{
public:
    NavigationButtons();

    HeaderButton* back() const { return m_back; }
    HeaderButton* forward() const { return m_forward; }

    SizeF sizeHint() const override;
    RectF visualRect() const override;

protected:
    void layout() override;
    void paint(Painter& painter) override;

private:
    HeaderButton* m_back;
    HeaderButton* m_forward;
};

} // namespace cutefish::ui
