// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/application_registry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <unicode/coll.h>
#include <unicode/unistr.h>

#include "base/application_bundle.h"
#include "base/event_loop.h"
#include "base/file.h"
#include "base/paths.h"

namespace cutefish::base {

namespace fs = std::filesystem;

static constexpr std::chrono::milliseconds kWatchRetry(2000);
static constexpr std::chrono::milliseconds kWatchRetryLimit(60000);
// How long a bundle outside the Applications folders, such as on a disk
// image, is taken as read before it is checked again.
static constexpr std::chrono::seconds kOtherBundleRecheck(2);

static std::string normalized(std::string_view value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    std::string result(value.substr(start, end - start + 1));
    icu::UnicodeString unicode = icu::UnicodeString::fromUTF8(result);
    unicode.toLower(icu::Locale::getRoot());
    result.clear();
    unicode.toUTF8String(result);
    return result;
}

// A bare prefix lets a short or unrelated id claim an entry ("code" and
// "Code::Blocks"), so the prefix has to end where a word does.
static bool startsWithWord(const std::string& value, const std::string& prefix)
{
    if (prefix.empty() || value.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (value.size() == prefix.size())
        return true;
    const char next = value[prefix.size()];
    return next == '-' || next == '.' || next == '_' || next == ' ';
}

static std::string fileName(const std::string& path)
{
    return fs::path(path).filename().string();
}

// The file name up to its last dot.
static std::string completeBaseName(const std::string& path)
{
    const std::string name = fileName(path);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

static bool isScript(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    const std::string suffix = name.substr(dot + 1);
    for (const char* script : {"py", "pl", "rb", "sh", "js", "lua", "tcl"}) {
        if (suffix == script)
            return true;
    }
    return false;
}

static bool opensBundles(const DesktopEntry& entry)
{
    return !entry.hidden && !entry.bundle && !entry.exec.empty()
            && std::find(entry.mimeTypes.begin(), entry.mimeTypes.end(), bundle::kType) != entry.mimeTypes.end();
}

static std::vector<DesktopEntry> scanBundles(const std::string& directory, const DesktopEntry& handler)
{
    std::vector<std::string> paths;
    std::error_code error;
    for (auto it = fs::directory_iterator(directory, fs::directory_options::skip_permission_denied, error);
         it != fs::directory_iterator(); it.increment(error)) {
        if (error)
            break;
        if (!bundle::bundlePath(it->path().string()).empty() && it->is_directory(error))
            paths.push_back(it->path().string());
    }
    std::sort(paths.begin(), paths.end());

    std::vector<DesktopEntry> results;
    for (const std::string& path : paths) {
        if (std::optional<DesktopEntry> entry = bundle::read(path, handler))
            results.push_back(std::move(*entry));
    }
    return results;
}

static std::vector<DesktopEntry> scan(const std::vector<std::string>& directories,
                                      const std::vector<std::string>& bundleDirectories, const std::string& language)
{
    std::vector<DesktopEntry> results;
    std::set<std::string> selectedIds;
    for (const std::string& rootPath : directories) {
        std::error_code error;
        if (!fs::is_directory(rootPath, error))
            continue;

        std::vector<std::string> files;
        for (auto it = fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (error)
                break;
            if (it->path().extension() == ".desktop" && it->is_regular_file(error))
                files.push_back(it->path().string());
        }
        std::sort(files.begin(), files.end());

        for (const std::string& path : files) {
            std::string id = fs::path(path).lexically_relative(rootPath).string();
            std::replace(id.begin(), id.end(), '/', '-');
            id.resize(id.size() - 8);
            if (selectedIds.contains(id))
                continue;
            const std::optional<std::string> contents = file::read(path);
            if (!contents)
                continue;
            if (std::optional<DesktopEntry> entry = DesktopEntry::parse(id, path, *contents, language)) {
                selectedIds.insert(id);
                results.push_back(std::move(*entry));
            }
        }
    }

    const auto handler = std::find_if(results.begin(), results.end(), opensBundles);
    if (handler != results.end()) {
        const DesktopEntry bundleHandler = *handler;
        for (const std::string& directory : bundleDirectories) {
            std::vector<DesktopEntry> bundles = scanBundles(directory, bundleHandler);
            std::move(bundles.begin(), bundles.end(), std::back_inserter(results));
        }
    }
    return results;
}

// One per loop, on the loop's thread.
static std::map<EventLoop*, std::unique_ptr<ApplicationRegistry>>& instances()
{
    static thread_local std::map<EventLoop*, std::unique_ptr<ApplicationRegistry>> s_instances;
    return s_instances;
}

ApplicationRegistry& ApplicationRegistry::instance(EventLoop* loop)
{
    if (!loop)
        loop = EventLoop::current();
    auto& registry = instances()[loop];
    if (!registry) {
        registry.reset(new ApplicationRegistry(*loop, desktopPaths(), bundle::applicationsDirectories()));
        loop->addDestroyHook([loop] { instances().erase(loop); });
    }
    return *registry;
}

void ApplicationRegistry::setInstance(EventLoop& loop, std::unique_ptr<ApplicationRegistry> registry)
{
    auto& kept = instances()[&loop];
    if (!kept)
        loop.addDestroyHook([loop = &loop] { instances().erase(loop); });
    kept = std::move(registry);
}

std::unique_ptr<ApplicationRegistry> ApplicationRegistry::create(EventLoop& loop, std::vector<std::string> directories,
                                                                 std::vector<std::string> bundleDirectories)
{
    return std::unique_ptr<ApplicationRegistry>(
            new ApplicationRegistry(loop, std::move(directories), std::move(bundleDirectories)));
}

ApplicationRegistry::ApplicationRegistry(EventLoop& loop, std::vector<std::string> directories,
                                         std::vector<std::string> bundleDirectories)
    : m_loop(loop)
    , m_directories(std::move(directories))
    , m_bundleDirectories(std::move(bundleDirectories))
    , m_debounce(&loop)
    , m_watchRetry(&loop)
    , m_watchRetryDelay(kWatchRetry)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(std::chrono::milliseconds(100));
    m_debounce.onTimeout = [this] { startScan(); };

    m_watchRetry.setSingleShot(true);
    m_watchRetry.onTimeout = [this] {
        m_watchRetryDelay = std::min(m_watchRetryDelay * 2, kWatchRetryLimit);
        const size_t watched = m_watches.size();
        monitorPaths();
        // What changed in a directory while it went unwatched is only seen
        // by reading it again.
        if (m_watches.size() > watched)
            scheduleScan();
    };

    if (dbus::Connection* bus = dbus::Connection::session(&loop)) {
        dbus::MatchRule rule;
        rule.sender = "org.cutefish.Services";
        rule.path = "/org/cutefish/Services/Language";
        rule.interface = "org.cutefish.Services.Language";
        rule.member = "languageChanged";
        m_languageChanged = bus->subscribe(rule, [this](const dbus::Message&) {
            if (DesktopEntry::displayLanguage() != m_language)
                scheduleScan();
        });
    }

    monitorPaths();
    startScan();
}

std::vector<std::string> ApplicationRegistry::desktopPaths()
{
    std::vector<std::string> result{fs::path(paths::dataHome() + "/applications").lexically_normal().string()};
    for (const std::string& directory : paths::dataDirectories()) {
        const std::string path = fs::path(directory + "/applications").lexically_normal().string();
        if (std::find(result.begin(), result.end(), path) == result.end())
            result.push_back(path);
    }
    return result;
}

void ApplicationRegistry::scheduleScan()
{
    m_debounce.start();
}

void ApplicationRegistry::startScan()
{
    if (m_loading && m_scan.isPending()) {
        m_scanQueued = true;
        return;
    }
    m_loading = true;
    m_language = DesktopEntry::displayLanguage();
    m_scan = runInThread(
            [directories = m_directories, bundleDirectories = m_bundleDirectories, language = m_language] {
                return scan(directories, bundleDirectories, language);
            },
            [this](std::vector<DesktopEntry> results) { applyScan(std::move(results)); });
}

void ApplicationRegistry::applyScan(std::vector<DesktopEntry> results)
{
    m_loading = false;
    m_byId.clear();
    m_byPath.clear();
    m_entries.clear();
    m_applications.clear();
    m_bundleHandler = nullptr;

    const std::vector<std::string> desktops = DesktopEntry::currentDesktops();
    for (DesktopEntry& data : results) {
        auto entry = std::make_shared<const DesktopEntry>(std::move(data));
        m_byId[entry->id] = entry;
        std::error_code error;
        m_byPath[fs::absolute(entry->path, error).lexically_normal().string()] = entry;
        m_entries.push_back(entry);
        if (entry->shouldShow(desktops))
            m_applications.push_back(entry);
    }
    for (const auto& entry : m_entries) {
        if (opensBundles(*entry)) {
            m_bundleHandler = entry;
            break;
        }
    }
    // Their command is the handler's, which may have changed.
    for (auto& [path, other] : m_otherBundles)
        other.read.invalidate();

    UErrorCode status = U_ZERO_ERROR;
    std::string localeName = m_language;
    std::unique_ptr<icu::Collator> collator(icu::Collator::createInstance(icu::Locale(localeName.c_str()), status));
    std::sort(m_applications.begin(), m_applications.end(),
              [&collator](const std::shared_ptr<const DesktopEntry>& a, const std::shared_ptr<const DesktopEntry>& b) {
                  int order = 0;
                  if (collator) {
                      UErrorCode compareStatus = U_ZERO_ERROR;
                      order = collator->compareUTF8(a->name, b->name, compareStatus);
                  } else {
                      order = a->name.compare(b->name);
                  }
                  return order == 0 ? a->id < b->id : order < 0;
              });

    monitorPaths();
    changed.emit();
    if (m_scanQueued) {
        m_scanQueued = false;
        startScan();
    }
}

void ApplicationRegistry::monitorPaths()
{
    m_watches.clear();
    bool complete = true;
    const auto watch = [this, &complete](const std::string& path) {
        FileWatch watch = FileWatch::directory(path, [this](const std::string&) { scheduleScan(); }, &m_loop);
        if (watch.isActive())
            m_watches.push_back(std::move(watch));
        else
            complete = false;
    };

    for (const std::string& rootPath : m_directories) {
        std::error_code error;
        if (!fs::is_directory(rootPath, error)) {
            // The closest directory that exists sees the applications
            // directory appear.
            fs::path parent(rootPath);
            while (parent.has_parent_path() && parent != parent.parent_path() && !fs::is_directory(parent, error))
                parent = parent.parent_path();
            if (fs::is_directory(parent, error))
                watch(parent.string());
            continue;
        }
        watch(rootPath);
        for (auto it = fs::recursive_directory_iterator(rootPath, fs::directory_options::skip_permission_denied, error);
             it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (error)
                break;
            if (it->is_directory(error))
                watch(it->path().string());
        }
    }

    // Copies arrive in them whole, renamed into place, so their own entries
    // are all there is to follow.
    for (const std::string& bundleDirectory : m_bundleDirectories) {
        std::error_code error;
        if (fs::is_directory(bundleDirectory, error)) {
            watch(bundleDirectory);
        } else {
            const fs::path directory(bundleDirectory);
            const std::string name = directory.filename().string();
            FileWatch parent = FileWatch::directory(
                    directory.parent_path().string(),
                    [this, name](const std::string& changed) {
                        if (changed == name)
                            scheduleScan();
                    },
                    &m_loop);
            if (parent.isActive())
                m_watches.push_back(std::move(parent));
            else
                complete = false;
        }
    }

    if (complete) {
        m_watchRetry.stop();
        m_watchRetryDelay = kWatchRetry;
    } else if (!m_watchRetry.isActive()) {
        m_watchRetry.start(m_watchRetryDelay);
    }
}

std::shared_ptr<const DesktopEntry> ApplicationRegistry::byId(const std::string& id) const
{
    if (std::shared_ptr<const DesktopEntry> entry = installedById(id))
        return entry;
    // A bundle's id is its path.
    return id.starts_with('/') ? otherBundle(bundle::bundlePath(id)) : nullptr;
}

std::shared_ptr<const DesktopEntry> ApplicationRegistry::installedById(const std::string& id) const
{
    const auto it = m_byId.find(id);
    if (it != m_byId.end())
        return it->second;
    if (id.starts_with('/')) {
        std::error_code error;
        const auto found = m_byPath.find(fs::absolute(id, error).lexically_normal().string());
        return found != m_byPath.end() ? found->second : nullptr;
    }
    const std::string lower = normalized(id);
    for (const auto& [key, entry] : m_byId) {
        if (normalized(key) == lower)
            return entry;
    }
    return nullptr;
}

std::shared_ptr<const DesktopEntry> ApplicationRegistry::byPath(const std::string& path) const
{
    if (path.empty())
        return nullptr;
    std::error_code error;
    const auto it = m_byPath.find(fs::absolute(path, error).lexically_normal().string());
    if (it != m_byPath.end())
        return it->second;
    return otherBundle(bundle::bundlePath(path));
}

std::shared_ptr<const DesktopEntry> ApplicationRegistry::readBundle(const std::string& path) const
{
    const std::string bundlePath = bundle::bundlePath(path);
    if (bundlePath.empty() || !m_bundleHandler)
        return nullptr;
    if (const auto it = m_byPath.find(bundlePath); it != m_byPath.end())
        return it->second;
    std::optional<DesktopEntry> entry = bundle::read(bundlePath, *m_bundleHandler);
    if (!entry)
        return nullptr;
    entry->noDisplay = true;
    return std::make_shared<const DesktopEntry>(std::move(*entry));
}

std::shared_ptr<const DesktopEntry> ApplicationRegistry::otherBundle(const std::string& path) const
{
    if (path.empty() || !m_bundleHandler)
        return nullptr;
    if (const auto it = m_byPath.find(path); it != m_byPath.end())
        return it->second;

    // A disk image may hang, so its bundles are never read on this thread.
    OtherBundle& other = m_otherBundles[path];
    if (!other.job.isPending() && (!other.read.isValid() || other.read.hasExpired(kOtherBundleRecheck))) {
        other.read.start();
        auto* self = const_cast<ApplicationRegistry*>(this);
        other.job = runInThread([path, handler = *m_bundleHandler] { return bundle::read(path, handler); },
                                [self, path](std::optional<DesktopEntry> entry) {
                                    self->otherBundleRead(path, std::move(entry));
                                });
    }
    return other.entry;
}

void ApplicationRegistry::otherBundleRead(const std::string& path, std::optional<DesktopEntry> entry)
{
    OtherBundle& other = m_otherBundles[path];
    // Kept out of menus: only the Applications folders hold installed
    // applications.
    if (entry)
        entry->noDisplay = true;
    const DesktopEntry* old = other.entry.get();
    const bool same = (!old && !entry)
            || (old && entry && old->name == entry->name && old->icon == entry->icon && old->exec == entry->exec);
    if (entry && !other.watch.isActive()) {
        // Made here once a read on a thread got through, so the folder answers.
        other.watch = FileWatch::directory(
                path,
                [this, path](const std::string& name) {
                    if (!name.empty() && name != "Contents")
                        return;
                    m_otherBundles[path].read.invalidate();
                    otherBundle(path);
                },
                &m_loop);
    } else if (!entry) {
        other.watch.reset();
    }
    if (same)
        return;
    other.entry = entry ? std::make_shared<const DesktopEntry>(std::move(*entry)) : nullptr;
    changed.emit();
}

std::shared_ptr<const DesktopEntry> ApplicationRegistry::resolveWindow(const std::string& appId,
                                                                       const std::string& windowClass,
                                                                       const std::string& executablePath,
                                                                       const std::string& executableName) const
{
    if (m_bundleHandler) {
        if (const std::string bundlePath = bundle::bundleOfProgram(executablePath); !bundlePath.empty())
            return byPath(bundlePath);
    }

    const std::string wantedAppId = normalized(appId);
    const std::string wantedClass = normalized(windowClass);
    const std::string wantedExecutable = normalized(executablePath);
    const std::string wantedName = normalized(executableName);
    std::vector<std::string> wantedPath;
    for (const auto& part : fs::path(wantedExecutable)) {
        if (!part.empty() && part != "/")
            wantedPath.push_back(part.string());
    }
    // A script's helpers share its data directory, so a script names its
    // application by its own name and never through that directory.
    const bool wantedScript = isScript(wantedName);
    const std::string wantedScriptName = wantedScript ? completeBaseName(wantedName) : std::string();
    const std::vector<std::string> desktops = DesktopEntry::currentDesktops();
    // On equal scores, an entry meant for this desktop beats one for another
    // desktop sharing its window class, and either beats a NoDisplay helper.
    const auto preference = [&desktops](const DesktopEntry& entry) {
        if (entry.shouldShow(desktops))
            return 2;
        return entry.noDisplay ? 0 : 1;
    };

    std::shared_ptr<const DesktopEntry> best;
    int bestScore = 0;
    int bestPreference = 0;
    // Menu visibility does not determine whether an entry owns windows.
    for (const auto& entry : m_entries) {
        if (entry->hidden || entry->bundle)
            continue;

        const std::vector<std::string> command = entry->command();
        const std::string firstArgument = command.empty() ? std::string() : command.front();
        const std::string id = normalized(entry->id);
        const std::string startupClass = normalized(entry->startupWMClass);
        const std::string program = normalized(firstArgument);
        const std::string programName = normalized(fileName(firstArgument));
        const std::string icon = normalized(entry->icon);
        const std::string name = normalized(entry->name);
        const std::string entryFileName = normalized(completeBaseName(entry->path));
        // A terminal entry's window belongs to the terminal emulator, so a
        // process match there is a GUI program or script sharing its name.
        const bool matchProcess = !entry->terminal;
        const std::string executable = matchProcess ? wantedExecutable : std::string();
        const std::string processName = matchProcess ? wantedName : std::string();
        const std::string scriptName = matchProcess ? wantedScriptName : std::string();

        int score = 0;
        const auto consider = [&score](bool matches, int value) {
            if (matches)
                score = std::max(score, value);
        };
        consider(!wantedAppId.empty() && id == wantedAppId, 100);
        consider(!wantedClass.empty() && id == wantedClass, 90);
        consider(!wantedClass.empty() && startupClass == wantedClass, 95);
        consider(!wantedAppId.empty() && startupClass == wantedAppId, 95);
        consider(!executable.empty() && program == executable, 85);
        consider(!processName.empty() && programName == processName, 80);
        consider(!scriptName.empty() && programName == scriptName, 80);
        consider(!executable.empty() && entryFileName == executable, 75);
        consider(!processName.empty() && entryFileName == processName, 75);
        consider(!executable.empty() && icon == executable, 75);
        consider(!processName.empty() && icon == processName, 75);
        consider(startsWithWord(startupClass, wantedAppId), 70);
        consider(startsWithWord(startupClass, wantedClass), 70);
        consider(startsWithWord(id, wantedClass), 60);
        consider(startsWithWord(entryFileName, wantedClass), 60);
        consider(startsWithWord(program, wantedClass), 55);
        consider(startsWithWord(icon, wantedClass), 55);
        consider(startsWithWord(name, wantedClass), 50);
        // A launcher script for a binary kept in a directory of its own name,
        // such as /usr/lib/thunderbird/thunderbird-bin.
        consider(matchProcess && !wantedScript && !programName.empty()
                     && std::find(wantedPath.begin(), wantedPath.end(), programName) != wantedPath.end(),
                 45);

        if (score == 0 || score < bestScore)
            continue;
        const int entryPreference = preference(*entry);
        if (score > bestScore || entryPreference > bestPreference) {
            best = entry;
            bestScore = score;
            bestPreference = entryPreference;
        }
    }
    return best;
}

} // namespace cutefish::base
