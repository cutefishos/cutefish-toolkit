// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/dialog_window.h"

#include <algorithm>
#include <cmath>

#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/header_button.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/translation.h"
#include "ui/window.h"
#include "ui/window_controls.h"
#include "ui/window_header.h"

namespace cutefish::ui {

// The dialog's metrics.
static constexpr double kHeaderHeight = 44;
static constexpr double kButtonMargin = 8;
static constexpr double kButton = 32;
// With the window controls, the header is a title bar: 32 high, the controls'
// first centre 16 in from the top and the start, the title centred on the
// window in a smaller size.
static constexpr double kControlsMargin = 0;
static constexpr double kControlsHeaderHeight = 32;
static constexpr double kControlsTitleSize = 12;
// The content sits a little under the title bar.
static constexpr double kControlsContentGap = 4;
// The three window controls, inset in the buttons' line as one control in a
// 32-pixel slot.
static constexpr double kControlsWidth = window_controls::kRowWidth + (kButton - window_controls::kButton);
static constexpr double kContentPadding = 16;
static constexpr double kTitleSize = 14;
static constexpr double kButtonTextSize = 13;
// The alert's metrics. Its labels' lines start at their tops and leave two
// pixels under the last line.
static constexpr double kAlertInset = 16;
static constexpr double kAlertTextInset = 22;
static constexpr double kAlertTop = 19;
static constexpr double kAlertTextSize = 13;
static constexpr double kAlertLineHeight = 16;
static constexpr double kAlertLabelSlack = 2;
static constexpr double kAlertTextGap = 9;
static constexpr double kAlertButtonsGap = 14;
static constexpr double kAlertButtonHeight = 28;
static constexpr double kAlertButtonSpacing = 8;
static constexpr double kAlertButtonPadding = 12;
static constexpr int kBold = 700;
// The keyboard focus ring outside a dialog button.
static constexpr double kFocusRing = 3;

// Destroys the window from the event loop, after the event being handled.
static void release(std::unique_ptr<Window>& window)
{
    if (!window)
        return;
    std::shared_ptr<Window> doomed(std::move(window));
    Application::instance()->loop().post([doomed] {});
}

DialogButton::DialogButton(std::string text, bool primary)
    : m_label(addChild<Label>(std::move(text)))
    , m_primary(primary)
{
    m_label->setTextAlignment(Label::TextAlignment::Center);
    m_label->setVerticallyCentered(true);
    m_label->setFontSize(kButtonTextSize);
    updateTextColor();
}

void DialogButton::setText(std::string text)
{
    m_label->setText(std::move(text));
}

void DialogButton::setPrimary(bool primary)
{
    m_primary = primary;
    updateTextColor();
    requestPaint();
}

void DialogButton::setDestructive(bool destructive)
{
    m_destructive = destructive;
    updateTextColor();
    requestPaint();
}

void DialogButton::updateTextColor()
{
    if (m_destructive)
        m_label->setColor(ColorId::DialogButtonDestructiveText);
    else if (m_primary)
        m_label->setColor(ColorId::DialogButtonProminentText);
    else
        m_label->setColor(ColorId::AlertText);
}

RectF DialogButton::visualRect() const
{
    return localRect().adjusted(-kFocusRing, -kFocusRing, kFocusRing, kFocusRing);
}

SizeF DialogButton::sizeHint() const
{
    const SizeF text = m_label->sizeHint();
    return {std::max(64.0, text.width + 2 * Theme::current().largeSpacing), std::max(32.0, text.height)};
}

double DialogButton::textWidth() const
{
    return m_label->sizeHint().width;
}

void DialogButton::layout()
{
    m_label->setGeometry(localRect());
}

// Pressing lightens the accent a tenth towards white in dark mode and
// darkens it a tenth towards black in light.
static Color pressedAccent(Color accent)
{
    const double target = Theme::current().darkMode ? 255 : 0;
    const auto mix = [target](uint8_t channel) { return uint8_t(std::lround(channel + (target - channel) * 0.1)); };
    return {mix(accent.r), mix(accent.g), mix(accent.b), accent.a};
}

void DialogButton::paint(Painter& painter)
{
    const RectF bounds = localRect();
    const double radius = bounds.height / 2;
    const bool pressed = isEnabled() && m_pressed;
    if (m_destructive) {
        painter.drawRoundedRect(bounds, {radius}, color(ColorId::DialogButtonDestructiveBackground));
        if (pressed) {
            const bool dark = Theme::current().darkMode;
            const Color veil = (dark ? Color::rgb(0xFFFFFF) : Color::rgb(0x000000)).withAlpha(dark ? 0.035 : 0.04);
            painter.drawRoundedRect(bounds, {radius}, veil);
        }
    } else if (m_primary) {
        const Color accent = color(ColorId::Highlight);
        painter.drawRoundedRect(bounds, {radius}, pressed ? pressedAccent(accent) : accent);
    } else {
        painter.drawRoundedRect(
            bounds, {radius},
            color(pressed ? ColorId::DialogButtonBackgroundPressed : ColorId::DialogButtonBackground));
    }
    if (hasVisibleFocus()) {
        const Color ring = m_primary && !m_destructive ? color(ColorId::Highlight).withAlpha(0.3)
                                                       : color(ColorId::DialogButtonFocusRing);
        painter.drawRoundedRect(bounds.adjusted(-kFocusRing, -kFocusRing, kFocusRing, kFocusRing),
                                {radius, -kFocusRing}, Color{0, 0, 0, 0}, 2, ring);
    }
}

// A dialog button fades as a whole when disabled.
void DialogButton::enabledChanged()
{
    setOpacity(isEnabled() ? 1 : 0.5);
}

void DialogButton::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    m_pressed = true;
    requestPaint();
}

void DialogButton::pointerRelease(const PointerEvent& event)
{
    if (!m_pressed)
        return;
    m_pressed = false;
    requestPaint();
    if (localRect().contains(event.position) && onClicked)
        onClicked();
}

// Space presses it and the release clicks; Return is left to the dialog.
bool DialogButton::keyPress(const KeyEvent& event)
{
    if (event.key != key::Space || event.modifiers != NoModifier)
        return false;
    m_pressed = true;
    requestPaint();
    return true;
}

bool DialogButton::keyRelease(const KeyEvent& event)
{
    if (event.key != key::Space || !m_pressed)
        return false;
    m_pressed = false;
    requestPaint();
    if (onClicked)
        onClicked();
    return true;
}

// The dialog's view: the header line with the title and buttons, the
// content below with the dialog's padding.
class DialogWindow::Root : public View
{
public:
    explicit Root(DialogWindow* dialog, std::string title)
        : m_dialog(dialog)
        , m_header(addChild<WindowHeader>(kHeaderHeight))
        , m_back(addChild<HeaderButton>(std::string(), kButton))
        , m_title(addChild<Label>(std::move(title)))
    {
        m_header->setButtonMargin(kControlsMargin);
        m_header->setMinimizeEnabled(false);
        m_header->setMaximizeEnabled(false);
        m_title->setFontSize(kControlsTitleSize);
        m_title->setTextAlignment(Label::TextAlignment::Center);
        m_title->setVerticallyCentered(true);
        m_back->setGlyph("chevron-left");
        m_back->setVisible(false);
        m_back->onClicked = [this] {
            if (m_dialog->onBack)
                m_dialog->onBack();
        };
    }

    Label* title() const { return m_title; }
    HeaderButton* back() const { return m_back; }
    void setControlsVisible(bool visible)
    {
        m_header->setCloseVisible(visible);
        m_header->setMinimizeVisible(visible);
        m_header->setMaximizeVisible(visible);
        m_controlsVisible = visible;
        m_title->setFontSize(visible ? kControlsTitleSize : kTitleSize);
        m_title->setTextAlignment(visible ? Label::TextAlignment::Center : Label::TextAlignment::Left);
        requestLayout();
    }
    View* content() const { return m_content; }
    void setHeight(double height)
    {
        m_height = height;
        requestLayout();
    }

    void setContent(std::unique_ptr<View> view)
    {
        if (m_content)
            takeChild(m_content);
        m_content = view.get();
        addChild(std::move(view));
    }

    double heightForWidth(double width) const override
    {
        if (m_height > 0)
            return m_height;
        const double content = m_content ? m_content->heightForWidth(width - 2 * kContentPadding) : 0;
        return contentTop() + content + kContentPadding;
    }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        // The title is centred on the buttons' line.
        const double header = headerHeight();
        const double line = m_controlsVisible ? (header - kButton) / 2 : kButtonMargin;
        double x = kButtonMargin;
        double left = kContentPadding;
        m_header->setGeometry({0, 0, width, header});
        if (m_controlsVisible) {
            x = kControlsMargin + kControlsWidth + Theme::current().smallSpacing;
            left = x;
        }
        if (m_back->isVisible()) {
            m_back->setGeometry({x, line, kButton, kButton});
            x += kButton + Theme::current().smallSpacing;
            left = x;
        }
        // Centred on the window: as far from the end as the buttons take at the start.
        const double right = m_controlsVisible ? left : kButtonMargin;
        m_title->setGeometry({left, line, std::max(0.0, width - left - right), kButton});
        if (m_content) {
            const double top = contentTop();
            m_content->setGeometry({kContentPadding, top, width - 2 * kContentPadding,
                                    std::max(0.0, geometry().height - top - kContentPadding)});
        }
    }

    // The empty parts of the window move it.
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && window())
            window()->startMove();
    }

private:
    double headerHeight() const { return m_controlsVisible ? kControlsHeaderHeight : kHeaderHeight; }
    double contentTop() const { return m_controlsVisible ? kControlsHeaderHeight + kControlsContentGap : kHeaderHeight; }

    DialogWindow* m_dialog;
    WindowHeader* m_header;
    HeaderButton* m_back;
    Label* m_title;
    View* m_content = nullptr;
    bool m_controlsVisible = true;
    double m_height = 0;
};

DialogWindow::DialogWindow(Window* parent, std::string title, double width)
    : m_parent(parent)
    , m_width(width)
    , m_window(std::make_unique<Window>())
{
    auto root = std::make_unique<Root>(this, std::move(title));
    m_root = root.get();
    m_window->setContentView(std::move(root));
    m_window->setTransientParent(parent, true);
    m_window->onCloseRequested = [this] { close(); };
}

DialogWindow::~DialogWindow() = default;

void DialogWindow::setTitle(std::string title)
{
    m_root->title()->setText(title);
    if (m_window)
        m_window->setTitle(title);
}

void DialogWindow::setContentView(std::unique_ptr<View> view)
{
    m_root->setContent(std::move(view));
}

void DialogWindow::setBackVisible(bool visible)
{
    m_root->back()->setVisible(visible);
}

void DialogWindow::setCloseVisible(bool visible)
{
    m_root->setControlsVisible(visible);
}

void DialogWindow::setHeight(double height)
{
    m_root->setHeight(height);
}

void DialogWindow::show()
{
    if (!m_window)
        return;
    m_window->setTitle(m_root->title()->text());
    m_window->resize({m_width, 0});
    m_window->setFitsContentHeight(true);
    m_window->show();
}

void DialogWindow::close()
{
    release(m_window);
    if (onClosed)
        onClosed();
}

AlertView::AlertView()
    : m_title(addChild<Label>())
    , m_text(addChild<Label>())
    , m_cancel(addChild<DialogButton>())
    , m_confirm(addChild<DialogButton>(std::string(), true))
    , m_topInset(kAlertTop)
{
    for (Label* label : {m_title, m_text}) {
        label->setFontSize(kAlertTextSize);
        label->setFixedLineHeight(kAlertLineHeight);
        label->setColor(ColorId::AlertText);
        label->setWrap(true);
    }
    m_title->setWeight(kBold);
    m_text->setVisible(false);
}

void AlertView::setText(std::string text)
{
    m_text->setVisible(!text.empty());
    m_text->setText(std::move(text));
    requestLayout();
}

void AlertView::setCancelVisible(bool visible)
{
    m_cancel->setVisible(visible);
    requestLayout();
}

void AlertView::setTopInset(double inset)
{
    m_topInset = inset;
    requestLayout();
}

bool AlertView::buttonsSideBySide() const
{
    if (!m_cancel->isVisible())
        return false;
    const double width = (kWidth - 2 * kAlertInset - kAlertButtonSpacing) / 2;
    return m_cancel->textWidth() + 2 * kAlertButtonPadding <= width
        && m_confirm->textWidth() + 2 * kAlertButtonPadding <= width;
}

SizeF AlertView::sizeHint() const
{
    return {kWidth, heightForWidth(kWidth)};
}

double AlertView::heightForWidth(double width) const
{
    const double textWidth = width - 2 * kAlertTextInset;
    double height = m_topInset + m_title->heightForWidth(textWidth) + kAlertLabelSlack;
    if (m_text->isVisible())
        height += kAlertTextGap + m_text->heightForWidth(textWidth) + kAlertLabelSlack;
    const int rows = buttonsSideBySide() || !m_cancel->isVisible() ? 1 : 2;
    height += kAlertButtonsGap + rows * kAlertButtonHeight + (rows - 1) * kAlertButtonSpacing + kAlertInset;
    return std::ceil(height);
}

void AlertView::layout()
{
    const double width = geometry().width;
    const double textWidth = width - 2 * kAlertTextInset;
    double y = m_topInset;
    const double titleHeight = m_title->heightForWidth(textWidth);
    m_title->setGeometry({kAlertTextInset, y, textWidth, titleHeight});
    y += titleHeight + kAlertLabelSlack;
    if (m_text->isVisible()) {
        y += kAlertTextGap;
        const double textHeight = m_text->heightForWidth(textWidth);
        m_text->setGeometry({kAlertTextInset, y, textWidth, textHeight});
        y += textHeight + kAlertLabelSlack;
    }
    y += kAlertButtonsGap;
    const double full = width - 2 * kAlertInset;
    if (buttonsSideBySide()) {
        const double buttonWidth = (full - kAlertButtonSpacing) / 2;
        m_cancel->setGeometry({kAlertInset, y, buttonWidth, kAlertButtonHeight});
        m_confirm->setGeometry(
            {kAlertInset + buttonWidth + kAlertButtonSpacing, y, buttonWidth, kAlertButtonHeight});
        return;
    }
    m_confirm->setGeometry({kAlertInset, y, full, kAlertButtonHeight});
    if (m_cancel->isVisible())
        m_cancel->setGeometry({kAlertInset, y + kAlertButtonHeight + kAlertButtonSpacing, full, kAlertButtonHeight});
}

ConfirmDialog::ConfirmDialog(Window* parent, std::string title, std::string text)
    : m_parent(parent)
    , m_title(std::move(title))
    , m_text(std::move(text))
    , m_confirm(tr("Confirm"))
    , m_cancel(tr("Cancel"))
{
    m_languageChanged = Application::instance()->languageChanged.connect([this] {
        setButtonTexts(m_defaultConfirm ? tr("Confirm") : m_confirm, m_defaultCancel ? tr("Cancel") : m_cancel);
    });
}

ConfirmDialog::~ConfirmDialog() = default;

void ConfirmDialog::setConfirmText(std::string text)
{
    m_defaultConfirm = false;
    setButtonTexts(std::move(text), m_cancel);
}

void ConfirmDialog::setCancelText(std::string text)
{
    m_defaultCancel = false;
    setButtonTexts(m_confirm, std::move(text));
}

void ConfirmDialog::setButtonTexts(std::string confirm, std::string cancel)
{
    m_confirm = std::move(confirm);
    m_cancel = std::move(cancel);
    if (!m_window)
        return;
    m_alert->confirm()->setText(m_confirm);
    m_alert->cancel()->setText(m_cancel);
    m_window->resize({AlertView::kWidth, 0});
}

void ConfirmDialog::setDestructive(bool destructive)
{
    m_destructive = destructive;
}

void ConfirmDialog::setCancelVisible(bool visible)
{
    m_cancelVisible = visible;
}

void ConfirmDialog::setTitle(std::string title)
{
    m_title = std::move(title);
    if (m_window) {
        m_window->setTitle(m_title);
        m_alert->title()->setText(m_title);
    }
}

void ConfirmDialog::setText(std::string text)
{
    m_text = std::move(text);
    if (m_window)
        m_alert->setText(m_text);
}

// The dialog's content, with the window controls over its corner when
// there is no Cancel button to do their work.
class ConfirmContent final : public View
{
public:
    ConfirmContent(std::unique_ptr<View> content, bool controls)
        : m_content(content.get())
    {
        addChild(std::move(content));
        if (controls) {
            m_header = addChild<WindowHeader>(kControlsHeaderHeight);
            m_header->setButtonMargin(kControlsMargin);
            m_header->setMinimizeEnabled(false);
            m_header->setMaximizeEnabled(false);
        }
    }

    SizeF sizeHint() const override { return m_content->sizeHint(); }
    double heightForWidth(double width) const override { return m_content->heightForWidth(width); }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const RectF bounds = localRect();
        m_content->setGeometry(bounds);
        if (m_header)
            m_header->setGeometry({0, 0, bounds.width, kControlsHeaderHeight});
    }

    // The empty parts of the window move it.
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && window())
            window()->startMove();
    }

private:
    View* m_content;
    WindowHeader* m_header = nullptr;
};

void ConfirmDialog::show()
{
    m_window = std::make_unique<Window>();
    m_window->setBackgroundColor(ColorId::AlertBackground);
    m_window->setTransientParent(m_parent, true);
    m_window->setTitle(m_title);
    m_window->onCloseRequested = [this] { finish(false); };

    auto alert = std::make_unique<AlertView>();
    m_alert = alert.get();
    // Under the window controls' line when they show.
    if (!m_cancelVisible)
        alert->setTopInset(kControlsHeaderHeight);
    alert->title()->setText(m_title);
    alert->setText(m_text);
    alert->setCancelVisible(m_cancelVisible);
    DialogButton* cancel = alert->cancel();
    DialogButton* confirm = alert->confirm();
    cancel->setText(m_cancel);
    cancel->onClicked = [this] { finish(false); };
    confirm->setText(m_confirm);
    confirm->setDestructive(m_destructive);
    confirm->onClicked = [this] { finish(true); };

    // Escape cancels; Return confirms even while Cancel has the focus.
    m_window->addShortcut(NoModifier, key::Escape, [this] { finish(false); });
    m_window->addShortcut(NoModifier, key::Return, [this] { finish(true); });
    m_window->addShortcut(NoModifier, key::KeypadEnter, [this] { finish(true); });
    m_window->setContentView(std::make_unique<ConfirmContent>(std::move(alert), !m_cancelVisible));
    m_window->resize({AlertView::kWidth, 0});
    m_window->setFitsContentHeight(true);
    // A destructive action waits for a deliberate choice.
    (m_destructive && m_cancelVisible ? cancel : confirm)->setFocus();
    m_window->show();
}

void ConfirmDialog::finish(bool accepted)
{
    release(m_window);
    if (accepted && onAccepted)
        onAccepted();
    else if (!accepted && onRejected)
        onRejected();
}

} // namespace cutefish::ui
