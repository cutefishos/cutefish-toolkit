// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/shortcut_convention.h"

#include "base/dbus.h"

namespace cutefish::ui {

namespace {

struct Convention
{
    bool command = false;
    base::dbus::Proxy proxy;
    base::dbus::Match changed;
    base::dbus::NameWatch watch;
    base::dbus::PendingCall pending;

    Convention()
    {
        base::dbus::Connection* bus = base::dbus::Connection::session();
        if (!bus)
            return;
        proxy = base::dbus::Proxy(bus, "org.cutefish.Keyboard1", "/org/cutefish/Keyboard1", "org.cutefish.Keyboard1");
        changed = proxy.onSignal("shortcutConventionChanged", [this](const base::dbus::Message&) { refresh(); });
        watch = bus->watchName("org.cutefish.Keyboard1", [this](const std::string& owner) {
            if (!owner.empty())
                refresh();
        });
    }

    void refresh()
    {
        pending = proxy.getPropertyAsync("shortcutConvention", [this](const base::dbus::Reply& reply) {
            if (const auto value = reply.value<base::dbus::Variant>())
                command = value->value<std::string>() == "command";
        });
    }
};

} // namespace

bool commandKeys()
{
    static Convention convention;
    return convention.command;
}

} // namespace cutefish::ui
