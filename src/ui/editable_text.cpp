// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/editable_text.h"

#include "modules/skunicode/include/SkUnicode.h"

#include "ui/application.h"
#include "ui/text_context.h"

namespace cutefish::ui {

static constexpr size_t kHistoryLimit = 100;

// The code point starting at the position; -1 for a malformed sequence.
static int32_t codePointAt(const std::string& text, size_t position)
{
    const auto byte = [&text](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(position);
    int length = 0;
    int32_t value = 0;
    if (lead < 0x80)
        return lead;
    if ((lead & 0xe0) == 0xc0) {
        length = 2;
        value = lead & 0x1f;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
        value = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
        value = lead & 0x07;
    } else {
        return -1;
    }
    if (position + length > text.size())
        return -1;
    for (int i = 1; i < length; ++i) {
        if ((byte(position + i) & 0xc0) != 0x80)
            return -1;
        value = (value << 6) | (byte(position + i) & 0x3f);
    }
    return value;
}

static size_t utf16Length(std::string_view text)
{
    size_t units = 0;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xc0) != 0x80)
            units += byte >= 0xf0 ? 2 : 1;
    }
    return units;
}

EditableText::EditableText()
{
    updateBoundaries();
    m_history.push_back({m_text, 0, 0});
}

void EditableText::setText(std::string text)
{
    m_text = std::move(text);
    ++m_revision;
    m_cursor = m_anchor = m_text.size();
    updateBoundaries();
    m_history.assign(1, {m_text, m_cursor, m_anchor});
    m_undoIndex = 0;
    m_lastEdit = Edit::None;
}

std::string EditableText::selectedText() const
{
    return m_text.substr(selectionStart(), selectionEnd() - selectionStart());
}

void EditableText::setCursor(size_t position, bool keepAnchor)
{
    m_cursor = snapped(position);
    if (!keepAnchor)
        m_anchor = m_cursor;
    m_lastEdit = Edit::None;
}

void EditableText::select(size_t anchor, size_t cursor)
{
    m_anchor = snapped(anchor);
    m_cursor = snapped(cursor);
    m_lastEdit = Edit::None;
}

void EditableText::selectAll()
{
    select(0, m_text.size());
}

void EditableText::insert(std::string_view text, bool typing, size_t maximumLength)
{
    replace(selectionStart(), selectionEnd(), text, typing ? Edit::Typing : Edit::Other, maximumLength);
}

void EditableText::erase(size_t start, size_t end)
{
    replace(snapped(start), snapped(end), {}, Edit::Other);
}

void EditableText::deleteBackward(bool word)
{
    if (hasSelection()) {
        replace(selectionStart(), selectionEnd(), {}, Edit::Other);
        return;
    }
    const size_t start = word ? previousWordStart(m_cursor) : previousGrapheme(m_cursor);
    if (start != m_cursor)
        replace(start, m_cursor, {}, Edit::Other);
}

void EditableText::deleteForward(bool word)
{
    if (hasSelection()) {
        replace(selectionStart(), selectionEnd(), {}, Edit::Other);
        return;
    }
    const size_t end = word ? nextWordEnd(m_cursor) : nextGrapheme(m_cursor);
    if (end != m_cursor)
        replace(m_cursor, end, {}, Edit::Other);
}

size_t EditableText::snapped(size_t position) const
{
    position = std::min(position, m_text.size());
    while (position > 0 && !m_graphemeStarts[position])
        --position;
    return position;
}

size_t EditableText::previousGrapheme(size_t position) const
{
    if (position == 0)
        return 0;
    --position;
    while (position > 0 && !m_graphemeStarts[position])
        --position;
    return position;
}

size_t EditableText::nextGrapheme(size_t position) const
{
    if (position >= m_text.size())
        return m_text.size();
    ++position;
    while (position < m_text.size() && !m_graphemeStarts[position])
        ++position;
    return position;
}

size_t EditableText::previousWordStart(size_t position) const
{
    while (position > 0 && isSpaceAt(previousGrapheme(position)))
        position = previousGrapheme(position);
    while (position > 0) {
        position = previousGrapheme(position);
        if (m_wordBoundaries[position])
            break;
    }
    return position;
}

size_t EditableText::nextWordEnd(size_t position) const
{
    while (position < m_text.size() && isSpaceAt(position))
        position = nextGrapheme(position);
    while (position < m_text.size()) {
        position = nextGrapheme(position);
        if (m_wordBoundaries[position])
            break;
    }
    return position;
}

std::pair<size_t, size_t> EditableText::wordAt(size_t position) const
{
    position = snapped(position);
    // At the very end, the word before.
    if (position == m_text.size() && position > 0)
        position = previousGrapheme(position);
    size_t start = position;
    while (start > 0 && !m_wordBoundaries[start])
        start = previousGrapheme(start);
    size_t end = nextGrapheme(position);
    while (end < m_text.size() && !m_wordBoundaries[end])
        end = nextGrapheme(end);
    return {start, end};
}

bool EditableText::isSpaceAt(size_t position) const
{
    if (position >= m_text.size())
        return false;
    const int32_t character = codePointAt(m_text, position);
    return character >= 0 && Application::instance()->text().unicode()->isWhitespace(character);
}

void EditableText::replace(size_t start, size_t end, std::string_view text, Edit edit, size_t maximumLength)
{
    if (maximumLength)
        text = text.substr(0, fittingLength(text, start, end, maximumLength));
    if (start == end && text.empty())
        return;
    m_text.replace(start, end - start, text);
    ++m_revision;
    m_cursor = m_anchor = start + text.size();
    updateBoundaries();
    record(edit);
}

size_t EditableText::fittingLength(std::string_view text, size_t start, size_t end, size_t maximumLength) const
{
    const size_t kept = utf16Length(m_text) - utf16Length(std::string_view(m_text).substr(start, end - start));
    const size_t room = kept < maximumLength ? maximumLength - kept : 0;
    size_t units = 0;
    size_t fits = 0;
    while (fits < text.size()) {
        const auto lead = static_cast<unsigned char>(text[fits]);
        const size_t length = lead >= 0xf0 ? 2 : 1;
        if (units + length > room)
            break;
        units += length;
        ++fits;
        while (fits < text.size() && (static_cast<unsigned char>(text[fits]) & 0xc0) == 0x80)
            ++fits;
    }
    if (fits == text.size() || fits == 0)
        return fits;

    // Back to the start of the grapheme the cut went through.
    std::string copy(text);
    skia_private::TArray<SkUnicode::CodeUnitFlags, true> flags;
    if (!Application::instance()->text().unicode()->computeCodeUnitFlags(copy.data(), int(copy.size()), false,
                                                                       &flags))
        return fits;
    while (fits > 0 && !(flags[int(fits)] & SkUnicode::kGraphemeStart))
        --fits;
    return fits;
}

void EditableText::updateBoundaries()
{
    const size_t size = m_text.size();
    m_graphemeStarts.assign(size + 1, false);
    m_wordBoundaries.assign(size + 1, false);
    m_graphemeStarts[0] = m_graphemeStarts[size] = true;
    m_wordBoundaries[0] = m_wordBoundaries[size] = true;
    if (size == 0)
        return;

    const auto& unicode = Application::instance()->text().unicode();
    // The flags call writes into the text when asked to replace tabs; it is not.
    std::string copy = m_text;
    skia_private::TArray<SkUnicode::CodeUnitFlags, true> flags;
    if (unicode->computeCodeUnitFlags(copy.data(), int(size), false, &flags)) {
        for (size_t i = 0; i < size; ++i)
            m_graphemeStarts[i] = m_graphemeStarts[i] || (flags[int(i)] & SkUnicode::kGraphemeStart);
    }
    std::vector<SkUnicode::Position> words;
    if (unicode->getUtf8Words(m_text.data(), int(size), nullptr, &words)) {
        for (SkUnicode::Position position : words) {
            if (position <= size)
                m_wordBoundaries[position] = true;
        }
    }
}

void EditableText::record(Edit edit)
{
    m_history.resize(m_undoIndex + 1);
    const State state{m_text, m_cursor, m_anchor};
    if (edit == Edit::Typing && m_lastEdit == Edit::Typing && m_undoIndex > 0) {
        m_history[m_undoIndex] = state;
    } else {
        m_history.push_back(state);
        ++m_undoIndex;
        if (m_history.size() > kHistoryLimit) {
            m_history.erase(m_history.begin());
            --m_undoIndex;
        }
    }
    m_lastEdit = edit;
}

void EditableText::undo()
{
    if (!canUndo())
        return;
    // Before the edit undone; its cursor is where that edit left it.
    const State& state = m_history[--m_undoIndex];
    m_text = state.text;
    ++m_revision;
    m_cursor = state.cursor;
    m_anchor = state.anchor;
    updateBoundaries();
    m_lastEdit = Edit::None;
}

void EditableText::redo()
{
    if (!canRedo())
        return;
    const State& state = m_history[++m_undoIndex];
    m_text = state.text;
    ++m_revision;
    m_cursor = state.cursor;
    m_anchor = state.anchor;
    updateBoundaries();
    m_lastEdit = Edit::None;
}

} // namespace cutefish::ui
