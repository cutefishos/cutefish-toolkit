// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "base/desktop_entry.h"

// The user's mimeapps.list and xdg-terminals.list, edited line by line so
// comments and entries this code does not touch survive; a port of
// cutefish-framework's MimeApps. Desktop entries come from
// ApplicationRegistry, so ask once it has loaded. MIME types are named, and
// an alias stands for its type.
namespace cutefish::base::mimeapps {

// The user's files under ~/.config, which every choice is written to.
std::string userListPath();
std::string userTerminalsListPath();
// Every mimeapps.list that applies, the user's first: the config
// directories, then the applications directories, each with the current
// desktops' own list before the plain one.
std::vector<std::string> listPaths();

// How the lists name an entry: its desktop file id, or a bundle's path, which
// names it only while it is installed.
std::string listedId(const DesktopEntry& entry);

std::vector<std::string> associations(const std::string& path, const std::string& group,
                                      const std::string& mimeType);
bool setDefault(const std::string& path, const std::string& mimeType, const std::string& desktopId);

// Desktop file paths of the installed applications that open the type: those
// the lists add, then those listing it themselves, else its parents a
// generation at a time, earliest installed first. An application a list
// removes is left out, unless an earlier list adds it.
std::vector<std::string> applications(const std::string& mimeType, const std::vector<std::string>& paths);
// The installed application the first list naming one chooses for the type,
// or an empty string. An application removed by an earlier list is not
// chosen by a later one.
std::string chosenApplication(const std::vector<std::string>& paths, const std::string& mimeType);
// What opens the type: the choice in the lists, else the first of applications().
std::string defaultApplication(const std::vector<std::string>& paths, const std::string& mimeType);

// Starts what opens the URI's scheme (its x-scheme-handler type, as the
// lists and the entries' MimeType give it) with the URI, once the installed
// applications are read, through org.cutefish.AppRuntime with the activation
// token, or itself when the runtime is not on the bus; done(false) when
// nothing does.
void openUri(const std::string& uri, const std::string& activationToken = {},
             std::function<void(bool started)> done = {}, const std::vector<std::string>& paths = listPaths());

// xdg-terminals.list of the xdg-terminal-exec spec: desktop entry IDs, most
// preferred first.
std::string defaultTerminal(const std::string& path);
bool setDefaultTerminal(const std::string& path, const std::string& desktopId);

} // namespace cutefish::base::mimeapps
