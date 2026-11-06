// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/text_field.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"

#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/clipboard.h"
#include "ui/editable_text.h"
#include "ui/keys.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/text_context.h"
#include "ui/text_lines.h"
#include "ui/theme.h"
#include "ui/translation.h"
#include "ui/window.h"

namespace textlayout = skia::textlayout;

namespace cutefish::ui {

static constexpr double kMinimumWidth = 200;
// A medium icon and half the small spacing.
static constexpr double kHeight = 35;
// The focus ring, outside the field.
static constexpr double kFocusRing = 2;
// The caret blinks on a coarse timer of half the cursor flash time, which
// fires up to 5% early: 475 ms for 1000.
static constexpr int kCursorFlashTime = 1000;
// Wide enough that a single line never wraps.
static constexpr double kUnboundedWidth = 1e6;
// U+2022 BULLET, one per grapheme at the field's font size, as
// gfx::RenderText::kPasswordReplacementChar.
static constexpr const char* kPasswordDot = "\u2022";
// The surrounding text an input method gets around the cursor.
static constexpr size_t kSurroundingLimit = 4000;

// UTF-16 code units before the byte offset, as the paragraph counts.
static size_t utf16Index(const std::string& text, size_t offset)
{
    size_t index = 0;
    for (size_t i = 0; i < offset && i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xc0) != 0x80)
            index += byte >= 0xf0 ? 2 : 1;
    }
    return index;
}

static size_t utf8Offset(const std::string& text, size_t index)
{
    size_t units = 0;
    size_t i = 0;
    for (; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xc0) == 0x80)
            continue;
        if (units >= index)
            break;
        units += byte >= 0xf0 ? 2 : 1;
    }
    return i;
}

struct TextField::Private
{
    EditableText edit;
    std::string placeholder;
    EchoMode echo = EchoMode::Normal;
    bool readOnly = false;
    size_t maximumLength = 0;
    // The text revision as the last edit or move left it, to tell a change.
    uint64_t savedRevision = 0;
    // With a validator, the editor as the last edit or move left it, with
    // its history, for a refused edit to return to.
    EditableText savedEdit;
    std::function<bool(const std::string&)> validator;
    double fontSize = 0;
    int weight = 400;
    bool centred = false;
    bool rightAligned = false;
    std::string family;
    std::optional<ColorRecipe> textColor;
    std::optional<ColorRecipe> placeholderColor;
    std::optional<ColorRecipe> selectionColor;
    std::optional<ColorRecipe> selectedTextColor;
    std::string mask = kPasswordDot;
    double caretWidth = 0;
    int cursorFlashTime = kCursorFlashTime;
    bool fadesWhenDisabled = true;
    bool selectByMouse = true;
    bool clipboardEnabled = true;
    bool contextMenuEnabled = true;
    double leftMargin = -1;
    double rightMargin = -1;
    bool frameVisible = true;

    double left() const { return leftMargin >= 0 ? leftMargin : Theme::current().largeSpacing; }
    double right() const { return rightMargin >= 0 ? rightMargin : Theme::current().largeSpacing; }
    double pixelSize() const { return TextContext::pixelSize(fontSize > 0 ? fontSize : Theme::current().fontSize()); }
    std::string preedit;
    int preeditCursor = -1;
    // Set by the field's owner, for the input method.
    TextPurpose purpose = TextPurpose::Normal;
    uint32_t hints = NoTextHint;

    // Laid out in logical pixels.
    std::unique_ptr<textlayout::Paragraph> paragraph;
    std::string display;
    bool paragraphEnabled = true;
    uint64_t paragraphTheme = 0;
    bool dirty = true;
    std::unique_ptr<textlayout::Paragraph> placeholderParagraph;
    uint64_t placeholderTheme = 0;

    // Logical pixels the text is scrolled by to keep the cursor in sight.
    double scrollX = 0;
    bool caretVisible = true;
    int blinkTimer = 0;
    bool selecting = false;
    int granularity = 1;
    std::pair<size_t, size_t> anchorWord;
    std::shared_ptr<TextField*> self;
    std::unique_ptr<Menu> menu;

    // The text as shown: the preedit inside it at the cursor, or dots.
    std::string displayText() const
    {
        if (echo == EchoMode::Password) {
            std::string dots;
            for (size_t at = 0; at < edit.text().size(); at = edit.nextGrapheme(at))
                dots += mask;
            return dots;
        }
        if (preedit.empty())
            return edit.text();
        std::string text = edit.text();
        text.insert(edit.cursor(), preedit);
        return text;
    }

    size_t toDisplay(size_t offset) const
    {
        if (echo == EchoMode::Password) {
            size_t graphemes = 0;
            for (size_t at = 0; at < offset; at = edit.nextGrapheme(at))
                ++graphemes;
            return graphemes * mask.size();
        }
        return offset <= edit.cursor() ? offset : offset + preedit.size();
    }

    size_t fromDisplay(size_t offset) const
    {
        if (echo == EchoMode::Password) {
            size_t at = 0;
            for (size_t dots = offset / std::max<size_t>(1, mask.size()); dots > 0 && at < edit.text().size(); --dots)
                at = edit.nextGrapheme(at);
            return at;
        }
        const size_t cursor = edit.cursor();
        if (offset <= cursor)
            return offset;
        if (offset >= cursor + preedit.size())
            return offset - preedit.size();
        return cursor;
    }

    // Where the caret shows, in the display text.
    size_t caretDisplayOffset() const
    {
        const size_t cursor = toDisplay(edit.cursor());
        if (!preedit.empty() && preeditCursor >= 0)
            return cursor + std::min(size_t(preeditCursor), preedit.size());
        return cursor;
    }

    textlayout::Paragraph* layout(bool enabled)
    {
        if (paragraph && !dirty && paragraphEnabled == enabled && paragraphTheme == Theme::generation())
            return paragraph.get();

        const TextContext& context = Application::instance()->text();
        const Theme& theme = Theme::current();
        const Color color = textColor ? textColor->resolve() : enabled ? theme.text : theme.disabledText;
        const textlayout::TextStyle normal = context.style(fontSize, weight, color, family);
        textlayout::ParagraphStyle style;
        style.setTextStyle(normal);
        style.setMaxLines(1);
        style.setApplyRoundingHack(false);

        display = displayText();
        auto builder = textlayout::ParagraphBuilder::make(style, context.fontCollection(),
                                                          context.unicode());
        builder->pushStyle(normal);
        const auto add = [&builder, this](size_t start, size_t end) {
            if (end > start)
                builder->addText(display.data() + start, end - start);
        };
        if (!preedit.empty()) {
            const size_t start = edit.cursor();
            textlayout::TextStyle underlined = normal;
            underlined.setDecoration(textlayout::TextDecoration::kUnderline);
            underlined.setDecorationColor(color.argb());
            add(0, start);
            builder->pushStyle(underlined);
            add(start, start + preedit.size());
            builder->pop();
            add(start + preedit.size(), display.size());
        } else if (edit.hasSelection()) {
            const size_t start = toDisplay(edit.selectionStart());
            const size_t end = toDisplay(edit.selectionEnd());
            textlayout::TextStyle selected = normal;
            selected.setColor((selectedTextColor ? selectedTextColor->resolve() : theme.highlightedText).argb());
            add(0, start);
            builder->pushStyle(selected);
            add(start, end);
            builder->pop();
            add(end, display.size());
        } else {
            add(0, display.size());
        }
        builder->pop();
        paragraph = builder->Build();
        paragraph->layout(SkScalar(kUnboundedWidth));
        paragraphEnabled = enabled;
        paragraphTheme = Theme::generation();
        dirty = false;
        return paragraph.get();
    }

    textlayout::Paragraph* placeholderLayout()
    {
        if (placeholderParagraph && placeholderTheme == Theme::generation())
            return placeholderParagraph.get();
        const TextContext& context = Application::instance()->text();
        const Color ink = placeholderColor ? placeholderColor->resolve() : color(ColorId::TextFieldPlaceholder);
        const textlayout::TextStyle style = context.style(fontSize, weight, ink, family);
        textlayout::ParagraphStyle paragraphStyle;
        paragraphStyle.setTextStyle(style);
        if (centred)
            paragraphStyle.setTextAlign(textlayout::TextAlign::kCenter);
        else if (rightAligned)
            paragraphStyle.setTextAlign(textlayout::TextAlign::kRight);
        paragraphStyle.setApplyRoundingHack(false);
        paragraphStyle.setMaxLines(1);
        paragraphStyle.setEllipsis(SkString("…"));
        auto builder = textlayout::ParagraphBuilder::make(paragraphStyle, context.fontCollection(),
                                                          context.unicode());
        builder->pushStyle(style);
        builder->addText(placeholder.data(), placeholder.size());
        builder->pop();
        placeholderParagraph = builder->Build();
        placeholderTheme = Theme::generation();
        return placeholderParagraph.get();
    }

    // Where centred or right-aligned text starts in a content box of the
    // width while it fits, as TextInput's horizontal alignment, else at the
    // start. Right-aligned text leaves room for the widest caret paint draws.
    double alignX(double width) const
    {
        if ((!centred && !rightAligned) || !paragraph)
            return 0;
        const double used = paragraph->getMaxIntrinsicWidth();
        if (centred)
            return used <= width ? (width - used) / 2 : 0;
        constexpr double kCaretRoom = 1.5;
        return used + kCaretRoom <= width ? width - used - kCaretRoom : 0;
    }

    // Logical pixels from the start of the text to the caret at a display offset.
    double caretX(size_t offset) const
    {
        if (display.empty() || !paragraph)
            return 0;
        const size_t index = utf16Index(display, offset);
        const size_t length = utf16Index(display, display.size());
        textlayout::Paragraph::GlyphInfo info;
        if (index < length && paragraph->getGlyphInfoAtUTF16Offset(index, &info)) {
            return info.fDirection == textlayout::TextDirection::kRtl
                    ? info.fGraphemeLayoutBounds.right()
                    : info.fGraphemeLayoutBounds.left();
        }
        if (index > 0 && paragraph->getGlyphInfoAtUTF16Offset(index - 1, &info)) {
            return info.fDirection == textlayout::TextDirection::kRtl
                    ? info.fGraphemeLayoutBounds.left()
                    : info.fGraphemeLayoutBounds.right();
        }
        return 0;
    }

    // The display offset nearest to a logical x from the start of the text.
    size_t displayOffsetAt(double x) const
    {
        if (display.empty() || !paragraph)
            return 0;
        textlayout::Paragraph::GlyphInfo info;
        if (!paragraph->getClosestUTF16GlyphInfoAt(SkScalar(x), paragraph->getHeight() / 2, &info))
            return 0;
        const SkRect& bounds = info.fGraphemeLayoutBounds;
        bool before = x < bounds.centerX();
        if (info.fDirection == textlayout::TextDirection::kRtl)
            before = !before;
        const size_t index = before ? info.fGraphemeClusterTextRange.start
                                    : info.fGraphemeClusterTextRange.end;
        return utf8Offset(display, index);
    }
};

TextField::TextField(std::string text)
    : d(std::make_unique<Private>())
{
    d->self = std::make_shared<TextField*>(this);
    d->edit.setText(std::move(text));
    d->savedRevision = d->edit.revision();
}

TextField::~TextField()
{
    if (d->blinkTimer)
        Application::instance()->loop().stopTimer(d->blinkTimer);
}

const std::string& TextField::text() const
{
    return d->edit.text();
}

void TextField::setText(std::string text)
{
    if (text == d->edit.text())
        return;
    d->edit.setText(std::move(text));
    d->preedit.clear();
    moved();
    textChanged();
    if (onTextChanged)
        onTextChanged();
}

void TextField::setMaximumLength(size_t length)
{
    d->maximumLength = length;
}

void TextField::setValidator(std::function<bool(const std::string& text)> validator)
{
    d->validator = std::move(validator);
    if (d->validator)
        d->savedEdit = d->edit;
}

void TextField::setFontSize(double size)
{
    if (size == d->fontSize)
        return;
    d->fontSize = size;
    d->dirty = true;
    d->placeholderParagraph.reset();
    requestLayout();
    requestPaint();
}

void TextField::setTextMargins(double left, double right)
{
    if (left == d->leftMargin && right == d->rightMargin)
        return;
    d->leftMargin = left;
    d->rightMargin = right;
    requestLayout();
}

void TextField::setFontWeight(int weight)
{
    if (weight == d->weight)
        return;
    d->weight = weight;
    d->dirty = true;
    d->placeholderParagraph.reset();
    requestLayout();
    requestPaint();
}

void TextField::setHorizontallyCentered(bool centred)
{
    d->centred = centred;
    d->placeholderParagraph.reset();
    requestPaint();
}

void TextField::setRightAligned(bool rightAligned)
{
    d->rightAligned = rightAligned;
    d->placeholderParagraph.reset();
    requestPaint();
}

double TextField::contentWidth() const
{
    return std::max(0.0, geometry().width - d->left() - d->right());
}

void TextField::setFrameVisible(bool visible)
{
    d->frameVisible = visible;
    requestPaint();
}

void TextField::setFadesWhenDisabled(bool fades)
{
    d->fadesWhenDisabled = fades;
    enabledChanged();
}

void TextField::setTextColor(std::optional<ColorRecipe> color)
{
    d->textColor = std::move(color);
    d->dirty = true;
    requestPaint();
}

void TextField::setPlaceholderColor(std::optional<ColorRecipe> color)
{
    d->placeholderColor = std::move(color);
    d->placeholderParagraph.reset();
    requestPaint();
}

void TextField::setSelectionColors(std::optional<ColorRecipe> background, std::optional<ColorRecipe> text)
{
    d->selectionColor = std::move(background);
    d->selectedTextColor = std::move(text);
    d->dirty = true;
    requestPaint();
}

void TextField::setFontFamily(std::string family)
{
    if (family == d->family)
        return;
    d->family = std::move(family);
    d->dirty = true;
    d->placeholderParagraph.reset();
    requestLayout();
    requestPaint();
}

void TextField::setPasswordCharacter(std::string character)
{
    d->mask = character.empty() ? std::string(kPasswordDot) : std::move(character);
    d->dirty = true;
    requestPaint();
}

void TextField::setCaretWidth(double width)
{
    d->caretWidth = width;
    requestPaint();
}

void TextField::setCursorFlashTime(int milliseconds)
{
    d->cursorFlashTime = milliseconds > 0 ? milliseconds : kCursorFlashTime;
}

void TextField::setSelectByMouse(bool select)
{
    d->selectByMouse = select;
}

void TextField::setClipboardEnabled(bool enabled)
{
    d->clipboardEnabled = enabled;
}

void TextField::setContextMenuEnabled(bool enabled)
{
    d->contextMenuEnabled = enabled;
}

void TextField::clear()
{
    setText({});
}

void TextField::setPlaceholder(std::string placeholder)
{
    if (placeholder == d->placeholder)
        return;
    d->placeholder = std::move(placeholder);
    d->placeholderParagraph.reset();
    requestLayout();
}

void TextField::setEchoMode(EchoMode mode)
{
    d->echo = mode;
    d->dirty = true;
    updateTextInput();
    requestPaint();
}

void TextField::setInputPurpose(TextPurpose purpose)
{
    d->purpose = purpose;
    updateTextInput();
}

void TextField::setInputHints(uint32_t hints)
{
    d->hints = hints;
    updateTextInput();
}

TextField::EchoMode TextField::echoMode() const
{
    return d->echo;
}

void TextField::setReadOnly(bool readOnly)
{
    d->readOnly = readOnly;
    updateTextInput();
}

bool TextField::isReadOnly() const
{
    return d->readOnly;
}

size_t TextField::cursorPosition() const
{
    return d->edit.cursor();
}

void TextField::setCursorPosition(size_t position)
{
    d->edit.setCursor(position);
    moved();
}

void TextField::select(size_t anchor, size_t cursor)
{
    d->edit.select(anchor, cursor);
    moved();
}

void TextField::selectAll()
{
    d->edit.selectAll();
    moved();
}

bool TextField::hasSelection() const
{
    return d->edit.hasSelection();
}

std::string TextField::selectedText() const
{
    return d->edit.selectedText();
}

const std::string& TextField::preedit() const
{
    return d->preedit;
}

void TextField::cut()
{
    if (!d->clipboardEnabled || d->readOnly || d->echo == EchoMode::Password || !d->edit.hasSelection())
        return;
    copy();
    d->edit.deleteBackward(false);
    edited();
}

void TextField::copy()
{
    if (!d->clipboardEnabled || d->echo == EchoMode::Password || !d->edit.hasSelection())
        return;
    Clipboard::setText(d->edit.selectedText());
}

void TextField::paste()
{
    if (!d->clipboardEnabled || d->readOnly)
        return;
    Clipboard::requestText([self = std::weak_ptr<TextField*>(d->self)](std::string text) {
        const auto field = self.lock();
        if (!field || text.empty())
            return;
        // One line: line breaks become spaces.
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::erase(text, '\r');
        (*field)->d->edit.insert(text, false, (*field)->d->maximumLength);
        (*field)->edited();
    });
}

void TextField::undo()
{
    // Earlier passwords stay out of reach of undo.
    if (d->readOnly || d->echo == EchoMode::Password || !d->edit.canUndo())
        return;
    d->edit.undo();
    edited();
}

void TextField::redo()
{
    if (d->readOnly || d->echo == EchoMode::Password || !d->edit.canRedo())
        return;
    d->edit.redo();
    edited();
}

RectF TextField::visualRect() const
{
    return localRect().adjusted(-kFocusRing, -kFocusRing, kFocusRing, kFocusRing);
}

SizeF TextField::sizeHint() const
{
    double width = kMinimumWidth;
    if (!d->placeholder.empty()) {
        textlayout::Paragraph* placeholder = d->placeholderLayout();
        placeholder->layout(SkScalar(kUnboundedWidth));
        width = std::max(width, double(placeholder->getMaxIntrinsicWidth()) + d->left() + d->right());
    }
    return {width, kHeight};
}

void TextField::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    const double scale = painter.scale();
    const RectF bounds = localRect();
    const bool focused = hasFocus();

    // The background, its focus ring concentric through the inset.
    if (d->frameVisible) {
        painter.drawRoundedRect(bounds, {theme.smallRadius, 0, themeCorners()}, theme.alternateBackground, 1,
                                focused ? theme.highlight : theme.alternateBackground);
    }
    if (focused && d->frameVisible) {
        painter.drawRoundedRect(bounds.adjusted(-kFocusRing, -kFocusRing, kFocusRing, kFocusRing),
                                {theme.smallRadius, -kFocusRing, themeCorners()}, Color{0, 0, 0, 0}, 2,
                                color(ColorId::TextFieldFocusRing));
    }

    const RectF content = bounds.adjusted(d->left(), 0, -d->right(), 0);
    textlayout::Paragraph* text = d->layout(isEnabled());
    const double width = content.width;

    // Keep the caret in sight, and no empty space past the end of the text.
    const double caretPixels = d->caretWidth > 0 ? std::max(1.0, std::round(d->caretWidth * scale))
                                                 : std::max(1.0, std::round(scale));
    const double caretWidth = caretPixels / scale;
    const double caret = d->caretX(d->caretDisplayOffset());
    const double textWidth = text->getMaxIntrinsicWidth();
    if (caret - d->scrollX > width - caretWidth)
        d->scrollX = caret - width + caretWidth;
    if (caret - d->scrollX < 0)
        d->scrollX = caret;
    d->scrollX = std::max(0.0, std::min(d->scrollX, textWidth + caretWidth - width));

    // One line box, centred in the field, unsnapped.
    const TextContext& context = Application::instance()->text();
    const double pixelSize = d->pixelSize();
    const bool hinted = textLayoutMode().hinted;
    const TextLineBox line =
        textLineBoxes(text, context.typeface(d->weight, d->family).get(), pixelSize, scale, hinted).front();
    const double originX = content.x + d->alignX(width) - d->scrollX;
    const double lineTop = (bounds.height - line.height) / 2;
    const PointF offset = painter.offset();
    const auto device = [scale, offset](double x, double y) {
        return SkPoint::Make(SkScalar(std::round((offset.x + x) * scale)), SkScalar(std::round((offset.y + y) * scale)));
    };
    SkCanvas* canvas = painter.canvas();

    painter.save();
    painter.clip(content);
    // The selection, the glyphs and the caret draw on the canvas itself; a
    // faded field fades them through a layer.
    const bool faded = painter.opacity() < 1;
    if (faded) {
        const Rect area = painter.deviceRect(content);
        const SkRect rect = SkRect::MakeXYWH(SkScalar(area.x), SkScalar(area.y), SkScalar(area.width),
                                             SkScalar(area.height));
        canvas->saveLayerAlphaf(&rect, float(painter.opacity()));
    }
    // A selection shows with or without the focus.
    if (d->preedit.empty() && d->edit.hasSelection()) {
        SkPaint selection;
        selection.setColor((d->selectionColor ? d->selectionColor->resolve() : theme.highlight).argb());
        const size_t start = utf16Index(d->display, d->toDisplay(d->edit.selectionStart()));
        const size_t end = utf16Index(d->display, d->toDisplay(d->edit.selectionEnd()));
        for (const textlayout::TextBox& box : text->getRectsForRange(
                 unsigned(start), unsigned(end), textlayout::RectHeightStyle::kMax,
                 textlayout::RectWidthStyle::kTight)) {
            const SkPoint topLeft = device(originX + box.rect.left(), lineTop);
            const SkPoint bottomRight = device(originX + box.rect.right(), lineTop + line.height);
            canvas->drawRect(SkRect::MakeLTRB(topLeft.fX, topLeft.fY, bottomRight.fX, bottomRight.fY), selection);
        }
    }

    // The paragraphs draw in logical pixels from the field's origin; their
    // first baseline goes on the line box's.
    canvas->save();
    canvas->translate(SkScalar(offset.x * scale), SkScalar(offset.y * scale));
    canvas->scale(SkScalar(scale), SkScalar(scale));
    if (d->display.empty() && !d->placeholder.empty()) {
        textlayout::Paragraph* placeholder = d->placeholderLayout();
        placeholder->layout(SkScalar(width));
        paintParagraph(canvas, placeholder, content.x, lineTop + line.ascent - placeholder->getAlphabeticBaseline());
    }
    paintParagraph(canvas, text, originX, lineTop + line.ascent - text->getAlphabeticBaseline());
    canvas->restore();

    const Window* window = this->window();
    if (focused && window && window->hasKeyboardFocus() && !d->blinkTimer)
        startBlinking();
    if (focused && window && window->hasKeyboardFocus() && d->caretVisible) {
        SkPaint paint;
        paint.setColor((d->textColor ? d->textColor->resolve() : theme.text).argb());
        const SkPoint top = device(originX + caret, lineTop);
        const SkPoint bottom = device(originX + caret, lineTop + line.height);
        canvas->drawRect(SkRect::MakeXYWH(top.fX, top.fY, SkScalar(caretPixels),
                                          bottom.fY - top.fY),
                         paint);
    }
    if (faded)
        canvas->restore();
    painter.restore();
}

void TextField::enabledChanged()
{
    // Disabled: half opaque, its text in the disabled colour.
    setOpacity(isEnabled() || !d->fadesWhenDisabled ? 1 : 0.5);
}

void TextField::pointerPress(const PointerEvent& event)
{
    if (event.button == MouseButton::Right) {
        if (d->contextMenuEnabled)
            showContextMenu(event.position);
        return;
    }
    if (event.button != MouseButton::Left)
        return;
    // Text inside the padding, as in paint.
    const double x = event.position.x - d->left() - d->alignX(contentWidth()) + d->scrollX;
    if (!d->preedit.empty()) {
        d->preedit.clear();
        d->dirty = true;
    }
    const size_t offset = d->fromDisplay(d->displayOffsetAt(x));
    if (!d->selectByMouse) {
        d->edit.setCursor(offset);
        moved();
        return;
    }
    d->selecting = true;
    // An obscured text is one word.
    d->granularity = std::clamp(event.clickCount, 1, 3);
    if (d->echo == EchoMode::Password && d->granularity == 2)
        d->granularity = 3;
    if (d->granularity == 3) {
        d->edit.selectAll();
    } else if (d->granularity == 2) {
        d->anchorWord = d->edit.wordAt(offset);
        d->edit.select(d->anchorWord.first, d->anchorWord.second);
    } else {
        d->edit.setCursor(offset, event.modifiers & ShiftModifier);
    }
    moved();
}

void TextField::pointerMove(const PointerEvent& event)
{
    if (!d->selecting || d->granularity == 3)
        return;
    const double x = event.position.x - d->left() - d->alignX(contentWidth()) + d->scrollX;
    const size_t offset = d->fromDisplay(d->displayOffsetAt(x));
    if (d->granularity == 2) {
        const auto word = d->edit.wordAt(offset);
        if (offset < d->anchorWord.first)
            d->edit.select(d->anchorWord.second, word.first);
        else
            d->edit.select(d->anchorWord.first, word.second);
    } else {
        d->edit.setCursor(offset, true);
    }
    moved();
}

void TextField::pointerRelease(const PointerEvent& event)
{
    const bool selected = d->selecting;
    d->selecting = false;
    if (!d->clipboardEnabled)
        return;
    // A mouse selection goes to the primary selection, and a
    // middle click pastes it at the cursor.
    if (event.button == MouseButton::Left && selected) {
        if (d->echo != EchoMode::Password && d->edit.hasSelection())
            Clipboard::setText(d->edit.selectedText(), ClipboardMode::Selection);
    } else if (event.button == MouseButton::Middle && !d->readOnly) {
        Clipboard::requestText([self = std::weak_ptr<TextField*>(d->self)](std::string text) {
            const auto field = self.lock();
            if (!field || text.empty())
                return;
            std::replace(text.begin(), text.end(), '\n', ' ');
            std::erase(text, '\r');
            (*field)->d->edit.setCursor((*field)->d->edit.cursor());
            (*field)->d->edit.insert(text, false, (*field)->d->maximumLength);
            (*field)->edited();
        }, ClipboardMode::Selection);
    }
}

void TextField::focusIn(FocusReason)
{
    moved();
}

void TextField::focusOut()
{
    if (onEditingFinished)
        onEditingFinished();
    if (d->blinkTimer) {
        Application::instance()->loop().stopTimer(d->blinkTimer);
        d->blinkTimer = 0;
    }
    d->edit.setCursor(d->edit.cursor());
    d->preedit.clear();
    d->dirty = true;
    requestPaint();
}

bool TextField::keyPress(const KeyEvent& event)
{
    const uint32_t modifiers = event.modifiers;
    const bool shift = modifiers & ShiftModifier;
    const bool control = modifiers & ControlModifier;
    const bool command = modifiers & (ControlModifier | AltModifier | SuperModifier);
    EditableText& edit = d->edit;
    // Word moves and deletes take a password whole, as Chromium's obscured
    // text does, so they tell nothing of its words.
    const bool obscured = d->echo == EchoMode::Password;

    const auto move = [this, &edit, shift](size_t position) {
        edit.setCursor(position, shift);
        moved();
        return true;
    };

    if (control && !(modifiers & (AltModifier | SuperModifier))) {
        switch (event.key) {
        case key::letter('a'):
            edit.selectAll();
            moved();
            return true;
        case key::letter('c'):
            copy();
            return true;
        case key::letter('x'):
            cut();
            return true;
        case key::letter('v'):
            paste();
            return true;
        case key::letter('z'):
            shift ? redo() : undo();
            return true;
        case key::letter('y'):
            redo();
            return true;
        default:
            break;
        }
    }

    switch (event.key) {
    case key::Left:
        if (!shift && edit.hasSelection() && !control)
            return move(edit.selectionStart());
        if (control && obscured)
            return move(0);
        return move(control ? edit.previousWordStart(edit.cursor())
                            : edit.previousGrapheme(edit.cursor()));
    case key::Right:
        if (!shift && edit.hasSelection() && !control)
            return move(edit.selectionEnd());
        if (control && obscured)
            return move(edit.text().size());
        return move(control ? edit.nextWordEnd(edit.cursor()) : edit.nextGrapheme(edit.cursor()));
    case key::Home:
        return move(0);
    case key::End:
        return move(edit.text().size());
    case key::BackSpace:
        if (d->readOnly)
            return true;
        if (control && obscured && !edit.hasSelection())
            edit.erase(0, edit.cursor());
        else
            edit.deleteBackward(control);
        edited();
        return true;
    case key::Delete:
        if (d->readOnly)
            return true;
        if (control && obscured && !edit.hasSelection())
            edit.erase(edit.cursor(), edit.text().size());
        else
            edit.deleteForward(control);
        edited();
        return true;
    case key::Return:
    case key::KeypadEnter: {
        const std::weak_ptr<TextField*> self = d->self;
        if (onAccepted)
            onAccepted();
        // A dialog may close, and destroy the field, on Return.
        if (self.expired())
            return true;
        if (onEditingFinished)
            onEditingFinished();
        return true;
    }
    default:
        break;
    }

    if (!event.text.empty() && !command) {
        if (!d->readOnly) {
            edit.insert(event.text, true, d->maximumLength);
            edited();
        }
        return true;
    }
    return false;
}

bool TextField::acceptsTextInput() const
{
    return !d->readOnly && d->echo == EchoMode::Normal && isEnabled();
}

TextInputState TextField::textInputState() const
{
    TextInputState state;
    const std::string& text = d->edit.text();
    // A window of the text around the cursor, cut on character boundaries.
    size_t start = d->edit.selectionStart() > kSurroundingLimit / 2
            ? d->edit.selectionStart() - kSurroundingLimit / 2
            : 0;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
        --start;
    size_t end = std::min(text.size(), start + kSurroundingLimit);
    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
    state.surroundingText = text.substr(start, end - start);
    state.cursor = d->edit.cursor() - start;
    state.anchor = d->edit.anchor() - start;

    const TextLayoutMode mode = textLayoutMode();
    const double scale = mode.scale;
    const double padding = d->left();
    // The state follows an edit before it is painted; lay the new text out first.
    textlayout::Paragraph* paragraph = d->layout(isEnabled());
    const double x = padding + d->alignX(contentWidth()) + d->caretX(d->caretDisplayOffset()) - d->scrollX;
    const double height = textLineBoxes(paragraph, Application::instance()->text().typeface(d->weight, d->family).get(),
                                        d->pixelSize(), scale, mode.hinted)
                              .front()
                              .height;
    const PointF topLeft = mapToWindow({x, (geometry().height - height) / 2});
    state.cursorRectangle = {topLeft.x, topLeft.y, 1, height};
    state.purpose = d->purpose;
    state.hints = d->hints;
    return state;
}

void TextField::textInput(const TextInputEvent& event)
{
    EditableText& edit = d->edit;
    d->preedit.clear();
    if (event.deleteBefore || event.deleteAfter) {
        const size_t cursor = edit.cursor();
        const size_t start = cursor > event.deleteBefore ? cursor - event.deleteBefore : 0;
        edit.erase(start, cursor + event.deleteAfter);
    }
    if (!event.commit.empty())
        edit.insert(event.commit, true, d->maximumLength);
    d->preedit = event.preedit;
    d->preeditCursor = event.preeditCursorBegin;
    edited();
}

void TextField::showContextMenu(PointF position)
{
    // The text menu, without the rows a read-only field has no use for.
    const bool editable = !d->readOnly;
    const bool clipboard = d->echo == EchoMode::Normal;
    const bool selection = d->edit.hasSelection();
    d->menu = std::make_unique<Menu>();
    Menu& menu = *d->menu;
    const auto addEditing = [&menu, editable](std::string text, std::function<void()> action,
                                              bool enabled) {
        const size_t index = menu.addItem(std::move(text), std::move(action));
        menu.setItemEnabled(index, enabled);
        menu.setItemVisible(index, editable);
    };
    addEditing(tr("Undo"), [this] { undo(); }, d->edit.canUndo());
    addEditing(tr("Redo"), [this] { redo(); }, d->edit.canRedo());
    if (editable)
        menu.addSeparator();
    addEditing(tr("Cut"), [this] { cut(); }, selection && clipboard);
    const size_t copyIndex = menu.addItem(tr("Copy"), [this] { copy(); });
    menu.setItemEnabled(copyIndex, selection && clipboard);
    addEditing(tr("Paste"), [this] { paste(); }, true);
    addEditing(tr("Delete"), [this] {
        d->edit.deleteBackward(false);
        edited();
    }, selection);
    menu.addSeparator();
    const size_t selectAllIndex = menu.addItem(tr("Select All"), [this] { selectAll(); });
    menu.setItemEnabled(selectAllIndex, !d->edit.text().empty());
    menu.popup(window(), mapToWindow(position));
}

bool TextField::edited()
{
    const bool changed = d->edit.revision() != d->savedRevision;
    if (changed && d->validator && !d->validator(d->edit.text())) {
        d->edit = d->savedEdit;
        moved();
        return false;
    }
    moved();
    // A key that deletes nothing, or the input method's composing alone.
    if (!changed)
        return true;
    textChanged();
    if (onTextChanged)
        onTextChanged();
    if (onTextEdited)
        onTextEdited();
    return true;
}

void TextField::moved()
{
    d->savedRevision = d->edit.revision();
    if (d->validator)
        d->savedEdit = d->edit;
    d->dirty = true;
    d->caretVisible = true;
    auto& loop = Application::instance()->loop();
    if (d->blinkTimer)
        loop.stopTimer(d->blinkTimer);
    d->blinkTimer = 0;
    if (hasFocus())
        startBlinking();
    requestPaint();
    updateTextInput();
}

void TextField::startBlinking()
{
    const auto interval = std::chrono::microseconds(int64_t(d->cursorFlashTime) * 1000 / 2 * 95 / 100);
    d->blinkTimer = Application::instance()->loop().startTimer(interval, true, [this] {
        // Without the keyboard no caret is drawn; the next paint with it
        // starts blinking again.
        const Window* window = this->window();
        if (!window || !window->hasKeyboardFocus()) {
            Application::instance()->loop().stopTimer(d->blinkTimer);
            d->blinkTimer = 0;
            d->caretVisible = true;
            return;
        }
        d->caretVisible = !d->caretVisible;
        requestPaint();
    });
}

} // namespace cutefish::ui
