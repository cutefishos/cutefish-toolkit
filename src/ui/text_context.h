// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include "include/core/SkFont.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurfaceProps.h"
#include "modules/skparagraph/include/TextStyle.h"

#include "ui/geometry.h"

class SkFontMgr;
class SkTypeface;
class SkUnicode;

namespace skia::textlayout {
class FontCollection;
}

namespace cutefish::ui {

// A font's vertical metrics, in logical pixels.
struct VerticalMetrics
{
    double ascent = 0;
    double descent = 0;
    double xHeight = 0;
    double capHeight = 0;
};

// Fonts and shaping shared by every window: the system font manager with
// fallback, and the rendering fontconfig asks for.
//
// Text lays out at a whole logical pixel size, with unhinted advances,
// lines as tall as their ascent and descent rounded up. It is drawn at the
// device size.
class TextContext
{
public:
    // For text in the LC_CTYPE locale named.
    explicit TextContext(const std::string& ctypeLocale);
    ~TextContext();

    TextContext(const TextContext&) = delete;
    TextContext& operator=(const TextContext&) = delete;

    const sk_sp<skia::textlayout::FontCollection>& fontCollection() const { return m_fontCollection; }
    const sk_sp<SkUnicode>& unicode() const { return m_unicode; }
    // Forgets fallback fonts and matched families, after fonts or the
    // appearance's font changed.
    void clearFontCaches();

    const std::string& family() const { return m_family; }
    // BCP 47, for picking the Han variant among fallback fonts.
    const std::string& locale() const { return m_locale; }
    // After the session language changed; forgets the font caches.
    void setLocale(const std::string& ctypeLocale);

    // The pixel size for a logical size: rounded to two decimals, then to
    // whole pixels.
    static double pixelSize(double logicalSize);

    // The theme's family at a logical size (0 for the theme's), rounded as
    // pixelSize() rounds it, in logical units.
    // A family given takes the theme's place, the theme's standing in when the
    // family is missing.
    skia::textlayout::TextStyle style(double fontSize, int weight, Color color,
                                      const std::string& family = {}) const;
    sk_sp<SkTypeface> typeface(int weight, const std::string& family = {}) const;
    // A face of a font file, installed or not: the face in the index's low
    // 16 bits, a variable font's named instance above them.
    sk_sp<SkTypeface> typefaceFromFile(const std::string& path, int index) const;

    // The typeface's height metrics: from OS/2 or hhea, in 26.6 fixed point.
    // Ascent and descent are rounded to whole pixels when metrics are hinted,
    // only without scaling and with full or medium hinting.
    // `hintedLayout` is the window's (Window::hintedTextLayout()): off, text
    // lays out unrounded at every scale, scale 1 included, as for the
    // compositor's lock screen and switcher.
    VerticalMetrics metrics(const SkTypeface* typeface, double pixelSize, double scale, bool hintedLayout) const;
    bool roundsMetrics(double scale, bool hintedLayout) const;
    // At a fractional device scale glyphs draw unhinted at quarter-pixel
    // horizontal positions, baselines on whole rows; at whole scales as
    // fontconfig asks. Advances and metrics are not affected.
    static bool drawsUnhinted(double deviceScale) { return std::abs(deviceScale - std::round(deviceScale)) > 1e-3; }
    // Whether glyph bounds and advances for ink bounds are hinted, as the
    // metrics are.
    bool hintsGlyphMetrics(double scale, bool hintedLayout) const { return roundsMetrics(scale, hintedLayout); }

    // Surfaces blend glyph coverage linearly, without Skia's contrast and
    // gamma, which embolden light text on dark backgrounds. Without a pixel
    // geometry text is greyscale.
    SkSurfaceProps surfaceProps(bool subpixel = true) const
    {
        return SkSurfaceProps(0, subpixel ? m_pixelGeometry : kUnknown_SkPixelGeometry, 0, 1);
    }

    SkFont::Edging edging() const { return m_edging; }
    SkFontHinting hinting() const { return m_hinting; }
    // What fontconfig asks for, which glyphs take at whole scales.
    SkFontHinting fontconfigHinting() const { return m_fontconfigHinting; }
    bool subpixelPositioning() const { return m_subpixelPositioning; }
    SkPixelGeometry pixelGeometry() const { return m_pixelGeometry; }
    // Whether subpixel text over translucent targets blends per channel (true)
    // or stays greyscale there; set by the window being painted.
    bool translucentSubpixel() const { return m_translucentSubpixel; }
    void setTranslucentSubpixel(bool subpixel) { m_translucentSubpixel = subpixel; }
    // Whether the window being painted has a transparent background, so its
    // own canvas counts as translucent: Skia's subpixel masks assume an
    // opaque target and would leave every glyph edge opaque there.
    bool transparentWindow() const { return m_transparentWindow; }
    void setTransparentWindow(bool transparent) { m_transparentWindow = transparent; }

private:
    void readFontconfigDefaults();

    sk_sp<SkFontMgr> m_fontManager;
    sk_sp<skia::textlayout::FontCollection> m_fontCollection;
    sk_sp<SkUnicode> m_unicode;
    std::string m_family = "sans-serif";
    std::string m_locale;
    struct Tables
    {
        bool valid = false;
        int unitsPerEm = 1000;
        double ascent = 0;
        double descent = 0;
        int xHeight = 0;
        int capHeight = 0;
        bool hasCapHeight = false;
    };
    const Tables& tables(const SkTypeface* typeface) const;

    mutable std::mutex m_tablesMutex;
    mutable std::map<uint32_t, Tables> m_tables;
    SkFont::Edging m_edging = SkFont::Edging::kAntiAlias;
    // What fontconfig asks for; layout uses unhinted advances regardless.
    SkFontHinting m_fontconfigHinting = SkFontHinting::kSlight;
    SkFontHinting m_hinting = SkFontHinting::kSlight;
    bool m_subpixelPositioning = true;
    bool m_translucentSubpixel = true;
    bool m_transparentWindow = false;
    SkPixelGeometry m_pixelGeometry = kUnknown_SkPixelGeometry;
};

} // namespace cutefish::ui
