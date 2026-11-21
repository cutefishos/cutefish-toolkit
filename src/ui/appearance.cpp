// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/appearance.h"

#include <cstring>
#include <string>
#include <systemd/sd-bus.h>

#include "base/dbus.h"
#include "ui/theme.h"

namespace cutefish::ui {

static constexpr const char* kService = "org.cutefish.Services";
static constexpr const char* kPath = "/org/cutefish/Services/Appearance";
static constexpr const char* kInterface = "org.cutefish.Services.Appearance";

// The accents, in the order of the setting's index.
static const Color kAccents[] = {
    accent::blue, accent::red, accent::green, accent::purple, accent::pink, accent::orange, accent::grey,
};

AppearanceWatcher::AppearanceWatcher(base::dbus::Connection& connection)
    : m_connection(connection)
{
    sd_bus* bus = connection.bus();
    sd_bus_match_signal_async(bus, &m_match, kService, kPath, kInterface, nullptr,
                              &AppearanceWatcher::changed, nullptr, this);
    sd_bus_call_method_async(bus, &m_call, kService, kPath, "org.freedesktop.DBus.Properties", "GetAll",
                             &AppearanceWatcher::replied, this, "s", kInterface);
}

void AppearanceWatcher::advance()
{
    while (sd_bus_process(m_connection.bus(), nullptr) > 0) {
    }
}

void AppearanceWatcher::waitForSettings()
{
    sd_bus* bus = m_connection.bus();
    while (m_call) {
        const int r = sd_bus_process(bus, nullptr);
        if (r < 0)
            break;
        if (r == 0 && sd_bus_wait(bus, UINT64_MAX) < 0)
            break;
    }
    m_waited = true;
    if (m_firstReply) {
        apply(m_firstReply);
        m_firstReply = sd_bus_message_unref(m_firstReply);
    }
}

AppearanceWatcher::~AppearanceWatcher()
{
    sd_bus_message_unref(m_firstReply);
    sd_bus_slot_unref(m_call);
    sd_bus_slot_unref(m_match);
}

int AppearanceWatcher::changed(sd_bus_message* message, void* data, sd_bus_error*)
{
    auto* self = static_cast<AppearanceWatcher*>(data);
    const char* member = sd_bus_message_get_member(message);
    // Wallpaper and cursor signals change nothing the toolkit draws.
    if (!member || std::strstr(member, "allpaper") || std::strstr(member, "ursor")
        || std::strstr(member, "ackground"))
        return 0;
    sd_bus_slot_unref(self->m_call);
    self->m_call = nullptr;
    sd_bus_call_method_async(self->m_connection.bus(), &self->m_call, kService, kPath,
                             "org.freedesktop.DBus.Properties", "GetAll",
                             &AppearanceWatcher::replied, self, "s", kInterface);
    return 0;
}

int AppearanceWatcher::replied(sd_bus_message* reply, void* data, sd_bus_error*)
{
    auto* self = static_cast<AppearanceWatcher*>(data);
    if (!sd_bus_message_is_method_error(reply, nullptr)) {
        if (self->m_waited) {
            apply(reply);
        } else {
            sd_bus_message_unref(self->m_firstReply);
            self->m_firstReply = sd_bus_message_ref(reply);
        }
    }
    sd_bus_slot_unref(self->m_call);
    self->m_call = nullptr;
    return 0;
}

void AppearanceWatcher::apply(sd_bus_message* reply)
{
    bool dark = Theme::current().darkMode;
    int accent = 0;
    double pointSize = Theme::current().fontPointSize;
    std::string family = Theme::current().fontFamily;
    std::string fixedFamily = Theme::current().fixedFontFamily;
    std::string iconTheme = Theme::current().iconTheme;
    bool continuous = Theme::current().continuousCorners;

    if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0)
        return;
    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char* name = nullptr;
        sd_bus_message_read(reply, "s", &name);
        const std::string key = name ? name : "";
        const char* contents = nullptr;
        sd_bus_message_peek_type(reply, nullptr, &contents);
        if (sd_bus_message_enter_container(reply, 'v', contents) < 0)
            break;
        const std::string type = contents ? contents : "";
        if (type == "b") {
            int value = 0;
            sd_bus_message_read(reply, "b", &value);
            if (key == "isDarkMode")
                dark = value;
            else if (key == "continuousCorners")
                continuous = value;
        } else if (type == "i") {
            int value = 0;
            sd_bus_message_read(reply, "i", &value);
            if (key == "accentColor")
                accent = value;
        } else if (type == "d") {
            double value = 0;
            sd_bus_message_read(reply, "d", &value);
            if (key == "systemFontPointSize" && value > 0)
                pointSize = value;
        } else if (type == "s") {
            const char* value = nullptr;
            sd_bus_message_read(reply, "s", &value);
            if (key == "systemFont" && value)
                family = value;
            else if (key == "systemFixedFont" && value)
                fixedFamily = value;
            else if (key == "iconTheme" && value && *value)
                iconTheme = value;
        } else {
            sd_bus_message_skip(reply, contents);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);

    Theme theme = dark ? Theme::dark() : Theme::light();
    if (accent >= 0 && accent < int(std::size(kAccents)))
        theme.highlight = kAccents[accent];
    theme.fontPointSize = pointSize;
    theme.fontFamily = family;
    theme.fixedFontFamily = fixedFamily;
    theme.iconTheme = iconTheme;
    theme.continuousCorners = continuous;
    Theme::setCurrent(theme);
}

} // namespace cutefish::ui
