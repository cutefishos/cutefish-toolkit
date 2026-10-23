// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/paths.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <pwd.h>
#include <regex>
#include <unistd.h>

#include "base/file.h"

namespace cutefish::base::paths {

std::string home()
{
    static const std::string directory = [] {
        passwd entry{};
        passwd* result = nullptr;
        char buffer[4096];
        if (getpwuid_r(getuid(), &entry, buffer, sizeof(buffer), &result) == 0 && result && result->pw_dir)
            return std::string(result->pw_dir);
        return std::string("/");
    }();
    return directory;
}

std::string configHome()
{
    return home() + "/.config";
}

std::string dataHome()
{
    if (const char* overriden = std::getenv("XDG_DATA_HOME"))
        if (*overriden)
            return overriden;
    return home() + "/.local/share";
}

std::string cacheHome()
{
    return home() + "/.cache";
}

std::string stateHome()
{
    return home() + "/.local/state";
}

std::string runtimeDirectory()
{
    return "/run/user/" + std::to_string(getuid());
}

std::vector<std::string> dataDirectories()
{
    const char* overriden = std::getenv("XDG_DATA_DIRS");
    const std::string directories =
        (overriden && *overriden) ? overriden : "/usr/local/share:/usr/share";

    // Empty entries name no directory; the rest keep their order.
    std::vector<std::string> result;
    size_t position = 0;
    while (position <= directories.size()) {
        const size_t end = directories.find(':', position);
        const std::string directory = directories.substr(
            position, end == std::string::npos ? end : end - position);
        if (!directory.empty()
            && std::find(result.begin(), result.end(), directory) == result.end())
            result.push_back(directory);
        if (end == std::string::npos)
            break;
        position = end + 1;
    }
    return result;
}

std::vector<std::string> configDirectories()
{
    return {"/etc/xdg"};
}

std::string cutefishConfig(const std::string& name)
{
    return configHome() + "/cutefish/" + name + ".conf";
}

std::string userDirectory(UserDirectory directory)
{
    return userDirectory(directory, configHome() + "/user-dirs.dirs");
}

std::string userDirectory(UserDirectory directory, const std::string& userDirsFile)
{
    static const struct
    {
        const char* key;
        const char* fallback;
    } names[] = {
        {"DESKTOP", "/Desktop"},     {"DOCUMENTS", "/Documents"}, {"DOWNLOAD", "/Downloads"},
        {"MUSIC", "/Music"},         {"PICTURES", "/Pictures"},   {"PUBLICSHARE", "/Public"},
        {"TEMPLATES", "/Templates"}, {"VIDEOS", "/Videos"},
    };
    const auto& name = names[size_t(directory)];

    // The last line of the form XDG_<KEY>_DIR=<value> counts, its quotes and
    // one trailing slash go, and a leading $HOME is the home directory, whether
    // a slash follows or not.
    if (const std::optional<std::string> contents = file::read(userDirsFile)) {
        static const std::regex line("^XDG_(.*)_DIR=(.*)$");
        std::optional<std::string> result;
        size_t start = 0;
        while (start < contents->size()) {
            size_t end = contents->find('\n', start);
            if (end == std::string::npos)
                end = contents->size();
            std::string text = contents->substr(start, end - start);
            start = end + 1;
            if (!text.empty() && text.back() == '\r')
                text.pop_back();
            std::smatch match;
            if (!std::regex_match(text, match, line) || match[1].str() != name.key)
                continue;
            std::string value = match[2].str();
            if (value.size() > 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            if (value.starts_with("$HOME"))
                value = home() + value.substr(5);
            if (value.size() > 1 && value.back() == '/')
                value.pop_back();
            result = value;
        }
        if (result)
            return *result;
    }
    return home() + name.fallback;
}

std::string findExecutable(const std::string& program)
{
    if (program.empty())
        return {};
    if (program.find('/') != std::string::npos)
        return access(program.c_str(), X_OK) == 0 ? program : std::string();

    const std::string directories[] = {home() + "/.local/bin", "/usr/local/sbin", "/usr/local/bin",
                                       "/usr/sbin", "/usr/bin", "/sbin", "/bin"};
    for (const std::string& directory : directories) {
        const std::string path = directory + "/" + program;
        if (access(path.c_str(), X_OK) == 0)
            return path;
    }
    return {};
}

} // namespace cutefish::base::paths
