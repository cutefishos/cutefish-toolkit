// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/desktop_entry.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "base/file.h"
#include "base/locale.h"
#include "base/paths.h"
#include "base/settings.h"

namespace cutefish::base {

static std::string trimmed(std::string_view text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
        return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(start, end - start + 1));
}

static bool equalsIgnoringCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

static bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

static std::vector<std::string> splitList(const std::string& value)
{
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(';', start);
        if (end == std::string::npos)
            end = value.size();
        if (end > start)
            result.push_back(trimmed(std::string_view(value).substr(start, end - start)));
        start = end + 1;
    }
    return result;
}

// One pass, so an escaped backslash cannot start another escape.
static std::string unescapeValue(std::string_view value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c != '\\' || i + 1 == value.size()) {
            result += c;
            continue;
        }
        const char next = value[++i];
        switch (next) {
        case 'n':
            result += '\n';
            break;
        case 't':
            result += '\t';
            break;
        case 'r':
            result += '\r';
            break;
        case 's':
            result += ' ';
            break;
        case ';':
            result += ';';
            break;
        case '\\':
            result += '\\';
            break;
        default:
            result += c;
            result += next;
            break;
        }
    }
    return result;
}

static bool parseBool(const std::string& value)
{
    return equalsIgnoringCase(value, "true");
}

static bool listContainsDesktop(const std::vector<std::string>& list, const std::vector<std::string>& desktops)
{
    for (const std::string& wanted : list) {
        for (const std::string& current : desktops) {
            if (equalsIgnoringCase(wanted, current))
                return true;
        }
    }
    return false;
}

// XDG locale matching order: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang.
static std::vector<std::string> localeCandidates(const std::string& language)
{
    const size_t modifierStart = language.find('@');
    const std::string modifier = modifierStart == std::string::npos ? std::string() : language.substr(modifierStart);
    const std::string name = language.substr(0, modifierStart);
    const std::string base = name.substr(0, name.find('_'));
    std::string bcp47 = name;
    std::replace(bcp47.begin(), bcp47.end(), '_', '-');

    std::vector<std::string> candidates;
    for (const std::string& candidate : {name + modifier, name, base + modifier, base, bcp47}) {
        if (!candidate.empty() && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
            candidates.push_back(candidate);
    }
    return candidates;
}

using Values = std::map<std::string, std::string>;

static std::string valueOf(const Values& values, const std::string& key)
{
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

static std::string localizedValue(const Values& values, const Values& localized, const std::string& key,
                                  const std::vector<std::string>& candidates)
{
    for (const std::string& candidate : candidates) {
        const auto it = localized.find(key + "[" + candidate + "]");
        if (it != localized.end())
            return it->second;
    }
    return valueOf(values, key);
}

static std::vector<std::string> tokenizeExec(const std::string& exec)
{
    std::vector<std::string> result;
    std::string token;
    bool quoted = false;
    char quote = 0;
    // A quoted empty argument is an argument too.
    bool started = false;

    for (size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (c == '\\' && i + 1 < exec.size()) {
            token += exec[++i];
            continue;
        }
        if (c == '"' || c == '\'') {
            if (!quoted) {
                quoted = true;
                started = true;
                quote = c;
            } else if (quote == c) {
                quoted = false;
            } else {
                token += c;
            }
            continue;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty() || started) {
                result.push_back(token);
                token.clear();
                started = false;
            }
            continue;
        }
        token += c;
    }
    if (!token.empty() || started)
        result.push_back(token);
    return result;
}

static std::string substituteToken(const std::string& token, const DesktopEntry& entry,
                                   const std::vector<std::string>& arguments)
{
    std::string result;
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '%' || i + 1 >= token.size()) {
            result += token[i];
            continue;
        }
        const char field = token[++i];
        if (field == '%') {
            result += '%';
        } else if (field == 'c') {
            result += entry.name;
        } else if (field == 'k') {
            result += entry.path;
        } else if (field == 'f' || field == 'u' || field == 'F' || field == 'U') {
            // Inside a larger token a list code can take only one argument.
            if (!arguments.empty())
                result += arguments.front();
        }
        // %i is handled as two arguments by commandForArguments().
    }
    return result;
}

std::vector<std::string> DesktopEntry::currentDesktops()
{
    return {"Cutefish"};
}

std::string DesktopEntry::displayLanguage()
{
    // The session language lives in cutefish-services' settings.
    const Settings settings(paths::cutefishConfig("services"));
    std::string language = settings.stringValue("Language/language");
    if (language.empty())
        return Locale::systemLanguage();
    std::replace(language.begin(), language.end(), '-', '_');
    return language;
}

bool DesktopEntry::shouldShow() const
{
    return shouldShow(currentDesktops());
}

bool DesktopEntry::shouldShow(const std::vector<std::string>& desktops) const
{
    // Hidden means the entry is deleted; it only exists to mask a system one.
    if (hidden || noDisplay)
        return false;
    if (exec.empty() && !dbusActivatable)
        return false;
    if (!tryExec.empty() && paths::findExecutable(tryExec).empty())
        return false;
    if (!onlyShowIn.empty() && !listContainsDesktop(onlyShowIn, desktops))
        return false;
    if (listContainsDesktop(notShowIn, desktops))
        return false;
    return true;
}

std::vector<std::string> DesktopEntry::commandForArguments(const std::vector<std::string>& arguments) const
{
    std::vector<std::string> result;
    for (const std::string& token : tokenizeExec(exec)) {
        if (token == "%F" || token == "%U") {
            result.insert(result.end(), arguments.begin(), arguments.end());
            continue;
        }
        if (token == "%f" || token == "%u") {
            if (!arguments.empty())
                result.push_back(arguments.front());
            continue;
        }
        if (token == "%i") {
            if (!icon.empty()) {
                result.push_back("--icon");
                result.push_back(icon);
            }
            continue;
        }
        std::string substituted = substituteToken(token, *this, arguments);
        if (!substituted.empty() || token.empty())
            result.push_back(std::move(substituted));
    }
    return result;
}

std::vector<std::string> DesktopEntry::parseExec(const std::string& exec)
{
    DesktopEntry entry;
    entry.exec = exec;
    return entry.command();
}

std::optional<DesktopEntry> DesktopEntry::parse(const std::string& id, const std::string& path,
                                                std::string_view contents, const std::string& language)
{
    const std::vector<std::string> candidates = localeCandidates(language.empty() ? displayLanguage() : language);
    static constexpr std::string_view kActionPrefix = "Desktop Action ";

    DesktopEntry data;
    data.id = id;
    data.path = path;

    std::string group;
    Values values;
    Values localized;
    std::map<std::string, Values> actionValues;

    const auto finishGroup = [&] {
        if (group == "Desktop Entry") {
            data.type = valueOf(values, "Type");
            data.name = localizedValue(values, localized, "Name", candidates);
            data.genericName = localizedValue(values, localized, "GenericName", candidates);
            data.comment = localizedValue(values, localized, "Comment", candidates);
            data.icon = valueOf(values, "Icon");
            data.exec = valueOf(values, "Exec");
            data.tryExec = valueOf(values, "TryExec");
            data.workingDirectory = valueOf(values, "Path");
            data.startupWMClass = valueOf(values, "StartupWMClass");
            data.categories = splitList(valueOf(values, "Categories"));
            data.keywords = splitList(valueOf(values, "Keywords"));
            data.mimeTypes = splitList(valueOf(values, "MimeType"));
            data.onlyShowIn = splitList(valueOf(values, "OnlyShowIn"));
            data.notShowIn = splitList(valueOf(values, "NotShowIn"));
            data.terminal = parseBool(valueOf(values, "Terminal"));
            data.noDisplay = parseBool(valueOf(values, "NoDisplay"));
            data.hidden = parseBool(valueOf(values, "Hidden"));
            data.dbusActivatable = parseBool(valueOf(values, "DBusActivatable"));
            data.startupNotify = !equalsIgnoringCase(valueOf(values, "StartupNotify"), "false");
        } else if (startsWith(group, kActionPrefix)) {
            const std::string actionId = group.substr(kActionPrefix.size());
            const auto it = actionValues.find(actionId);
            if (it != actionValues.end() && !it->second.empty()) {
                const Values& action = it->second;
                data.actions.push_back(DesktopAction{actionId, localizedValue(action, action, "Name", candidates),
                                                     valueOf(action, "Icon"), valueOf(action, "Exec")});
            }
        }
        values.clear();
        localized.clear();
    };

    size_t start = 0;
    while (start < contents.size()) {
        size_t end = contents.find('\n', start);
        if (end == std::string_view::npos)
            end = contents.size();
        const std::string line = trimmed(contents.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#')
            continue;

        if (line.front() == '[' && line.back() == ']') {
            finishGroup();
            group = line.substr(1, line.size() - 2);
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == 0 || equals == std::string::npos)
            continue;
        const std::string key = trimmed(std::string_view(line).substr(0, equals));
        std::string value = unescapeValue(trimmed(std::string_view(line).substr(equals + 1)));
        if (startsWith(group, kActionPrefix)) {
            actionValues[group.substr(kActionPrefix.size())][key] = std::move(value);
        } else if (group == "Desktop Entry") {
            const size_t localeStart = key.find('[');
            if (localeStart != std::string::npos && localeStart > 0 && key.back() == ']')
                localized[key] = std::move(value);
            else
                values[key] = std::move(value);
        }
    }
    finishGroup();

    // A Hidden entry is kept without a Name so it still masks the system entry
    // of the same id further down the data directories.
    if (data.type != "Application")
        return std::nullopt;
    if (data.name.empty() && !data.hidden)
        return std::nullopt;
    return data;
}

} // namespace cutefish::base
