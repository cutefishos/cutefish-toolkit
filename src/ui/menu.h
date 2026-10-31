// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/bubble_shape.h"
#include "ui/geometry.h"

namespace cutefish::ui {

class View;
class Window;
struct PopupPlacement;

namespace wayland {
class MainMenuMirror;
}

// What a submenu of a main menu is for: the status bar opens the
// application's own menu from the application's name.
enum class MenuRole
{
    None,
    Application,
    Windows,
    Help,
    Services,
};

// A row's check mark; mixed for a mix of on and off.
enum class MenuItemState
{
    Off,
    On,
    Mixed,
};

// A desktop menu, or a combo box's drop-down: a popup of rows
// over the compositor's blur, driven by the pointer or the keyboard.
// Choosing a row, Escape or a press elsewhere closes it. Submenus open in
// popups of their own beside their rows; only the root menu grabs, and keys
// go to the innermost open menu.
//
// A menu is also a main menu, the one the status bar shows for the
// application (Application::setMainMenu): its rows are the bar's entries and
// their submenus open from them, onAboutToShow before each opens.
class Menu
{
public:
    Menu();
    ~Menu();

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    // Returns the row's index. The shortcut is only shown, to the right.
    size_t addItem(std::string text, std::function<void()> action, std::string shortcut = {});
    void addSeparator();
    // A row opening a menu of its own beside it, on hover, Right, Return or a
    // click. The submenu belongs to this menu; fill it here or from its
    // onAboutToShow. Returns it.
    Menu* addSubmenu(std::string text);
    Menu* submenu(size_t index) const;
    void setItemText(size_t index, std::string text);
    void setItemEnabled(size_t index, bool enabled);
    void setItemVisible(size_t index, bool visible);
    // A checked row gives every row a leading column, so the labels line up.
    void setItemChecked(size_t index, bool checked);
    // Mixed shows as unchecked in the menu's own popup.
    void setItemState(size_t index, MenuItemState state);
    // The key that chooses the row, a character or a key name (Return, F5),
    // with ui::KeyModifier flags; shown as the row's shortcut. The program
    // handles the key itself. An empty key for none.
    void setItemKeyEquivalent(size_t index, std::string key, uint32_t modifiers);
    // The text shown to the right, in place of what setItemKeyEquivalent
    // writes there; the key equivalent itself stays.
    void setItemShortcut(size_t index, std::string text);
    // An icon from the icon theme before the text.
    void setItemIcon(size_t index, std::string icon);
    // One of the toolkit's glyphs before the text, tinted like it.
    void setItemGlyph(size_t index, std::string glyph);
    // The row's text in this family, as a font list previews its fonts.
    void setItemFontFamily(size_t index, std::string family);
    // The row at most this wide, its text elided.
    void setItemMaximumWidth(size_t index, double width);
    // What the menu is for as a submenu of a main menu; none by default.
    void setRole(MenuRole role);
    MenuRole role() const { return m_role; }
    // As for a drop-down, never narrower than what it hangs off.
    void setMinimumWidth(double width) { m_minimumWidth = width; }
    // A drop-down rather than a desktop menu: at most half the screen tall
    // (240 at least) with a scroll bar while it scrolls, opened on the
    // checked row, rows found by typing their start.
    void setDropDown(bool dropDown) { m_dropDown = dropDown; }
    void clear();
    // After rows were added, removed or changed while the menu is open: on the
    // next loop turn the rows are built again from the items, the popup resized
    // (the host places it again), the lit row kept by index while it is still
    // there, and an open submenu kept while its row is still shown. Nothing
    // happens when nothing changed since the menu opened or was last rebuilt.
    void itemsChanged();

    // A bubble menu: an arrow on this edge pointing at the target, in the
    // coordinates of the window it opens in. None by default.
    void setArrow(BubbleShape::Edge edge, PointF target);
    // Continuous corners on the body; circular by default.
    void setContinuousCorners(bool continuous) { m_continuousCorners = continuous; }
    // Where the menu stays, in the window's coordinates, as
    // MenuPopupWindow's availableGeometry; unset, the window's screen size.
    void setAvailableGeometry(std::optional<RectF> geometry) { m_availableGeometry = geometry; }
    // The size it opens at in the window, arrow included: the content's
    // implicit size after ensureLayout(). Runs no onAboutToShow.
    SizeF implicitSize(Window* window);
    // The shortest arrow edge that still shows the arrow.
    double minimumEdgeLength() const;
    // With its top-left at a point of the window, kept six pixels inside the
    // available geometry, as MenuPopupWindow::showAt.
    void popupAt(Window* window, PointF topLeft);

    // At a point of the window, usually where the pointer is.
    void popup(Window* window, PointF position);
    // gap pixels under the view, or under a rect of it in its coordinates,
    // from its left edge; above it when there is more room there. Opened
    // from the keyboard, a drop-down lights its checked row.
    void popupFor(View* view, double gap = 3, std::optional<RectF> rect = {}, bool fromKeyboard = false);
    void close();
    bool isOpen() const { return m_window != nullptr; }
    // The popup while open, for tests.
    Window* window() const { return m_window.get(); }

    std::function<void()> onClosed;
    // Before the menu opens, as a D-Bus menu's aboutToShow: rows added here
    // show. A submenu with no visible row opens only when this is set.
    std::function<void()> onAboutToShow;
    // Left in a root menu, Right on a row without a submenu: a menu bar walks
    // to its neighbouring menu.
    std::function<void()> onNavigateLeft;
    std::function<void()> onNavigateRight;

private:
    class Rows;
    class Surface;
    friend class Rows;
    friend class Surface;
    friend class wayland::MainMenuMirror;

    struct Item
    {
        std::string text;
        std::string shortcut;
        std::string keyEquivalent;
        uint32_t keyModifiers = 0;
        std::string icon;
        std::string glyph;
        std::string fontFamily;
        std::function<void()> action;
        std::unique_ptr<Menu> submenu;
        double maximumWidth = 0;
        bool separator = false;
        bool enabled = true;
        bool visible = true;
        bool checked = false;
        bool mixed = false;
    };

    // Tells a main menu showing this menu that its rows changed.
    void changed();

    void open(Window* parent, const PopupPlacement& placement, bool fromKeyboard, bool grab = true);
    // The bubble as it is drawn at a size, its arrow at the position.
    BubbleShape bubble(SizeF size, double arrowPosition) const;
    // The popup's size, capped to the room the window's screen leaves.
    SizeF popupSize(Window* parent, SizeF content) const;
    // Where the open's top-left is, for the arrow.
    std::optional<PointF> m_openAt;
    // popupAt() ran onAboutToShow already for this opening.
    bool m_aboutToShowDone = false;
    // The items as the open menu shows them, to tell a change.
    std::string m_shownItems;
    bool m_rebuildPosted = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::string itemsSignature() const;
    void rebuild();
    void applyBlur(SizeF size, double arrowPosition);
    void activate(size_t index);
    Rows* rows() const;
    Menu* rootMenu();
    // The open submenu, and closing it.
    void closeSubmenu();
    // Closes a hover-opened submenu the pointer is neither in nor on the row of.
    void checkSubmenuHover();
    void scheduleSubmenuCheck();

    std::vector<Item> m_items;
    std::unique_ptr<Window> m_window;
    // The menu this one is a submenu of.
    Menu* m_parentMenu = nullptr;
    Menu* m_openSubmenu = nullptr;
    bool m_submenuFromKeyboard = false;
    int m_submenuTimer = 0;
    double m_minimumWidth = 0;
    bool m_dropDown = false;
    BubbleShape::Edge m_arrowEdge = BubbleShape::Edge::None;
    PointF m_arrowTarget;
    bool m_continuousCorners = false;
    std::optional<RectF> m_availableGeometry;
    MenuRole m_role = MenuRole::None;
    // Set while a main menu shows this menu.
    std::function<void()> m_changed;
};

} // namespace cutefish::ui
