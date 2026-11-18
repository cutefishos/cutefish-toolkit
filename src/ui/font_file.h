// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/bitmap.h"
#include "ui/geometry.h"

namespace cutefish::ui {

class Painter;

// One face of a font file: a face of a collection, or a named instance of a
// variable font.
struct FontFace
{
    // What FontSample loads: the face in the low 16 bits, the named instance
    // above them.
    int index = 0;
    // In English where the file names them so.
    std::string family;
    std::string style;
    std::string postScriptName;
    // The head table's revision in 16.16 fixed point.
    int revision = 0;
    // 100 to 900.
    int weight = 400;
    bool italic = false;
    // The characters with a glyph: inclusive ranges, ascending.
    std::vector<std::pair<char32_t, char32_t>> coverage;

    bool covers(char32_t character) const;
    // Every character of the UTF-8 text.
    bool covers(std::string_view text) const;
    // The first `count` characters with a glyph from `first` to `last`, in
    // UTF-8.
    std::string characters(char32_t first, char32_t last, int count) const;
};

// The faces of a TrueType or OpenType font or collection, in the file's
// order; a variable font gives its named instances. Empty for any other
// file. Any thread.
std::vector<FontFace> readFontFaces(const std::string& path);

// The face a viewer shows first: the upright one nearest to the regular
// weight.
int defaultFontFace(const std::vector<FontFace>& faces);

// The installed files that hold the face, by its PostScript name and
// revision. The font directories are read anew, so a file copied or removed
// a moment ago counts. Any thread.
std::vector<std::string> installedFontFiles(const FontFace& face);

// A few characters in one face of a font file, alone on a transparent bitmap
// cut to their ink: each character's own glyph at its advance, unshaped, at
// the size in device pixels. `index` is a FontFace's. Null when the file is
// no font or the face draws nothing for the text. Any thread, with or
// without an Application.
Bitmap renderFontGlyphs(const std::string& path, int index, const std::string& text, double pixelSize, Color color);

// Text set in one face of a font file, in that face's glyphs alone: what the
// face lacks shows as its missing glyph, never in another font.
class FontSample
{
public:
    // `index` is a FontFace's.
    FontSample(const std::string& path, int index);
    ~FontSample();

    FontSample(const FontSample&) = delete;
    FontSample& operator=(const FontSample&) = delete;

    bool isValid() const;

    // One line of the text at the logical pixel size: its advance and the
    // face's line height.
    SizeF measure(const std::string& text, double size) const;
    // The line's top left at the position.
    void draw(Painter& painter, const std::string& text, double size, PointF position, Color color) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace cutefish::ui
