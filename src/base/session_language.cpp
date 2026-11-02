// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/session_language.h"

#include <cstdlib>
#include <optional>

#include "base/dbus.h"
#include "base/locale.h"

namespace cutefish::base {

static constexpr const char* kService = "org.cutefish.Services";
static constexpr const char* kPath = "/org/cutefish/Services/Language";
static constexpr const char* kInterface = "org.cutefish.Services.Language";

struct SessionLanguage::Private
{
    explicit Private(dbus::Connection* bus)
        : proxy(bus, kService, kPath, kInterface)
    {
    }

    dbus::Proxy proxy;
    dbus::Match match;
    dbus::PendingCall call;
    RegionalPreferences preferences;
};

// The service syncs its settings before it signals a change.
static bool readPreferences(RegionalPreferences& current)
{
    RegionalPreferences preferences = RegionalPreferences::read(RegionalPreferences::sessionPath());
    if (preferences == current)
        return false;
    current = std::move(preferences);
    Locale::setSystemPreferences(current);
    return true;
}

SessionLanguage::SessionLanguage(dbus::Connection* bus)
    : d(std::make_unique<Private>(bus))
{
    d->preferences = RegionalPreferences::read(RegionalPreferences::sessionPath());
    Locale::setSystemPreferences(d->preferences);
    Locale::reloadSystem();

    d->match = d->proxy.onSignal("languageChanged", [this](const dbus::Message&) {
        d->call = d->proxy.getPropertyAsync("environment", [this](const dbus::Reply& reply) {
            const bool region = readPreferences(d->preferences);
            const auto value = reply.value<dbus::Variant>();
            const auto variables = value ? value->get<std::map<std::string, std::string>>() : std::nullopt;
            const bool language = variables && apply(*variables);
            if (region && !language)
                Locale::reloadSystem();
            if (region || language)
                changed.emit();
        });
    });
}

SessionLanguage::~SessionLanguage()
{
    Locale::setSystemPreferences(std::nullopt);
}

bool SessionLanguage::apply(const std::map<std::string, std::string>& variables)
{
    bool differs = false;
    for (const auto& [name, value] : variables) {
        const char* current = std::getenv(name.c_str());
        if (value.empty() ? current != nullptr : !current || value != current) {
            differs = true;
            break;
        }
    }
    if (!differs)
        return false;
    // Threads reading the environment meanwhile are safe as long as only set
    // variables are replaced: the C library keeps their previous strings.
    for (const auto& [name, value] : variables) {
        if (value.empty())
            unsetenv(name.c_str());
        else
            setenv(name.c_str(), value.c_str(), 1);
    }
    Locale::setProcessLocale();
    Locale::reloadSystem();
    return true;
}

} // namespace cutefish::base
