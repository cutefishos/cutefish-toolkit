// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

// The user's and the system's standard directories. They are the XDG base
// directory defaults; the XDG_* variables are not read.
namespace cutefish::base::paths {

std::string home();
// ~/.config
std::string configHome();
// ~/.local/share
std::string dataHome();
// ~/.cache
std::string cacheHome();
// ~/.local/state
std::string stateHome();
// /run/user/<uid>
std::string runtimeDirectory();
// The user's and the system's Flatpak exports, /usr/local/share, /usr/share,
// then /var/lib/snapd/desktop for Snap entries and icons.
std::vector<std::string> dataDirectories();
// /etc/xdg
std::vector<std::string> configDirectories();
// ~/.config/cutefish/<name>.conf, a Cutefish process's own settings.
std::string cutefishConfig(const std::string& name);

enum class UserDirectory {
    Desktop,
    Documents,
    Downloads,
    Music,
    Pictures,
    PublicShare,
    Templates,
    Videos,
};

// The user's choice in ~/.config/user-dirs.dirs, with $HOME expanded, else
// ~/Desktop, ~/Documents, ~/Downloads, ~/Music, ~/Pictures, ~/Public,
// ~/Templates or ~/Videos. The directory may not exist.
std::string userDirectory(UserDirectory directory);
// The same from another user-dirs.dirs file, for tests.
std::string userDirectory(UserDirectory directory, const std::string& userDirsFile);

// The program's path through ~/.local/bin and the system's binary
// directories, then /snap/bin, or itself when it has a slash; empty when not executable.
std::string findExecutable(const std::string& program);

} // namespace cutefish::base::paths
