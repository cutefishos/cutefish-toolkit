// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ui/header_button_group.h"

namespace cutefish::ui {

// Back and forward for a header, one square each of a single plate in the
// window controls' style.
class NavigationButtons : public HeaderButtonGroup
{
public:
    NavigationButtons();

    HeaderButton* back() const { return m_back; }
    HeaderButton* forward() const { return m_forward; }

private:
    HeaderButton* m_back;
    HeaderButton* m_forward;
};

} // namespace cutefish::ui
