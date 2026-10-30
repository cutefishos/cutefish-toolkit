// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "wayland/main_menu.h"

#include <cutefish-main-menu-v1-client-protocol.h>

#include "base/event_loop.h"
#include "ui/event.h"
#include "wayland/connection.h"

namespace cutefish::ui::wayland {

static uint32_t protocolRole(MenuRole role)
{
    switch (role) {
    case MenuRole::Application:
        return CUTEFISH_MENU_V1_ROLE_APPLICATION;
    case MenuRole::Windows:
        return CUTEFISH_MENU_V1_ROLE_WINDOWS;
    case MenuRole::Help:
        return CUTEFISH_MENU_V1_ROLE_HELP;
    case MenuRole::Services:
        return CUTEFISH_MENU_V1_ROLE_SERVICES;
    case MenuRole::None:
        break;
    }
    return CUTEFISH_MENU_V1_ROLE_NONE;
}

static uint32_t protocolState(MenuItemState state)
{
    switch (state) {
    case MenuItemState::On:
        return CUTEFISH_MENU_ITEM_V1_STATE_ON;
    case MenuItemState::Mixed:
        return CUTEFISH_MENU_ITEM_V1_STATE_MIXED;
    case MenuItemState::Off:
        break;
    }
    return CUTEFISH_MENU_ITEM_V1_STATE_OFF;
}

static uint32_t protocolModifiers(uint32_t modifiers)
{
    uint32_t result = 0;
    if (modifiers & ShiftModifier)
        result |= CUTEFISH_MENU_ITEM_V1_MODIFIER_SHIFT;
    if (modifiers & ControlModifier)
        result |= CUTEFISH_MENU_ITEM_V1_MODIFIER_CONTROL;
    if (modifiers & AltModifier)
        result |= CUTEFISH_MENU_ITEM_V1_MODIFIER_ALT;
    if (modifiers & SuperModifier)
        result |= CUTEFISH_MENU_ITEM_V1_MODIFIER_SUPER;
    return result;
}

struct MainMenuListeners
{
    static constexpr cutefish_menu_v1_listener menu = {
        MainMenuMirror::willOpen,
        MainMenuMirror::closed,
    };
    static constexpr cutefish_menu_item_v1_listener item = {
        MainMenuMirror::activated,
    };
};

MainMenuMirror::MainMenuMirror(Connection& connection, Menu* menu)
    : m_connection(connection)
    , m_menu(menu)
    , m_alive(menu->m_alive)
{
    m_proxy = cutefish_main_menu_manager_v1_create_menu(connection.mainMenus());
    cutefish_menu_v1_add_listener(m_proxy, &MainMenuListeners::menu, this);
    std::weak_ptr<bool> self = m_self;
    menu->m_changed = [this, self] {
        if (!self.expired())
            scheduleSync();
    };
    sync();
}

MainMenuMirror::~MainMenuMirror()
{
    if (isAlive())
        m_menu->m_changed = nullptr;
    destroyRows();
    cutefish_menu_v1_destroy(m_proxy);
}

void MainMenuMirror::scheduleSync()
{
    if (m_syncPosted)
        return;
    m_syncPosted = true;
    // The program may be changing several rows at once.
    m_connection.loop().post([self = std::weak_ptr<bool>(m_self), this] {
        if (!self.expired())
            sync();
    });
}

void MainMenuMirror::sync()
{
    m_syncPosted = false;
    if (!isAlive()) {
        destroyRows();
        return;
    }
    if (m_menu->m_role != m_role) {
        m_role = m_menu->m_role;
        cutefish_menu_v1_set_role(m_proxy, protocolRole(m_role));
    }
    const std::vector<Menu::Item>& items = m_menu->m_items;
    bool same = items.size() == m_rows.size();
    for (size_t i = 0; same && i < items.size(); ++i)
        same = m_rows[i]->separator == items[i].separator && m_rows[i]->submenuSource == items[i].submenu.get();
    if (!same) {
        rebuild();
        return;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        update(*m_rows[i], items[i], false);
        if (m_rows[i]->submenu)
            m_rows[i]->submenu->sync();
    }
}

void MainMenuMirror::rebuild()
{
    destroyRows();
    const std::vector<Menu::Item>& items = m_menu->m_items;
    for (size_t i = 0; i < items.size(); ++i) {
        auto row = std::make_unique<Row>();
        row->owner = this;
        row->separator = items[i].separator;
        row->proxy = cutefish_main_menu_manager_v1_create_item(
            m_connection.mainMenus(),
            row->separator ? CUTEFISH_MENU_ITEM_V1_KIND_SEPARATOR : CUTEFISH_MENU_ITEM_V1_KIND_NORMAL);
        cutefish_menu_item_v1_add_listener(row->proxy, &MainMenuListeners::item, row.get());
        update(*row, items[i], true);
        if (items[i].submenu) {
            row->submenuSource = items[i].submenu.get();
            row->submenu = std::make_unique<MainMenuMirror>(m_connection, row->submenuSource);
            cutefish_menu_item_v1_set_submenu(row->proxy, row->submenu->proxy());
        }
        cutefish_menu_v1_insert_item(m_proxy, row->proxy, uint32_t(i));
        m_rows.push_back(std::move(row));
    }
}

void MainMenuMirror::update(Row& row, const Menu::Item& item, bool force)
{
    if (row.separator)
        return;
    if (force || row.text != item.text) {
        row.text = item.text;
        cutefish_menu_item_v1_set_title(row.proxy, row.text.c_str());
    }
    if (force || row.enabled != item.enabled) {
        row.enabled = item.enabled;
        cutefish_menu_item_v1_set_enabled(row.proxy, row.enabled ? 1 : 0);
    }
    if (force || row.visible != item.visible) {
        row.visible = item.visible;
        cutefish_menu_item_v1_set_hidden(row.proxy, row.visible ? 0 : 1);
    }
    const MenuItemState state = item.mixed ? MenuItemState::Mixed
        : item.checked                      ? MenuItemState::On
                                            : MenuItemState::Off;
    if (force || row.state != state) {
        row.state = state;
        cutefish_menu_item_v1_set_state(row.proxy, protocolState(state));
    }
    if (force || row.icon != item.icon) {
        row.icon = item.icon;
        cutefish_menu_item_v1_set_icon_name(row.proxy, row.icon.c_str());
    }
    if (force || row.key != item.keyEquivalent || row.modifiers != item.keyModifiers) {
        row.key = item.keyEquivalent;
        row.modifiers = item.keyModifiers;
        cutefish_menu_item_v1_set_key_equivalent(row.proxy, row.key.c_str(), protocolModifiers(row.modifiers));
    }
}

void MainMenuMirror::destroyRows()
{
    for (const std::unique_ptr<Row>& row : m_rows) {
        cutefish_menu_item_v1_destroy(row->proxy);
        row->submenu.reset();
    }
    m_rows.clear();
}

void MainMenuMirror::willOpen(void* data, cutefish_menu_v1*, uint32_t serial)
{
    auto* mirror = static_cast<MainMenuMirror*>(data);
    std::weak_ptr<bool> self = mirror->m_self;
    if (mirror->isAlive() && mirror->m_menu->onAboutToShow) {
        // The handler may change the menus, this one included.
        const std::function<void()> handler = mirror->m_menu->onAboutToShow;
        handler();
    }
    if (self.expired())
        return;
    mirror->sync();
    cutefish_menu_v1_ready(mirror->m_proxy, serial);
}

void MainMenuMirror::closed(void* data, cutefish_menu_v1*)
{
    auto* mirror = static_cast<MainMenuMirror*>(data);
    if (mirror->isAlive() && mirror->m_menu->onClosed) {
        const std::function<void()> handler = mirror->m_menu->onClosed;
        handler();
    }
}

void MainMenuMirror::activated(void* data, cutefish_menu_item_v1*, uint32_t serial)
{
    auto* row = static_cast<Row*>(data);
    MainMenuMirror* mirror = row->owner;
    if (!mirror->isAlive())
        return;
    if (serial)
        mirror->m_connection.setInputSerial(serial);
    // The rows follow the menu's items one to one.
    for (size_t i = 0; i < mirror->m_rows.size(); ++i) {
        if (mirror->m_rows[i].get() != row)
            continue;
        const std::vector<Menu::Item>& items = mirror->m_menu->m_items;
        if (i < items.size() && items[i].action && items[i].enabled) {
            // The action may change or destroy the menu.
            const std::function<void()> action = items[i].action;
            action();
        }
        return;
    }
}

} // namespace cutefish::ui::wayland
