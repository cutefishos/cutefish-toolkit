// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/signal.h"
#include "ui/geometry.h"

namespace cutefish::base {
class EventLoop;
class SessionLanguage;
}

namespace cutefish::base::dbus {
class Connection;
}

namespace cutefish::ui {

class AppearanceWatcher;
class Menu;
class TextContext;
class Window;

namespace gpu {
class EglContext;
}

namespace wayland {
class Connection;
}

// The process-wide state windows share: the event loop, the compositor
// connection, the GPU context and the fonts. Exits when the compositor
// cannot be reached.
class Application
{
public:
    enum class Platform {
        Wayland,
        // No compositor and no GPU: windows render only when grabbed, for tests.
        Headless,
        // Inside a process that owns the screen, such as the compositor:
        // EmbeddedWindows render into memory when the host asks, input comes
        // from the host, and nothing connects to Wayland, a GPU or D-Bus.
        // The application uses the calling thread's current loop when there
        // is one, and leaves it to the host; otherwise the host drives
        // loop() through its descriptor.
        Embedded,
    };

    Application(int argc, char** argv, Platform platform = Platform::Wayland);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    static Application* instance();

    const std::vector<std::string>& arguments() const { return m_arguments; }
    Platform platform() const { return m_platform; }

    // Names the application's desktop entry; every toplevel window carries it.
    // Defaults to the executable's name.
    void setApplicationId(std::string applicationId);
    const std::string& applicationId() const { return m_applicationId; }
    // The xdg-activation token the launcher gave this process, once: for a
    // single-instance application to hand to its running instance, whose
    // Window::activate() takes it. Empty when there was none.
    std::string takeActivationToken() { return std::exchange(m_activationToken, {}); }
    // For a process that took the token out of its environment before the
    // application existed, as one whose environment other code holds must.
    void setActivationToken(std::string token) { m_activationToken = std::move(token); }

    base::EventLoop& loop() const { return *m_loop; }
    bool hasConnection() const { return m_connection != nullptr; }
    // The compositor closed the connection, as when it exits or restarts;
    // the loop has been asked to quit with 1.
    bool isConnectionLost() const;
    // Wayland only.
    wayland::Connection& connection() const { return *m_connection; }
    gpu::EglContext& gpu() const { return *m_gpu; }
    // Waits for the fonts the first time, which load on a thread of their
    // own while the application starts.
    TextContext& text() const;

    // The loop's shared session bus connection, when there is one.
    base::dbus::Connection* sessionBus() const { return m_sessionBus; }
    const std::vector<Window*>& windows() const { return m_windows; }

    // The main menu the status bar shows while one of the application's
    // windows is active, unless the window has its own: its rows are the
    // bar's entries, and the submenu with MenuRole::Application opens from
    // the application's name. Not owned; it stays while it is the main menu.
    void setMainMenu(Menu* menu);
    Menu* mainMenu() const { return m_mainMenu; }

    struct Screen
    {
        // The wl_output name.
        std::string name;
        // In the compositor's global logical coordinates, the ones
        // Window::setScreenPosition takes.
        RectF geometry;
        // What windows are laid out in: the screen less the desktop's
        // panels and dock; the whole screen until the compositor tells.
        RectF workArea;
        // Device pixels per logical pixel.
        double scale = 1;
        // The one with the status bar.
        bool primary = false;
    };
    // The outputs, known from the start, before any window; none off
    // Wayland.
    std::vector<Screen> screens() const;
    // A screen came, went or changed.
    base::Signal<> screensChanged;

    int run();
    void quit(int exitCode = 0);

    // Text, dates and numbers follow the session language as it changes:
    // the process's locale is set again, then this runs, for the views to
    // take their text again.
    base::Signal<> languageChanged;
    // Applications on Wayland follow cutefish-services on their own; a host
    // whose application has no session bus, as the compositor's, lends its.
    void followSessionLanguage(base::dbus::Connection* bus);
    // LANG, LANGUAGE and the LC_ variables, an empty value removing one.
    void setLanguageEnvironment(const std::map<std::string, std::string>& variables);

private:
    friend class EmbeddedWindow;
    friend struct Theme;
    friend class Window;

    void languageEnvironmentChanged();
    void themeChanged();
    // The theme's font family changed: cached font lookups go.
    void fontsChanged();
    // Notifies the windows whose isActive() changed, after the events at hand.
    void scheduleActiveUpdate();

    std::vector<std::string> m_arguments;
    std::string m_applicationId;
    std::string m_activationToken;
    Platform m_platform;
    std::unique_ptr<base::EventLoop> m_ownedLoop;
    base::EventLoop* m_loop = nullptr;
    std::unique_ptr<wayland::Connection> m_connection;
    std::unique_ptr<gpu::EglContext> m_gpu;
    mutable std::unique_ptr<TextContext> m_text;
    mutable std::thread m_textLoader;
    mutable bool m_fontsChangedWhileLoading = false;
    base::dbus::Connection* m_sessionBus = nullptr;
    std::unique_ptr<AppearanceWatcher> m_appearance;
    std::unique_ptr<base::SessionLanguage> m_languageWatch;
    base::Subscription m_languageChanged;
    std::vector<Window*> m_windows;
    Menu* m_mainMenu = nullptr;
    bool m_activeUpdatePending = false;
};

} // namespace cutefish::ui
