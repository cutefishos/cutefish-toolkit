// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui/menu.h"

struct cutefish_menu_v1;
struct cutefish_menu_item_v1;

namespace cutefish::ui::wayland {

class Connection;
struct MainMenuListeners;

// One ui::Menu as a cutefish_menu_v1, its rows as items and their submenus
// mirrored in turn. Rows follow the menu on the next loop turn after it
// changes; before the status bar opens it, the menu's onAboutToShow runs
// and the rows follow at once. Every row is mirrored, hidden ones too, so
// an item's place is its row's.
class MainMenuMirror
{
public:
    MainMenuMirror(Connection& connection, Menu* menu);
    ~MainMenuMirror();

    MainMenuMirror(const MainMenuMirror&) = delete;
    MainMenuMirror& operator=(const MainMenuMirror&) = delete;

    cutefish_menu_v1* proxy() const { return m_proxy; }
    Menu* menu() const { return m_menu; }
    // The menu still exists.
    bool isAlive() const { return !m_alive.expired(); }

    // Brings the items up to date with the rows, and the submenus with theirs.
    void sync();

private:
    friend struct MainMenuListeners;

    struct Row
    {
        MainMenuMirror* owner = nullptr;
        cutefish_menu_item_v1* proxy = nullptr;
        bool separator = false;
        Menu* submenuSource = nullptr;
        std::unique_ptr<MainMenuMirror> submenu;
        std::string text;
        std::string icon;
        std::string key;
        uint32_t modifiers = 0;
        bool enabled = true;
        bool visible = true;
        MenuItemState state = MenuItemState::Off;
    };

    static void willOpen(void* data, cutefish_menu_v1* menu, uint32_t serial);
    static void closed(void* data, cutefish_menu_v1* menu);
    static void activated(void* data, cutefish_menu_item_v1* item, uint32_t serial);

    void scheduleSync();
    void rebuild();
    void update(Row& row, const Menu::Item& item, bool force);
    void destroyRows();

    Connection& m_connection;
    Menu* m_menu;
    std::weak_ptr<bool> m_alive;
    cutefish_menu_v1* m_proxy = nullptr;
    std::vector<std::unique_ptr<Row>> m_rows;
    MenuRole m_role = MenuRole::None;
    bool m_syncPosted = false;
    std::shared_ptr<bool> m_self = std::make_shared<bool>(true);
};

} // namespace cutefish::ui::wayland
