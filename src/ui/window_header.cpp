// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/window_header.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/application.h"
#include "ui/header_button.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_controls.h"

namespace cutefish::ui {

static constexpr double kButton = window_controls::kButton;
// The controls sit in the 32-pixel slot the apps centre their margins on.
static constexpr double kSlot = 32;

namespace {

// Holds the controls, so the pointer over any of them or the gaps between
// them hovers the row as one.
class ControlsZone final : public View
{
public:
    ControlsZone()
    {
        setTracksHover(true);
    }

    std::function<void(bool)> onHoverChanged;

protected:
    void hoverChanged() override
    {
        if (onHoverChanged)
            onHoverChanged(isHovered());
    }
};

// Restore while the window is maximized, maximize otherwise.
class MaximizeButton final : public HeaderButton
{
public:
    MaximizeButton()
        : HeaderButton("maximize", kButton)
    {
    }

protected:
    std::string currentArtwork() const override
    {
        const Window* window = this->window();
        return window && window->states().maximized ? "restore" : "maximize";
    }
};

} // namespace

WindowHeader::WindowHeader(double height)
    : m_height(height)
    , m_zone(addChild<ControlsZone>())
    , m_close(m_zone->addChild<HeaderButton>("close", kButton))
    , m_minimize(m_zone->addChild<HeaderButton>("minimize", kButton))
    , m_maximize(m_zone->addChild<MaximizeButton>())
{
    for (HeaderButton* button : {m_close, m_minimize, m_maximize})
        button->setWindowControl(true);
    static_cast<ControlsZone*>(m_zone)->onHoverChanged = [this](bool hovered) {
        for (HeaderButton* control : {m_close, m_minimize, m_maximize})
            control->setGroupHovered(hovered);
    };
    m_minimize->onClicked = [this] {
        if (Window* window = this->window())
            window->minimize();
    };
    m_maximize->onClicked = [this] {
        if (Window* window = this->window())
            window->setMaximized(!window->states().maximized);
    };
    m_close->onClicked = [this] {
        Window* window = this->window();
        if (window && window->onCloseRequested)
            window->onCloseRequested();
        else if (window && !window->isPopup())
            Application::instance()->quit();
    };
}

void WindowHeader::setContentView(std::unique_ptr<View> view)
{
    if (m_content)
        takeChild(m_content);
    m_content = view.get();
    addChild(std::move(view));
    // Under the buttons.
    raiseChild(m_zone);
}

void WindowHeader::setMinimizeVisible(bool visible)
{
    m_minimize->setVisible(visible);
}

void WindowHeader::setMaximizeVisible(bool visible)
{
    m_maximize->setVisible(visible);
}

void WindowHeader::setMinimizeEnabled(bool enabled)
{
    m_minimize->setEnabled(enabled);
}

void WindowHeader::setMaximizeEnabled(bool enabled)
{
    m_maximize->setEnabled(enabled);
}

void WindowHeader::setCloseVisible(bool visible)
{
    m_close->setVisible(visible);
}

void WindowHeader::setCloseOnly(bool closeOnly)
{
    if (closeOnly == m_closeOnly)
        return;
    m_closeOnly = closeOnly;
    m_close->setArtwork(closeOnly ? "dialog-close" : "close");
    m_minimize->setVisible(!closeOnly);
    m_maximize->setVisible(!closeOnly);
    m_maximize->setEnabled(!closeOnly);
    requestLayout();
}

void WindowHeader::setContentUnderButtons(bool under)
{
    if (under == m_contentUnderButtons)
        return;
    m_contentUnderButtons = under;
    requestLayout();
}

void WindowHeader::setButtonMargin(double margin)
{
    if (margin == m_buttonMargin)
        return;
    m_buttonMargin = margin;
    requestLayout();
}

void WindowHeader::setButtonsCentered(bool centered)
{
    if (centered == m_buttonsCentered)
        return;
    m_buttonsCentered = centered;
    requestLayout();
}

void WindowHeader::setButtonSize(double size)
{
    if (size == m_buttonSize)
        return;
    m_buttonSize = size;
    requestLayout();
}

double WindowHeader::buttonsTop() const
{
    return m_zone->geometry().y;
}

void WindowHeader::layout()
{
    const double size = m_closeOnly ? kButton : m_buttonSize;
    const double slotInset = (kSlot - size) / 2;
    // A RowLayout centres on whole pixels.
    double top = m_buttonsCentered ? std::floor((geometry().height - size) / 2) : m_buttonMargin + slotInset;
    const double spacing = Theme::current().coloredWindowControls ? window_controls::kColoredSpacing
                                                                  : window_controls::kSpacing;
    double row = 0;
    for (HeaderButton* button : {m_close, m_minimize, m_maximize}) {
        if (button->isVisible())
            row += (row > 0 ? spacing : 0) + size;
    }
    // FishUI's RowLayout of a start margin item and the buttons' row: each
    // cell starts on a whole logical pixel, halves rounding up, so a
    // fractional margin moves the row by up to half a pixel.
    double rowX = std::floor(m_buttonMargin + 0.5) + slotInset;
    if (m_closeOnly) {
        rowX = window_controls::kDialogCloseInset;
        top = window_controls::kDialogCloseInset;
    }
    m_zone->setGeometry({rowX, top, row, size});
    double x = 0;
    for (HeaderButton* button : {m_close, m_minimize, m_maximize}) {
        if (!button->isVisible())
            continue;
        button->setGeometry({x, 0, size, size});
        x += size + spacing;
    }
    m_leadingInset = rowX + row + spacing;
    if (!m_content)
        return;
    // The content keeps the spacing after the row, as the ports expect.
    if (m_contentUnderButtons)
        m_content->setGeometry({0, 0, geometry().width, geometry().height});
    else
        m_content->setGeometry({m_leadingInset, 0, std::max(0.0, geometry().width - m_leadingInset), geometry().height});
}

void WindowHeader::pointerPress(const PointerEvent& event)
{
    Window* window = this->window();
    if (!window || event.button != MouseButton::Left || window->states().fullScreen)
        return;
    if (event.clickCount == 2) {
        toggleMaximized();
        return;
    }
    window->startMove();
}

void WindowHeader::toggleMaximized()
{
    Window* window = this->window();
    if (!window || window->states().fullScreen || !m_maximize->isEnabled())
        return;
    window->setMaximized(!window->states().maximized);
}

} // namespace cutefish::ui
