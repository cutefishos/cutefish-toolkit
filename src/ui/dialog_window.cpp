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
#include "ui/layout.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/translation.h"
#include "ui/window.h"

namespace cutefish::ui {

// The dialog's metrics.
static constexpr double kHeaderHeight = 44;
static constexpr double kButtonMargin = 8;
// The window controls, centred on the 32-pixel buttons' line.
static constexpr double kControl = 26;
static constexpr double kButton = 32;
static constexpr double kContentPadding = 16;
static constexpr double kTitleSize = 14;
static constexpr double kConfirmMinimumWidth = 336;
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
    m_label->setWeight(500);
    m_label->setFontSize(12);
    if (primary)
        m_label->setColor(ColorId::DialogButtonProminentText);
}

void DialogButton::setText(std::string text)
{
    m_label->setText(std::move(text));
}

void DialogButton::setPrimary(bool primary)
{
    m_primary = primary;
    m_label->setColor(primary ? std::optional<ColorRecipe>(ColorId::DialogButtonProminentText) : std::nullopt);
    requestPaint();
}

void DialogButton::setDestructive(bool destructive)
{
    m_destructive = destructive;
    requestPaint();
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

void DialogButton::layout()
{
    m_label->setGeometry(localRect());
}

Color DialogButton::actionColor() const
{
    return color(m_destructive ? ColorId::DestructiveAction : ColorId::Highlight);
}

void DialogButton::paint(Painter& painter)
{
    const RectF bounds = localRect();
    const double radius = bounds.height / 2;
    const Color surface = m_primary ? actionColor() : color(ColorId::DialogButtonBackground);
    Color fill = surface;
    if (isEnabled() && m_pressed)
        fill = surface.darker(1.08);
    else if (isEnabled() && m_hovered)
        fill = m_primary ? surface.lighter(1.05) : color(ColorId::DialogButtonBackgroundHovered);
    // Borders are one device pixel, 1 / scale; the inner edge
    // and the focus ring keep the button's shape through RoundedRectangle's inset.
    const double pixel = 1 / painter.scale();
    painter.drawRoundedRect(bounds, {radius}, fill, pixel,
                            m_primary ? actionColor().darker(1.25) : color(ColorId::DialogButtonBorder));
    const Color innerEdge = color(m_primary ? ColorId::DialogButtonProminentInnerEdge : ColorId::DialogButtonInnerEdge);
    if (innerEdge.a > 0) {
        painter.drawRoundedRect(bounds.adjusted(pixel, pixel, -pixel, -pixel), {radius, pixel}, Color{0, 0, 0, 0},
                                pixel, innerEdge);
    }
    if (hasVisibleFocus()) {
        painter.drawRoundedRect(bounds.adjusted(-kFocusRing, -kFocusRing, kFocusRing, kFocusRing),
                                {radius, -kFocusRing}, Color{0, 0, 0, 0}, 2,
                                m_primary ? actionColor().withAlpha(0.3) : color(ColorId::DialogButtonFocusRing));
    }
}

// A dialog button fades as a whole when disabled.
void DialogButton::enabledChanged()
{
    setOpacity(isEnabled() ? 1 : 0.5);
}

void DialogButton::pointerEnter()
{
    m_hovered = true;
    requestPaint();
}

void DialogButton::pointerLeave()
{
    m_hovered = false;
    requestPaint();
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
        , m_close(addChild<HeaderButton>("close", kControl))
        , m_back(addChild<HeaderButton>(std::string(), kButton))
        , m_title(addChild<Label>(std::move(title)))
    {
        m_title->setFontSize(kTitleSize);
        m_title->setVerticallyCentered(true);
        m_back->setGlyph("chevron-left");
        m_back->setVisible(false);
        m_back->onClicked = [this] {
            if (m_dialog->onBack)
                m_dialog->onBack();
        };
        m_close->setWindowControl(true);
        m_close->onClicked = [this] { m_dialog->close(); };
    }

    Label* title() const { return m_title; }
    HeaderButton* back() const { return m_back; }
    HeaderButton* closeButton() const { return m_close; }
    View* content() const { return m_content; }

    void setContent(std::unique_ptr<View> view)
    {
        if (m_content)
            takeChild(m_content);
        m_content = view.get();
        addChild(std::move(view));
    }

    double heightForWidth(double width) const override
    {
        const double content = m_content ? m_content->heightForWidth(width - 2 * kContentPadding) : 0;
        return kHeaderHeight + content + kContentPadding;
    }

    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const double width = geometry().width;
        // The title is centred on the buttons' line.
        const double line = kButtonMargin;
        double x = kButtonMargin;
        double left = kContentPadding;
        if (m_close->isVisible()) {
            m_close->setGeometry({x + (kButton - kControl) / 2, line + (kButton - kControl) / 2, kControl, kControl});
            x += kButton + Theme::current().smallSpacing;
            left = x;
        }
        if (m_back->isVisible()) {
            m_back->setGeometry({x, line, kButton, kButton});
            x += kButton + Theme::current().smallSpacing;
            left = x;
        }
        m_title->setGeometry({left, line, std::max(0.0, width - left - kButtonMargin), kButton});
        if (m_content) {
            m_content->setGeometry({kContentPadding, kHeaderHeight, width - 2 * kContentPadding,
                                    std::max(0.0, geometry().height - kHeaderHeight - kContentPadding)});
        }
    }

    // The empty parts of the window move it.
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && window())
            window()->startMove();
    }

private:
    DialogWindow* m_dialog;
    HeaderButton* m_close;
    HeaderButton* m_back;
    Label* m_title;
    View* m_content = nullptr;
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
    m_root->closeButton()->setVisible(visible);
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
    m_confirmButton->setText(m_confirm);
    if (m_cancelButton)
        m_cancelButton->setText(m_cancel);
    m_window->resize({std::max(kConfirmMinimumWidth, m_actions->sizeHint().width + 2 * kContentPadding), 0});
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
        m_titleLabel->setText(m_title);
    }
}

void ConfirmDialog::setText(std::string text)
{
    m_text = std::move(text);
    if (m_window) {
        m_textLabel->setText(m_text);
        m_textLabel->setVisible(!m_text.empty());
    }
}

// The dialog's content, with the window's close button over its corner
// when there is no Cancel button to do its work.
class ConfirmContent final : public View
{
public:
    ConfirmContent(std::unique_ptr<View> content, std::function<void()> close)
        : m_content(content.get())
    {
        addChild(std::move(content));
        if (close) {
            m_close = addChild<HeaderButton>("close", kControl);
            m_close->setWindowControl(true);
            m_close->onClicked = std::move(close);
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
        if (m_close)
            m_close->setGeometry({kButtonMargin + (kButton - kControl) / 2, kButtonMargin + (kButton - kControl) / 2,
                                  kControl, kControl});
    }

    // The empty parts of the window move it.
    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && window())
            window()->startMove();
    }

private:
    View* m_content;
    HeaderButton* m_close = nullptr;
};

void ConfirmDialog::show()
{
    m_window = std::make_unique<Window>();
    m_window->setBackgroundColor(ColorId::SecondBackground);
    m_window->setTransientParent(m_parent, true);
    m_window->setTitle(m_title);
    m_window->onCloseRequested = [this] { finish(false); };

    auto column = std::make_unique<Column>();
    column->setMargins({kContentPadding, kContentPadding, kContentPadding, 12});
    column->setSpacing(kContentPadding);

    auto* texts = column->addChild<Column>();
    texts->setSpacing(4);
    texts->setLayoutHints({.fillWidth = true});
    // Clear of the close corner of the window.
    if (!m_cancelVisible)
        texts->setMargins({32, 0, 0, 0});
    Label* title = texts->addChild<Label>(m_title);
    m_titleLabel = title;
    title->setFontSize(kTitleSize);
    title->setWeight(600);
    title->setWrap(true);
    title->setLayoutHints({.fillWidth = true});
    // Kept when empty, so a text set while shown has its place.
    Label* text = texts->addChild<Label>(m_text);
    text->setFontSize(kTitleSize);
    text->setColor(ColorId::DisabledText);
    text->setWrap(true);
    text->setLayoutHints({.fillWidth = true});
    text->setVisible(!m_text.empty());
    m_textLabel = text;

    auto* actions = column->addChild<Row>();
    actions->setSpacing(8);
    actions->setLayoutHints({.alignment = Alignment::End});
    DialogButton* cancel = nullptr;
    if (m_cancelVisible) {
        cancel = actions->addChild<DialogButton>(m_cancel);
        cancel->onClicked = [this] { finish(false); };
    }
    DialogButton* confirm = actions->addChild<DialogButton>(m_confirm, true);
    m_actions = actions;
    m_confirmButton = confirm;
    m_cancelButton = cancel;
    confirm->setDestructive(m_destructive);
    confirm->onClicked = [this] { finish(true); };

    // Escape cancels; Return confirms even while Cancel has the focus.
    m_window->addShortcut(NoModifier, key::Escape, [this] { finish(false); });
    m_window->addShortcut(NoModifier, key::Return, [this] { finish(true); });
    m_window->addShortcut(NoModifier, key::KeypadEnter, [this] { finish(true); });
    // Measured as the window will lay the buttons out, before they are in it.
    column->setTextLayoutMode(m_window->textLayoutMode());
    const double width = std::max(kConfirmMinimumWidth, actions->sizeHint().width + 2 * kContentPadding);
    std::function<void()> close;
    if (!m_cancelVisible)
        close = [this] { finish(false); };
    m_window->setContentView(std::make_unique<ConfirmContent>(std::move(column), std::move(close)));
    m_window->resize({width, 0});
    m_window->setFitsContentHeight(true);
    // A destructive action waits for a deliberate choice.
    (m_destructive && cancel ? static_cast<View*>(cancel) : static_cast<View*>(confirm))->setFocus();
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
