// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "base/signal.h"
#include "ui/view.h"

namespace cutefish::ui {

class Label;
class Window;

// A dialog button: a pill, in the accent colour when primary, in red
// when it confirms something that cannot be undone.
class DialogButton : public View
{
public:
    explicit DialogButton(std::string text = {}, bool primary = false);

    void setText(std::string text);
    void setPrimary(bool primary);
    void setDestructive(bool destructive);

    SizeF sizeHint() const override;
    RectF visualRect() const override;
    bool acceptsPointer() const override { return true; }
    bool acceptsFocus() const override { return true; }

    std::function<void()> onClicked;

protected:
    void layout() override;
    void paint(Painter& painter) override;
    void enabledChanged() override;
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;
    bool keyPress(const KeyEvent& event) override;
    bool keyRelease(const KeyEvent& event) override;

private:
    Color actionColor() const;

    Label* m_label;
    bool m_primary;
    bool m_destructive = false;
    bool m_hovered = false;
    bool m_pressed = false;
};

// A dialog: a modal window over its parent, its title on the line of
// its close button and its height that of its content.
class DialogWindow
{
public:
    DialogWindow(Window* parent, std::string title, double width = 400);
    virtual ~DialogWindow();

    DialogWindow(const DialogWindow&) = delete;
    DialogWindow& operator=(const DialogWindow&) = delete;

    void setTitle(std::string title);
    void setContentView(std::unique_ptr<View> view);
    template<typename T, typename... Args>
    T* setContent(Args&&... args)
    {
        auto view = std::make_unique<T>(std::forward<Args>(args)...);
        T* content = view.get();
        setContentView(std::move(view));
        return content;
    }
    // A back button, before the title, for pages that replace each other.
    void setBackVisible(bool visible);
    // Hidden for dialogs whose own Cancel or Close button closes them.
    void setCloseVisible(bool visible);

    void show();
    // Goes once the event being handled is over.
    void close();
    Window* window() const { return m_window.get(); }

    std::function<void()> onBack;
    std::function<void()> onClosed;

private:
    class Root;

    Window* m_parent;
    double m_width;
    std::unique_ptr<Window> m_window;
    Root* m_root;
};

// A confirmation: a title, an explanation and two buttons, the
// confirming one in the accent colour or, when it destroys something, red.
class ConfirmDialog
{
public:
    ConfirmDialog(Window* parent, std::string title, std::string text = {});
    ~ConfirmDialog();

    // Also while shown. Buttons left at "Confirm" and "Cancel" follow the
    // session language themselves.
    void setTitle(std::string title);
    void setText(std::string text);
    void setConfirmText(std::string text);
    void setCancelText(std::string text);
    void setDestructive(bool destructive);
    void setCancelVisible(bool visible);

    void show();
    Window* window() const { return m_window.get(); }

    std::function<void()> onAccepted;
    std::function<void()> onRejected;

private:
    void finish(bool accepted);
    void setButtonTexts(std::string confirm, std::string cancel);

    Window* m_parent;
    std::string m_title;
    std::string m_text;
    std::string m_confirm;
    std::string m_cancel;
    bool m_destructive = false;
    bool m_cancelVisible = true;
    std::unique_ptr<Window> m_window;
    Label* m_titleLabel = nullptr;
    Label* m_textLabel = nullptr;
    View* m_actions = nullptr;
    DialogButton* m_confirmButton = nullptr;
    DialogButton* m_cancelButton = nullptr;
    bool m_defaultConfirm = true;
    bool m_defaultCancel = true;
    base::Subscription m_languageChanged;
};

} // namespace cutefish::ui
