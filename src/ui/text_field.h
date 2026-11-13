// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "ui/color_mixer.h"
#include "ui/view.h"

namespace cutefish::ui {

// A single-line text field. Positions are UTF-8 byte offsets; input
// methods compose text in it while it has focus, and only then.
class TextField : public View
{
public:
    enum class EchoMode {
        Normal,
        // Dots in place of the text, which never reaches an input method or the clipboard.
        Password,
    };

    explicit TextField(std::string text = {});
    ~TextField() override;

    const std::string& text() const;
    // Moves the cursor to the end and clears the undo history.
    void setText(std::string text);
    void setPlaceholder(std::string placeholder);
    void setEchoMode(EchoMode mode);
    EchoMode echoMode() const;
    void setReadOnly(bool readOnly);
    // Hints for the input method: the content purpose and a mask of
    // TextHint (a field that allows prediction sets CompletionHint). A
    // password field takes no input method.
    void setInputPurpose(TextPurpose purpose);
    void setInputHints(uint32_t hints);
    bool isReadOnly() const;
    // The maximum length, in UTF-16 units; 0 for none.
    // Typing and pasting stop at it.
    void setMaximumLength(size_t length);
    // An edit the validator refuses does not happen.
    void setValidator(std::function<bool(const std::string& text)> validator);
    // Logical pixels; 0 follows the theme.
    void setFontSize(double size);
    // 400 unless set, as a CSS weight.
    void setFontWeight(int weight);
    // The text centred in the field while it fits, as horizontalAlignment
    // Text.AlignHCenter; the placeholder too.
    void setHorizontallyCentered(bool centred);
    // The text against the right margin while it fits, the caret after it;
    // the placeholder too.
    void setRightAligned(bool rightAligned);
    // Around the text, left and right; the theme's large spacing unless set.
    void setTextMargins(double left, double right);
    // The rounded background and its border; off for a field drawn into
    // something else.
    void setFrameVisible(bool visible);
    // A disabled field fades to half unless this is off, as for a bare
    // text input with the frame off and no margins.
    void setFadesWhenDisabled(bool fades);
    // The text and the caret, in every state; unset follows the theme.
    void setTextColor(std::optional<ColorRecipe> color);
    void setPlaceholderColor(std::optional<ColorRecipe> color);
    // The selection's background and text colours; unset, the theme's
    // highlight colours.
    void setSelectionColors(std::optional<ColorRecipe> background, std::optional<ColorRecipe> text);
    // A family as Label::setFontFamily takes it; empty follows the theme.
    void setFontFamily(std::string family);
    // What a password shows for each character; U+2022 unless set.
    void setPasswordCharacter(std::string character);
    // Logical; drawn as whole device pixels, at least one. Unset, one device
    // pixel per whole scale.
    void setCaretWidth(double width);
    // The cursor flash time: the caret is shown and hidden for half of it
    // each, on a coarse timer that may fire 5% early; 1000 unless set.
    void setCursorFlashTime(int milliseconds);
    // Selecting by mouse: off, a press only moves the cursor.
    void setSelectByMouse(bool select);
    // Off, copy, cut and paste do nothing, from keys or the menu.
    void setClipboardEnabled(bool enabled);
    // The menu on a right press; embedded windows never show one.
    void setContextMenuEnabled(bool enabled);
    // Empties the field.
    void clear();

    size_t cursorPosition() const;
    void setCursorPosition(size_t position);
    void select(size_t anchor, size_t cursor);
    void selectAll();
    bool hasSelection() const;
    std::string selectedText() const;
    // What the input method is composing, shown at the cursor.
    const std::string& preedit() const;

    void cut();
    void copy();
    void paste();
    void undo();
    void redo();

    // Where the text's baseline lies from the field's top, at its height.
    double baseline() const;

    SizeF sizeHint() const override;
    RectF visualRect() const override;
    bool acceptsPointer() const override { return true; }
    bool acceptsFocus() const override { return true; }
    CursorShape cursorShape() const override { return CursorShape::Text; }

    // Any change of the text, from the user or from code.
    std::function<void()> onTextChanged;
    // Changes the user made, as textEdited.
    std::function<void()> onTextEdited;
    // Return or Enter.
    std::function<void()> onAccepted;
    // Return, Enter or losing the focus, as editingFinished.
    std::function<void()> onEditingFinished;

protected:
    void paint(Painter& painter) override;
    void pointerPress(const PointerEvent& event) override;
    void pointerMove(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;
    void focusIn(FocusReason reason) override;
    void focusOut() override;
    void enabledChanged() override;
    bool keyPress(const KeyEvent& event) override;
    bool acceptsTextInput() const override;
    TextInputState textInputState() const override;
    void textInput(const TextInputEvent& event) override;
    // Before onTextChanged.
    virtual void textChanged() {}

private:
    struct Private;

    void showContextMenu(PointF position);
    // The width the text lies in, inside the margins.
    double contentWidth() const;
    // After the user's edit; false when it was refused and undone.
    bool edited();
    void moved();
    void startBlinking();

    std::unique_ptr<Private> d;
};

} // namespace cutefish::ui
