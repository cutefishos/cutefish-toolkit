// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ui/geometry.h"

namespace cutefish::ui {

class Painter;
struct GlyphRendererPrivate;

// A font: a family and a size in points (at 96 logical DPI), with kerning
// off.
struct FontSpec
{
    std::string family;
    double pointSize = 10;
    bool bold = false;
    bool italic = false;

    bool operator==(const FontSpec&) const = default;
};

// The font's metrics in a window of the scale, in logical pixels: FreeType at
// the whole pixel size of the point size, with the line metrics of the hhea
// table overridden by OS/2, rounded to whole pixels only where metrics are
// hinted (scale 1 with full or medium hinting and a hinted text layout).
class FontMetrics
{
public:
    explicit FontMetrics(const FontSpec& font, double scale = 1, bool hintedLayout = true);

    // The pixel size: whole pixels.
    double pixelSize() const { return m_pixelSize; }

    double ascentF() const { return m_ascent; }
    double descentF() const { return m_descent; }
    double leadingF() const { return m_leading; }
    double heightF() const { return m_ascent + m_descent; }
    double lineSpacingF() const { return m_leading + m_ascent + m_descent; }
    double underlinePosF() const { return m_underlinePosition; }
    double lineWidthF() const { return m_lineThickness; }
    double strikeOutPosF() const;
    double overlinePosF() const { return m_ascent + 1; }
    double horizontalAdvanceF(std::string_view text) const;

    int ascent() const;
    int descent() const;
    int leading() const;
    // ascent() + descent(), each rounded first.
    int height() const;
    int lineSpacing() const;
    int underlinePos() const;
    int strikeOutPos() const;
    int overlinePos() const { return ascent() + 1; }
    int lineWidth() const;
    int horizontalAdvance(std::string_view text) const;

    // The family matched and whether its glyphs share one width.
    const std::string& family() const { return m_family; }
    bool fixedPitch() const { return m_fixedPitch; }

private:
    double m_pixelSize = 0;
    double m_ascent = 0;
    double m_descent = 0;
    double m_leading = 0;
    double m_underlinePosition = 0;
    double m_lineThickness = 1;
    bool m_roundAdvances = false;
    std::string m_family;
    bool m_fixedPitch = false;
    std::shared_ptr<GlyphRendererPrivate> m_font;
};

// One cluster of a cell grid: its text and the device x of its origin.
struct GlyphCluster
{
    std::u32string_view text;
    double x = 0;
};

// Draws clusters of a font at given device positions, as a terminal's glyph
// atlas does: each cluster shaped on its own (HarfBuzz, left to right,
// kerning off) with fontconfig's fallback for what the family lacks, its
// origin and baseline on whole device pixels, greyscale antialiased and
// hinted as labels are. Colour glyphs, such as emoji, keep their colours.
// Bold and italic take the family's faces, else synthetic ones, as the face's
// own file says it is: an oblique shear, and emboldening under 64 px.
// Shaped clusters are cached for the renderer's life, so a frame only
// places glyphs.
class GlyphRenderer
{
public:
    GlyphRenderer(const FontSpec& font, double scale);
    ~GlyphRenderer();

    GlyphRenderer(const GlyphRenderer&) = delete;
    GlyphRenderer& operator=(const GlyphRenderer&) = delete;

    const FontSpec& font() const;
    double scale() const;

    // Every cluster in the colour, on the baseline (device pixels from the
    // painter's origin, as are the x positions). `stretch` scales what is
    // drawn about `stretchOrigin` (device pixels), as for double-width and
    // double-height lines.
    void draw(Painter& painter, Color color, double baseline, std::span<const GlyphCluster> clusters,
              SizeF stretch = {1, 1}, PointF stretchOrigin = {});

    // The natural advance of a cluster in device pixels, for wide glyphs.
    double advance(std::u32string_view cluster);
    size_t cachedClusters() const;
    void clearCache();

private:
    std::shared_ptr<GlyphRendererPrivate> d;
};

// A rect of whole device pixels from the painter's origin (its offset
// rounded to device pixels), filled without antialiasing. Cheap enough for
// thousands a frame: the GPU backend batches them.
void fillDeviceRect(Painter& painter, const Rect& rect, Color color);

struct FontFamily
{
    std::string name;
    bool fixedPitch = false;
};

// The installed families, sorted without regard to case, each with whether
// it is fixed pitch.
std::vector<FontFamily> fontFamilies();

} // namespace cutefish::ui
