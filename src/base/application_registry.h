// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/desktop_entry.h"
#include "base/elapsed_timer.h"
#include "base/file_watcher.h"
#include "base/signal.h"
#include "base/thread_pool.h"
#include "base/timer.h"

namespace cutefish::base {

class EventLoop;

// The installed applications: every .desktop file in the user's and the
// system's applications directories, and the application bundles in
// ~/Applications and /Applications when an entry runs them, read on a
// thread and read again when they change or the session language does.
class ApplicationRegistry
{
public:
    // The loop's registry, made on first use and gone with the loop.
    static ApplicationRegistry& instance(EventLoop* loop = nullptr);
    // A registry of other applications directories, for tests.
    static std::unique_ptr<ApplicationRegistry> create(EventLoop& loop, std::vector<std::string> directories,
                                                       std::vector<std::string> bundleDirectories = {});
    // Makes a created registry the loop's instance(), for tests.
    static void setInstance(EventLoop& loop, std::unique_ptr<ApplicationRegistry> registry);

    ApplicationRegistry(const ApplicationRegistry&) = delete;
    ApplicationRegistry& operator=(const ApplicationRegistry&) = delete;

    // Until the first scan has arrived, and while another runs.
    bool isLoading() const { return m_loading; }

    // An exact id first, then one that differs only in case.
    std::shared_ptr<const DesktopEntry> byId(const std::string& id) const;
    // As byId, but never a bundle outside the Applications folders.
    std::shared_ptr<const DesktopEntry> installedById(const std::string& id) const;
    // A bundle outside the Applications folders is read on a thread: null
    // until then, and checked again when asked for later.
    std::shared_ptr<const DesktopEntry> byPath(const std::string& path) const;
    // The bundle at path, read now when it is not installed; null when it is
    // no bundle or no entry runs bundles.
    std::shared_ptr<const DesktopEntry> readBundle(const std::string& path) const;
    // The entry a window or process belongs to, from its app id or window
    // class and its executable's path and name; null when none fits, or when
    // nothing as certain as the app id or class picks one of several entries
    // that fit equally well. A bundle's program belongs to that bundle alone.
    std::shared_ptr<const DesktopEntry> resolveWindow(const std::string& appId, const std::string& windowClass,
                                                      const std::string& executablePath,
                                                      const std::string& executableName) const;
    // Whether an entry runs application bundles.
    bool runsBundles() const { return bool(m_bundleHandler); }
    // Every entry, the ones menus leave out included.
    const std::vector<std::shared_ptr<const DesktopEntry>>& entries() const { return m_entries; }
    // What menus show, sorted by name.
    const std::vector<std::shared_ptr<const DesktopEntry>>& applications() const { return m_applications; }

    // After each scan, and when a bundle outside the Applications folders was read.
    Signal<> changed;

    // ~/.local/share/applications, then the system's.
    static std::vector<std::string> desktopPaths();

    void scheduleScan();

private:
    ApplicationRegistry(EventLoop& loop, std::vector<std::string> directories, std::vector<std::string> bundleDirectories);
    friend struct std::default_delete<ApplicationRegistry>;
    ~ApplicationRegistry() = default;

    void startScan();
    void applyScan(std::vector<DesktopEntry> results);
    void monitorPaths();
    std::shared_ptr<const DesktopEntry> otherBundle(const std::string& path) const;
    void otherBundleRead(const std::string& path, std::optional<DesktopEntry> entry);

    // A bundle outside the Applications folders, as last read.
    struct OtherBundle
    {
        std::shared_ptr<const DesktopEntry> entry;
        ElapsedTimer read;
        Job job;
        // Sees the folder go, or the disk image it is on ejected.
        FileWatch watch;
    };

    EventLoop& m_loop;
    std::vector<std::string> m_directories;
    std::vector<std::string> m_bundleDirectories;
    Timer m_debounce;
    // Brings back the watches that could not be made, such as when the
    // user's inotify watches ran out.
    Timer m_watchRetry;
    std::chrono::milliseconds m_watchRetryDelay;
    Job m_scan;
    std::vector<FileWatch> m_watches;
    dbus::Match m_languageChanged;
    std::string m_language;
    std::map<std::string, std::shared_ptr<const DesktopEntry>> m_byId;
    std::map<std::string, std::shared_ptr<const DesktopEntry>> m_byPath;
    std::vector<std::shared_ptr<const DesktopEntry>> m_entries;
    std::vector<std::shared_ptr<const DesktopEntry>> m_applications;
    std::shared_ptr<const DesktopEntry> m_bundleHandler;
    mutable std::map<std::string, OtherBundle> m_otherBundles;
    bool m_loading = false;
    bool m_scanQueued = false;
};

} // namespace cutefish::base
