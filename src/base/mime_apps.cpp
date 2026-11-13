// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/mime_apps.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <limits.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "base/application_bundle.h"
#include "base/application_registry.h"
#include "base/dbus.h"
#include "base/desktop_entry.h"
#include "base/event_loop.h"
#include "base/file.h"
#include "base/mime_database.h"
#include "base/paths.h"
#include "base/process.h"
#include "base/signal.h"

namespace cutefish::base::mimeapps {

using Lines = std::vector<std::string>;

namespace {

// A lock file holding the process id, the program's name and the host's on
// three lines, the common format, so other programs editing the list wait.
class LockFile
{
public:
    explicit LockFile(std::string path) : m_path(std::move(path)) {}
    ~LockFile()
    {
        if (m_locked)
            unlink(m_path.c_str());
    }

    bool tryLock()
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int fd = open(m_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            if (fd >= 0) {
                char host[HOST_NAME_MAX + 1] = {};
                gethostname(host, sizeof(host) - 1);
                char name[256] = {};
                const ssize_t length = readlink("/proc/self/exe", name, sizeof(name) - 1);
                const std::string program = length > 0 ? std::filesystem::path(std::string(name, size_t(length))).filename().string()
                                                       : std::string();
                const std::string data = std::to_string(getpid()) + "\n" + program + "\n" + host + "\n";
                const bool written = ::write(fd, data.data(), data.size()) == ssize_t(data.size());
                ::close(fd);
                m_locked = written;
                return written;
            }
            if (errno != EEXIST || !isStale() || unlink(m_path.c_str()) != 0)
                return false;
        }
        return false;
    }

private:
    // Stale when its process is gone or it is older than 30 seconds.
    bool isStale() const
    {
        struct stat info;
        if (stat(m_path.c_str(), &info) != 0)
            return true;
        if (std::time(nullptr) - info.st_mtime > 30)
            return true;
        const std::optional<std::string> contents = file::read(m_path);
        if (!contents)
            return false;
        const long pid = std::strtol(contents->c_str(), nullptr, 10);
        return pid > 0 && ::kill(pid_t(pid), 0) != 0 && errno == ESRCH;
    }

    std::string m_path;
    bool m_locked = false;
};

// The lists' additions for the type that no earlier list removed, and every
// removal, which also applies to the applications listing the type themselves.
struct ListedAssociations
{
    std::vector<std::string> added;
    std::vector<std::string> removed;
};

} // namespace

static bool contains(const std::vector<std::string>& list, const std::string& value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

// Without leading and trailing white space.
static std::string trimmed(std::string_view text)
{
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    size_t start = 0;
    size_t end = text.size();
    while (start < end && space(text[start]))
        ++start;
    while (end > start && space(text[end - 1]))
        --end;
    return std::string(text.substr(start, end - start));
}

// Split at every separator, keeping empty parts.
static Lines split(std::string_view text, char separator)
{
    Lines parts;
    size_t start = 0;
    while (true) {
        const size_t at = text.find(separator, start);
        parts.emplace_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos)
            return parts;
        start = at + 1;
    }
}

static Lines splitSkippingEmpty(std::string_view text, char separator)
{
    Lines parts = split(text, separator);
    parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
    return parts;
}

static std::string join(const Lines& parts, const std::string& separator)
{
    std::string text;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            text += separator;
        text += parts[i];
    }
    return text;
}

static bool isGroupHeader(const std::string& trimmedLine)
{
    return trimmedLine.size() >= 2 && trimmedLine.front() == '[' && trimmedLine.back() == ']';
}

static Lines readEntries(const Lines& lines, const std::string& group, const std::string& key)
{
    std::string current;
    for (const std::string& line : lines) {
        const std::string text = trimmed(line);
        if (isGroupHeader(text)) {
            current = text.substr(1, text.size() - 2);
        } else if (current == group && !text.starts_with('#')) {
            const size_t equals = line.find('=');
            if (equals != std::string::npos && trimmed(std::string_view(line).substr(0, equals)) == key)
                return splitSkippingEmpty(trimmed(std::string_view(line).substr(equals + 1)), ';');
        }
    }
    return {};
}

static void writeEntry(Lines& lines, const std::string& group, const std::string& key, const Lines& values)
{
    const std::string entry = key + "=" + join(values, ";") + ";";
    std::string current;
    long insertion = -1;
    for (long i = 0; i < long(lines.size()); ++i) {
        const std::string text = trimmed(lines[size_t(i)]);
        if (isGroupHeader(text)) {
            if (current == group)
                break;
            current = text.substr(1, text.size() - 2);
            if (current == group)
                insertion = i + 1;
        } else if (current == group) {
            const std::string& line = lines[size_t(i)];
            const size_t equals = line.find('=');
            if (equals != std::string::npos && trimmed(std::string_view(line).substr(0, equals)) == key) {
                lines.erase(lines.begin() + i);
                --i;
            } else {
                insertion = i + 1;
            }
        }
    }
    if (insertion < 0) {
        lines.push_back("[" + group + "]");
        insertion = long(lines.size());
    }
    lines.insert(lines.begin() + insertion, entry);
}

// An entry may carry a "+" (include) or "-" (exclude) prefix and an ":action" suffix.
static std::string terminalId(const std::string& line)
{
    std::string entry = trimmed(line);
    if (entry.starts_with('+') || entry.starts_with('-'))
        entry.erase(0, 1);
    return entry.substr(0, entry.find(':'));
}

// Hidden entries only mask a system entry; programs that need a terminal are
// not offered for opening files.
static bool opensFiles(const DesktopEntry& entry)
{
    return !entry.hidden && !entry.terminal;
}

// A bundle that is no longer installed opens nothing, even where a list
// still names it.
static std::shared_ptr<const DesktopEntry> entryForDesktopId(std::string desktopId)
{
    if (desktopId.ends_with(".desktop"))
        desktopId.resize(desktopId.size() - 8);
    std::shared_ptr<const DesktopEntry> entry = ApplicationRegistry::instance().installedById(desktopId);
    return entry && opensFiles(*entry) ? entry : nullptr;
}

// The file split at every '\n', empty parts kept; nothing for a missing file.
static Lines fileLines(const std::string& path)
{
    const std::optional<std::string> contents = file::read(path);
    if (!contents || contents->empty())
        return {};
    return split(*contents, '\n');
}

static bool writeLines(const std::string& path, const Lines& lines)
{
    return file::writeAtomically(path, join(lines, "\n") + "\n");
}

static bool makeParentDirectory(const std::string& path)
{
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    return !error;
}

std::string listedId(const DesktopEntry& entry)
{
    return entry.bundle ? entry.path : entry.id + ".desktop";
}

std::string userListPath()
{
    return paths::configHome() + "/mimeapps.list";
}

std::string userTerminalsListPath()
{
    return paths::configHome() + "/xdg-terminals.list";
}

std::vector<std::string> listPaths()
{
    std::vector<std::string> directories = {paths::configHome()};
    for (const std::string& directory : paths::configDirectories())
        directories.push_back(directory);
    directories.push_back(paths::dataHome() + "/applications");
    for (const std::string& directory : paths::dataDirectories())
        directories.push_back(directory + "/applications");

    std::vector<std::string> desktops = DesktopEntry::currentDesktops();
    std::vector<std::string> result;
    for (const std::string& directory : directories) {
        for (std::string desktop : desktops) {
            std::transform(desktop.begin(), desktop.end(), desktop.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            const std::string path = directory + "/" + desktop + "-mimeapps.list";
            if (!contains(result, path))
                result.push_back(path);
        }
        const std::string path = directory + "/mimeapps.list";
        if (!contains(result, path))
            result.push_back(path);
    }
    return result;
}

static ListedAssociations listedAssociations(const std::vector<std::string>& paths, const std::string& mimeType)
{
    ListedAssociations listed;
    for (const std::string& path : paths) {
        const Lines lines = fileLines(path);
        if (lines.empty())
            continue;
        for (const std::string& app : readEntries(lines, "Added Associations", mimeType)) {
            if (!contains(listed.removed, app) && !contains(listed.added, app))
                listed.added.push_back(app);
        }
        for (std::string& app : readEntries(lines, "Removed Associations", mimeType))
            listed.removed.push_back(std::move(app));
    }
    return listed;
}

std::vector<std::string> associations(const std::string& path, const std::string& group, const std::string& mimeType)
{
    const std::optional<std::string> contents = file::read(path);
    if (!contents)
        return {};
    return readEntries(split(*contents, '\n'), group, mimeType);
}

bool setDefault(const std::string& path, const std::string& mimeType, const std::string& desktopId)
{
    const auto hasAny = [](const std::string& text, const char* characters) {
        return text.find_first_of(characters) != std::string::npos;
    };
    if (mimeType.empty() || hasAny(mimeType, "\n\r=") || desktopId.empty() || hasAny(desktopId, "\n\r;"))
        return false;
    if (!makeParentDirectory(path))
        return false;
    LockFile lock(path + ".lock");
    if (!lock.tryLock())
        return false;
    Lines lines;
    if (access(path.c_str(), F_OK) == 0) {
        const std::optional<std::string> contents = file::read(path);
        if (!contents)
            return false;
        lines = split(*contents, '\n');
    }
    Lines added = readEntries(lines, "Added Associations", mimeType);
    added.erase(std::remove(added.begin(), added.end(), desktopId), added.end());
    added.insert(added.begin(), desktopId);
    writeEntry(lines, "Default Applications", mimeType, {desktopId});
    writeEntry(lines, "Added Associations", mimeType, added);
    Lines removed = readEntries(lines, "Removed Associations", mimeType);
    const size_t before = removed.size();
    removed.erase(std::remove(removed.begin(), removed.end(), desktopId), removed.end());
    if (removed.size() != before)
        writeEntry(lines, "Removed Associations", mimeType, removed);
    return writeLines(path, lines);
}

// When the file came to be; files without a birth time sort first.
static int64_t birthTime(const std::string& path)
{
    struct statx info;
    if (statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &info) != 0 || !(info.stx_mask & STATX_BTIME))
        return INT64_MIN;
    return int64_t(info.stx_btime.tv_sec) * 1000000000 + info.stx_btime.tv_nsec;
}

// URI scheme types are not in the MIME database; the lists name them all the same.
static std::string listedType(const std::string& mimeType)
{
    static constexpr std::string_view kSchemeHandler = "x-scheme-handler/";
    if (!mimeType.starts_with(kSchemeHandler))
        return mime::canonicalName(mimeType);
    std::string type = mimeType;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return type;
}

std::vector<std::string> applications(const std::string& mimeType, const std::vector<std::string>& paths)
{
    const std::vector<std::shared_ptr<const DesktopEntry>>& entries = ApplicationRegistry::instance().entries();
    std::vector<std::string> types;
    std::vector<std::string> apps;
    const std::string type = listedType(mimeType);
    if (!type.empty())
        types.push_back(type);

    while (apps.empty() && !types.empty()) {
        std::vector<std::string> names;
        for (const std::string& name : types) {
            names.push_back(name);
            for (const std::string& alias : mime::aliases(name))
                names.push_back(alias);
        }

        std::vector<std::string> removed;
        std::vector<std::shared_ptr<const DesktopEntry>> added;
        for (const std::string& name : names) {
            ListedAssociations listed = listedAssociations(paths, name);
            removed.insert(removed.end(), listed.removed.begin(), listed.removed.end());
            for (const std::string& app : listed.added) {
                std::shared_ptr<const DesktopEntry> entry = entryForDesktopId(app);
                if (entry && std::find(added.begin(), added.end(), entry) == added.end()) {
                    added.push_back(entry);
                    apps.push_back(entry->path);
                }
            }
        }

        std::vector<std::shared_ptr<const DesktopEntry>> matches;
        for (const std::shared_ptr<const DesktopEntry>& entry : entries) {
            const std::string listed = entry->bundle ? entry->path
                                                     : std::filesystem::path(entry->path).filename().string();
            if (!opensFiles(*entry) || contains(removed, listed))
                continue;
            // A bundle opens the descendants of its types too, so it matches
            // the first generation or none.
            if (entry->bundle ? bundle::opensType(*entry, type)
                              : std::any_of(entry->mimeTypes.begin(), entry->mimeTypes.end(),
                                            [&names](const std::string& name) { return contains(names, name); }))
                matches.push_back(entry);
        }

        // The first one opens files without a default, so keep that the earliest installed.
        std::stable_sort(matches.begin(), matches.end(),
                         [](const auto& a, const auto& b) { return birthTime(a->path) < birthTime(b->path); });

        for (const std::shared_ptr<const DesktopEntry>& entry : matches) {
            // One program installed under two desktop files is offered once.
            const bool duplicate = std::any_of(added.begin(), added.end(), [&entry](const auto& other) {
                return other->exec == entry->exec && other->name == entry->name;
            });
            if (duplicate)
                continue;
            added.push_back(entry);
            apps.push_back(entry->path);
        }

        std::vector<std::string> parents;
        for (const std::string& name : types) {
            for (const std::string& parent : mime::parents(name)) {
                if (mime::isKnown(parent))
                    parents.push_back(mime::canonicalName(parent));
            }
        }
        types = parents;
    }
    return apps;
}

std::string chosenApplication(const std::vector<std::string>& paths, const std::string& mimeType)
{
    const std::string type = listedType(mimeType);
    if (type.empty())
        return {};
    std::vector<std::string> removed;
    for (const std::string& path : paths) {
        const Lines lines = fileLines(path);
        if (lines.empty())
            continue;
        for (const std::string& app : readEntries(lines, "Default Applications", type)) {
            if (contains(removed, app))
                continue;
            if (std::shared_ptr<const DesktopEntry> entry = entryForDesktopId(app))
                return entry->path;
        }
        for (std::string& app : readEntries(lines, "Removed Associations", type))
            removed.push_back(std::move(app));
    }
    return {};
}

std::string defaultApplication(const std::vector<std::string>& paths, const std::string& mimeType)
{
    const std::string chosen = chosenApplication(paths, mimeType);
    if (!chosen.empty())
        return chosen;
    const std::vector<std::string> apps = applications(mimeType, paths);
    return apps.empty() ? std::string() : apps.front();
}

std::string defaultTerminal(const std::string& path)
{
    const std::optional<std::string> contents = file::read(path);
    if (!contents)
        return {};
    for (const std::string& line : split(*contents, '\n')) {
        const std::string entry = trimmed(line);
        if (!entry.empty() && !entry.starts_with('#') && !entry.starts_with('-'))
            return terminalId(entry);
    }
    return {};
}

bool setDefaultTerminal(const std::string& path, const std::string& desktopId)
{
    if (desktopId.empty() || desktopId.find_first_of("\n\r") != std::string::npos)
        return false;
    if (!makeParentDirectory(path))
        return false;
    LockFile lock(path + ".lock");
    if (!lock.tryLock())
        return false;
    Lines lines;
    if (access(path.c_str(), F_OK) == 0) {
        const std::optional<std::string> contents = file::read(path);
        if (!contents)
            return false;
        lines = splitSkippingEmpty(*contents, '\n');
    }
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [&desktopId](const std::string& line) {
                                   return !trimmed(line).starts_with('#') && terminalId(line) == desktopId;
                               }),
                lines.end());
    lines.insert(lines.begin(), desktopId);
    return writeLines(path, lines);
}

static constexpr char kRuntime[] = "org.cutefish.AppRuntime";
static constexpr char kRuntimePath[] = "/AppRuntime";

// Without the runtime: the entry's command, here.
static bool startCommand(const DesktopEntry& entry, const std::string& uri, const std::string& activationToken)
{
    const std::vector<std::string> command = entry.commandForArguments({uri});
    if (command.empty())
        return false;
    std::vector<std::string> environment;
    if (!activationToken.empty())
        environment.push_back("XDG_ACTIVATION_TOKEN=" + activationToken);
    return Process::startDetached(command.front(), {command.begin() + 1, command.end()}, entry.workingDirectory,
                                  environment)
            > 0;
}

// As the desktop starts applications: through the application runtime, which
// gives the program the applications' environment, a scope of its own and the
// activation token.
static void startHandler(const std::string& uri, const std::string& type, const std::string& activationToken,
                         const std::vector<std::string>& paths, const std::function<void(bool started)>& done)
{
    const std::shared_ptr<const DesktopEntry> entry =
            ApplicationRegistry::instance().byPath(defaultApplication(paths, type));
    dbus::Connection* bus = entry ? dbus::Connection::session() : nullptr;
    if (!bus) {
        const bool started = entry && startCommand(*entry, uri, activationToken);
        if (done)
            done(started);
        return;
    }
    dbus::Proxy runtime(bus, kRuntime, kRuntimePath, kRuntime);
    runtime.setTimeout(std::chrono::seconds(5));
    auto pending = std::make_shared<dbus::PendingCall>();
    *pending = runtime.callAsync(
            "launchApplication",
            [entry, uri, activationToken, done, pending](const dbus::Reply& reply) {
                const bool started = reply.isError() ? startCommand(*entry, uri, activationToken)
                                                     : reply.value<uint32_t>().value_or(0) != 0;
                if (done)
                    done(started);
                // The call's slot goes once its callback has returned.
                EventLoop::current()->post([pending] {});
            },
            entry->path, std::vector<std::string> {uri}, activationToken);
}

void openUri(const std::string& uri, const std::string& activationToken, std::function<void(bool started)> done,
             const std::vector<std::string>& paths)
{
    const size_t colon = uri.find(':');
    const std::string scheme = colon == std::string::npos ? std::string() : uri.substr(0, colon);
    const bool valid = !scheme.empty() && std::isalpha(static_cast<unsigned char>(scheme.front()))
            && std::all_of(scheme.begin(), scheme.end(), [](unsigned char c) {
                   return std::isalnum(c) || c == '+' || c == '-' || c == '.';
               });
    if (!valid) {
        if (done)
            done(false);
        return;
    }
    const std::string type = listedType("x-scheme-handler/" + scheme);
    ApplicationRegistry& registry = ApplicationRegistry::instance();
    if (!registry.isLoading()) {
        startHandler(uri, type, activationToken, paths, done);
        return;
    }
    // Once the installed applications are read.
    auto waiting = std::make_shared<Subscription>();
    *waiting = registry.changed.connect([uri, type, activationToken, paths, done, waiting] {
        if (ApplicationRegistry::instance().isLoading())
            return;
        startHandler(uri, type, activationToken, paths, done);
        waiting->reset();
    });
}

} // namespace cutefish::base::mimeapps
