// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <map>
#include <memory>
#include <string>

#include "base/signal.h"

namespace cutefish::base {

namespace dbus {
class Connection;
}

// Follows the session language and region that cutefish-services keeps:
// when they change, the process's language variables, locale and regional
// preferences (Locale::setSystemPreferences) are set again and changed runs.
// ui::Application holds one; a process without it, such as a daemon, makes
// its own.
class SessionLanguage
{
public:
    explicit SessionLanguage(dbus::Connection* bus);
    ~SessionLanguage();

    SessionLanguage(const SessionLanguage&) = delete;
    SessionLanguage& operator=(const SessionLanguage&) = delete;

    Signal<> changed;

    // LANG, LANGUAGE and the LC_ variables, an empty value removing one; then
    // setlocale() and Locale::system() again. False when nothing differed.
    static bool apply(const std::map<std::string, std::string>& variables);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace cutefish::base
