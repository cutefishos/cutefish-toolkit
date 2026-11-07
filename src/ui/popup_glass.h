// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <optional>

#include "ui/window.h"

namespace cutefish::ui {

// The glass of menus and drop-downs, frosted enough that text reads over any
// backdrop; the lens is kept to the lip and the edge is the compositor's
// window outline, so there is no rim. The rest is the compositor's defaults.
GlassMaterial popupGlass(bool darkMode);

// A popup's glass, kept in the theme's colours.
class PopupGlass
{
public:
    // Asks the window for the glass when the theme changed since; true when
    // the compositor draws it, and then the popup paints no fill of its own.
    bool update(Window& window);
    // A popup window's transparent background, before its first paint. Over
    // the glass the text sits on transparent pixels, where one alpha per
    // pixel cannot carry per-channel coverage, so it is greyscale there.
    static void prepare(Window& window);

private:
    std::optional<bool> m_darkMode;
};

} // namespace cutefish::ui
