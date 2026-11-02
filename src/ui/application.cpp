// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/application.h"

#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/locale.h"
#include "base/session_language.h"
#include "gpu/egl_context.h"
#include "ui/appearance.h"
#include "ui/platform_window.h"
#include "ui/text_context.h"
#include "ui/text_lines.h"
#include "ui/translation.h"
#include "ui/window.h"
#include "wayland/connection.h"

namespace cutefish::ui {

static Application* s_instance = nullptr;

Application::Application(int argc, char** argv, Platform platform)
    : m_arguments(argv, argv + argc)
    , m_platform(platform)
{
    s_instance = this;
    base::Locale::setProcessLocale();
    i18n::bindDomain(kTranslationDomain);
    std::error_code error;
    m_applicationId = std::filesystem::read_symlink("/proc/self/exe", error).filename();
    // The xdg-activation contract: the token is this process's alone, not
    // its children's. Taken before the font thread starts, as unsetenv() is
    // not thread-safe.
    const char* token = platform == Platform::Wayland ? std::getenv("XDG_ACTIVATION_TOKEN") : nullptr;
    if (token) {
        m_activationToken = token;
        unsetenv("XDG_ACTIVATION_TOKEN");
    }

    const char* ctype = std::setlocale(LC_CTYPE, nullptr);
    const std::string ctypeLocale = ctype ? ctype : "";
    // Fontconfig reads its configuration and caches while the compositor
    // connection, the appearance settings and the application's views are
    // set up.
    if (platform == Platform::Wayland)
        m_textLoader = std::thread([this, ctypeLocale] { m_text = std::make_unique<TextContext>(ctypeLocale); });
    else
        m_text = std::make_unique<TextContext>(ctypeLocale);

    m_loop = platform == Platform::Embedded ? base::EventLoop::current() : nullptr;
    if (!m_loop) {
        m_ownedLoop = std::make_unique<base::EventLoop>();
        m_loop = m_ownedLoop.get();
    }
    // The appearance settings are on their way and the driver loads while
    // the fonts are read.
    if (platform == Platform::Wayland) {
        m_sessionBus = base::dbus::Connection::session(m_loop);
        if (m_sessionBus)
            m_appearance = std::make_unique<AppearanceWatcher>(*m_sessionBus);
        m_connection = wayland::Connection::open(*m_loop);
        if (!m_connection)
            std::exit(1);
        m_gpu = gpu::EglContext::createInBackground(m_connection->display());
        m_connection->onOutputsChanged = [this] {
            screensChanged();
        };
        if (m_appearance)
            m_appearance->advance();
    }
    if (m_appearance)
        m_appearance->waitForSettings();

    if (m_sessionBus)
        followSessionLanguage(m_sessionBus);
}

Application::~Application()
{
    if (m_textLoader.joinable())
        m_textLoader.join();
    // Posted tasks may own windows, whose views still need the loop, the
    // connection and the GPU context while they are destroyed. A host's loop
    // keeps its tasks.
    if (m_ownedLoop)
        m_loop->dropPosted();
    m_appearance.reset();
    m_languageWatch.reset();
    m_sessionBus = nullptr;
    clearSubpixelCoverage();
    m_text.reset();
    m_gpu.reset();
    m_connection.reset();
    m_ownedLoop.reset();
    m_loop = nullptr;
    s_instance = nullptr;
}

Application* Application::instance()
{
    return s_instance;
}

TextContext& Application::text() const
{
    if (m_textLoader.joinable()) {
        m_textLoader.join();
        if (m_fontsChangedWhileLoading)
            m_text->clearFontCaches();
    }
    return *m_text;
}

void Application::fontsChanged()
{
    if (m_textLoader.joinable())
        m_fontsChangedWhileLoading = true;
    else
        m_text->clearFontCaches();
}

int Application::run()
{
    return m_loop->run();
}

void Application::setMainMenu(Menu* menu)
{
    m_mainMenu = menu;
    for (Window* window : m_windows) {
        if (!window->m_mainMenu && !window->m_transientParent)
            window->updateMainMenu();
    }
}

void Application::setApplicationId(std::string applicationId)
{
    m_applicationId = std::move(applicationId);
    for (Window* window : m_windows) {
        if (!window->m_parent)
            window->m_platform->setAppId(m_applicationId);
    }
}

void Application::followSessionLanguage(base::dbus::Connection* bus)
{
    m_languageWatch = std::make_unique<base::SessionLanguage>(bus);
    m_languageChanged = m_languageWatch->changed.connect([this] { languageEnvironmentChanged(); });
}

void Application::setLanguageEnvironment(const std::map<std::string, std::string>& variables)
{
    if (base::SessionLanguage::apply(variables))
        languageEnvironmentChanged();
}

void Application::languageEnvironmentChanged()
{
    const char* ctype = std::setlocale(LC_CTYPE, nullptr);
    text().setLocale(ctype ? ctype : "");
    languageChanged.emit();
    themeChanged();
}

void Application::themeChanged()
{
    clearSubpixelCoverage();
    for (Window* window : m_windows)
        window->handleThemeChanged();
}

void Application::scheduleActiveUpdate()
{
    if (m_activeUpdatePending)
        return;
    m_activeUpdatePending = true;
    const auto update = [this] {
        m_activeUpdatePending = false;
        for (Window* window : std::vector<Window*>(m_windows))
            window->updateActive();
    };
    // Headless windows have no compositor to wait for.
    if (m_connection)
        m_connection->sync(update);
    else
        update();
}

std::vector<Application::Screen> Application::screens() const
{
    std::vector<Screen> screens;
    if (!m_connection)
        return screens;
    for (const wayland::Connection::OutputInfo& output : m_connection->outputs()) {
        screens.push_back({output.name,
                           {output.position.x, output.position.y, output.logicalSize.width,
                            output.logicalSize.height},
                           output.workArea, output.scale, output.primary});
    }
    return screens;
}

bool Application::isConnectionLost() const
{
    return m_connection && m_connection->isLost();
}

void Application::quit(int exitCode)
{
    m_loop->quit(exitCode);
}

} // namespace cutefish::ui
