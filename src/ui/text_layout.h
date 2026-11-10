// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/geometry.h"
#include "ui/glyph_run.h"

namespace cutefish::ui {

class Painter;
struct TextLayoutPrivate;

// A run of a paragraph drawn differently: colour, weight, slant, underline.
// UTF-8 byte offsets. Where ranges overlap, the later one wins.
struct TextFormatRange
{
    size_t start = 0;
    size_t length = 0;
    std::optional<Color> color;
    bool bold = false;
    bool italic = false;
    bool underline = false;
};

// One paragraph laid out for a code editor: the caller makes the lines, each
// with its own width and position. Offsets are UTF-8 bytes. Trailing spaces
// count in a line's width but never push a word to the next line. Tabs
// advance to the next stop from the line's start.
//
// Advances follow the window's scale: whole pixels where metrics are hinted
// (scale 1 with full or medium hinting), unrounded otherwise. Text is shaped with
// HarfBuzz per run of one font, direction and style, with fontconfig's
// fallback; right-to-left runs are ordered visually per line.
//
// On the application's thread only: the fonts and metrics it caches are
// shared by every layout without a lock.
class TextLayout
{
public:
    enum class Wrap {
        None,
        // At word boundaries, and anywhere within a word too long for a
        // line.
        WordOrAnywhere,
    };

    struct Line
    {
        size_t textStart = 0;
        size_t textLength = 0;
        // Trailing spaces included.
        double naturalTextWidth = 0;
        double width = 0;
        PointF position;
        double ascent = 0;
        double descent = 0;
    };

    TextLayout();
    TextLayout(std::string text, const FontSpec& font, double scale = 1, bool hintedLayout = true);
    ~TextLayout();

    TextLayout(TextLayout&&) noexcept;
    TextLayout& operator=(TextLayout&&) noexcept;

    void setText(std::string text);
    const std::string& text() const;
    // The family and point size.
    void setFont(const FontSpec& font);
    // The window's scale, for advances and drawing.
    void setScale(double scale, bool hintedLayout = true);
    void setWrap(Wrap wrap);
    // Logical pixels between stops; 0 for eight spaces.
    void setTabStopDistance(double distance);
    // Space added after each glyph, in ems of the font.
    void setLetterSpacing(double ems);
    void setFormats(std::vector<TextFormatRange> formats);
    // For text outside every coloured range.
    void setDefaultColor(Color color);

    // Lines are made between these; earlier lines are dropped.
    void beginLayout();
    // The next line, `width` wide, at `position`; -1 when the text is all
    // laid out. An empty text has one empty line.
    int createLine(double width, PointF position);
    void endLayout();

    int lineCount() const;
    const Line& lineAt(int index) const;
    // The line holding the offset; the last line for the end.
    int lineForTextPosition(size_t offset) const;
    // The widest line's natural width.
    double maximumWidth() const;

    // Where the caret goes before the offset, in the layout's coordinates;
    // offsets inside a cluster share its edge.
    double cursorToX(int line, size_t offset) const;
    // The cluster boundary nearest to x on the line; past either end, the
    // line's end or start.
    size_t xToCursor(int line, double x) const;

    // Every line at its position, `origin` added, glyphs on the line's
    // baseline, coloured by the formats.
    void draw(Painter& painter, PointF origin);

private:
    std::unique_ptr<TextLayoutPrivate> d;
};

// The system's fixed-pitch font:
// the appearance's fixed family ("Noto Mono" unless set) at the system
// font's point size.
FontSpec systemFixedFont();

} // namespace cutefish::ui
