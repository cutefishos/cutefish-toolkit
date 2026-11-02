// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>

namespace cutefish::ui {

class Window;

// FishUI's AboutDialog: a fixed 300 wide window over its parent with the
// application's icon, name, description and a link that opens in the
// browser, under a header with only the close button. Escape closes it.
class AboutDialog
{
public:
    explicit AboutDialog(Window* parent);
    ~AboutDialog();

    AboutDialog(const AboutDialog&) = delete;
    AboutDialog& operator=(const AboutDialog&) = delete;

    // Before show().
    void setName(std::string name) { m_name = std::move(name); }
    void setDescription(std::string description) { m_description = std::move(description); }
    // A theme icon, looked up as image://icontheme does, or a file.
    void setIconSource(std::string icon) { m_icon = std::move(icon); }
    // https://cutefish.org unless set.
    void setLink(std::string link) { m_link = std::move(link); }

    // Raises the dialog when it is shown already.
    void show();
    // Goes once the event being handled is over.
    void close();
    bool isShown() const { return m_window != nullptr; }
    Window* window() const { return m_window.get(); }

    // A link click opens the link with this; unset, xdg-open runs.
    std::function<void(const std::string& link)> onLinkActivated;
    std::function<void()> onClosed;

private:
    class Content;

    Window* m_parent;
    std::string m_name;
    std::string m_description;
    std::string m_icon;
    std::string m_link = "https://cutefish.org";
    std::unique_ptr<Window> m_window;
};

} // namespace cutefish::ui
