// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "ui/geometry.h"
#include "ui/window.h"

namespace cutefish::ui {

class View;

// An item the application draws in the menu bar: a date, a short title,
// an icon, an icon and a text. Its window is as tall as the bar and as wide
// as setWidth() makes it; its content view draws it and takes its input, so
// the application decides what a press does, typically opening a popup on
// a primary press and a menu on a secondary one.
//
// Draw single-colour artwork in foregroundColor(), which follows the bar's
// look over the desktop behind it. While isHighlighted(), the bar draws its
// highlight plate behind the item.
//
// Popups hang from the item: Window(&item.window(), item.popupPlacement())
// opens one centred under it, ui::Menu::popupAt(&item.window(), ...) a menu.
// The compositor dismisses a grabbing one on a press elsewhere and keeps it
// on a press on the item, which the application then hears.
//
// On a compositor without status items (isSupported() false) the item is
// never shown; use a status notifier item there.
class StatusItem
{
public:
    // Whether the compositor shows status items.
    static bool isSupported();

    StatusItem();
    ~StatusItem();

    StatusItem(const StatusItem&) = delete;
    StatusItem& operator=(const StatusItem&) = delete;

    Window& window() { return *m_window; }
    void setContentView(std::unique_ptr<View> view);

    // In logical pixels, whole ones; the bar leaves the gaps between items.
    void setWidth(double width);
    double width() const;
    // The bar's height, which the item is drawn at; a default until the bar
    // configured the item.
    double height() const;
    // The widest the bar shows the item at; 0 for no limit. A wider item is
    // not shown.
    double maximumWidth() const { return m_maximumWidth; }

    // A name that stays the same each time the application makes this item,
    // so it comes back to its place in the bar.
    void setIdentifier(const std::string& identifier);
    // Asks for the bar's highlight behind the item, as while a popup of the
    // item is open; grabbing popups of the item have it anyway.
    void setHighlighted(bool highlighted);
    // Whether the bar draws its highlight behind the item now.
    bool isHighlighted() const { return m_highlighted; }
    // The bar is dark behind its items, which are then light.
    bool darkAppearance() const { return m_darkAppearance; }
    // The colour the bar draws its own text and symbols in.
    Color foregroundColor() const { return m_foreground; }
    // Where the item is on the screen, global logical; empty while it has no
    // place in the bar. A window that cannot be a popup of the item opens
    // next to it with Window::setPlacementAnchor().
    const RectF& screenRect() const { return m_screenRect; }

    // Shows the item in the bar, once.
    void show();

    // Hangs a popup centred under the item, `gap` below it.
    PopupPlacement popupPlacement(double gap = 0) const;

    // The bar's height or the widest the item may be changed.
    std::function<void()> onConfigured;
    std::function<void()> onAppearanceChanged;
    std::function<void()> onHighlightChanged;
    std::function<void()> onScreenRectChanged;

private:
    class Client;
    friend class Client;

    void repaint();

    std::unique_ptr<Client> m_client;
    std::unique_ptr<Window> m_window;
    double m_maximumWidth = 0;
    bool m_highlighted = false;
    bool m_darkAppearance = false;
    Color m_foreground {0, 0, 0, 217};
    RectF m_screenRect;
};

} // namespace cutefish::ui
