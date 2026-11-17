// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/navigation_buttons.h"

namespace cutefish::ui {

NavigationButtons::NavigationButtons()
    : m_back(addButton("chevron-left"))
    , m_forward(addButton("chevron-right"))
{
}

} // namespace cutefish::ui
