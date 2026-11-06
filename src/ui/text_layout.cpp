// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/text_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skshaper/include/SkShaper.h"
#include "modules/skshaper/include/SkShaper_harfbuzz.h"
#include "modules/skunicode/include/SkUnicode.h"

#include "ui/application.h"
#include "ui/font_fallback.h"
#include "ui/line_breaks.h"
#include "ui/painter.h"
#include "ui/text_context.h"
#include "ui/text_lines.h"
#include "ui/theme.h"

namespace cutefish::ui {

// Characters a font set remembers the fallback of before it starts over.
static constexpr size_t kMaximumFallbacks = 65536;

static double pixelSizeForPoints(double points)
{
    const double size = std::floor(points * 96.0 / 72.0 * 100 + 0.5) / 100;
    return std::max(1.0, std::floor(size + 0.5));
}

namespace {

// The typefaces of one family and style, and the fallbacks found for
// characters it lacks, shared by every layout of that font.
struct FontSet
{
    sk_sp<SkTypeface> primary;
    sk_sp<SkTypeface> emoji;
    std::unordered_map<char32_t, sk_sp<SkTypeface>> fallbacks;
    std::unique_ptr<FontFallback> fallback;
    std::string family;
    SkFontStyle style;
    // Whether the primary typeface draws each ASCII character: 1 yes, 0 no,
    // -1 not asked yet.
    std::array<int8_t, 128> asciiInPrimary;
};

} // namespace

static FontSet& fontSet(const std::string& family, bool bold, bool italic)
{
    static std::map<std::tuple<std::string, bool, bool>, std::unique_ptr<FontSet>> sets;
    const auto key = std::make_tuple(family, bold, italic);
    auto it = sets.find(key);
    if (it != sets.end())
        return *it->second;
    auto set = std::make_unique<FontSet>();
    set->family = family;
    set->asciiInPrimary.fill(-1);
    set->style = SkFontStyle(bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight, SkFontStyle::kNormal_Width,
                             italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    TextContext& text = Application::instance()->text();
    sk_sp<SkFontMgr> manager = text.fontCollection()->getFallbackManager();
    if (manager) {
        set->primary = manager->matchFamilyStyle(family.c_str(), set->style);
        if (!set->primary)
            set->primary = manager->matchFamilyStyle(text.family().c_str(), set->style);
        if (!set->primary)
            set->primary = manager->legacyMakeTypeface(nullptr, set->style);
        set->emoji = manager->matchFamilyStyle("emoji", SkFontStyle());
    }
    return *sets.emplace(key, std::move(set)).first->second;
}

static const sk_sp<SkTypeface>& typefaceFor(FontSet& set, char32_t c)
{
    // No ASCII character has the emoji presentation.
    if (c < 0x80) {
        int8_t& known = set.asciiInPrimary[c];
        if (known < 0)
            known = !set.primary || c < 0x20 || set.primary->unicharToGlyph(SkUnichar(c)) != 0;
        if (known)
            return set.primary;
    }
    // Emoji presentation goes to the colour emoji font first.
    if (u_hasBinaryProperty(UChar32(c), UCHAR_EMOJI_PRESENTATION) && set.emoji
        && set.emoji->unicharToGlyph(SkUnichar(c)))
        return set.emoji;
    if (!set.primary || set.primary->unicharToGlyph(SkUnichar(c)) != 0 || c < 0x20)
        return set.primary;
    if (const auto it = set.fallbacks.find(c); it != set.fallbacks.end())
        return it->second;
    if (!set.fallback)
        set.fallback = std::make_unique<FontFallback>(set.family, set.style);
    sk_sp<SkTypeface> fallback = set.fallback->typefaceFor(c);
    if (!fallback)
        fallback = set.primary;
    if (set.fallbacks.size() >= kMaximumFallbacks)
        set.fallbacks.clear();
    return set.fallbacks.emplace(c, std::move(fallback)).first->second;
}

// The typeface of the character ending at next, the emoji font when the
// emoji variation selector follows it.
static sk_sp<SkTypeface> presentedTypeface(FontSet& set, const std::string& text, UChar32 c, size_t next)
{
    const sk_sp<SkTypeface>& face = typefaceFor(set, char32_t(std::max<UChar32>(c, 0)));
    if (set.emoji && face != set.emoji && text.compare(next, 3, "\xEF\xB8\x8F") == 0
        && set.emoji->unicharToGlyph(SkUnichar(c)))
        return set.emoji;
    return face;
}

namespace {

// Glyphs of one shaped piece: one font, direction and style.
struct GlyphRun
{
    sk_sp<SkTypeface> typeface;
    bool bold = false;
    bool italic = false;
    std::vector<SkGlyphID> glyphs;
    // Each glyph's pen position and offset, relative to its cluster's start.
    std::vector<float> x;
    std::vector<float> y;
    // The typeface's line metrics.
    double ascent = 0;
    double descent = 0;
};

// The smallest unit a line holds: one or more characters that shape
// together, with the glyphs that draw them.
struct Cluster
{
    size_t start = 0;
    size_t end = 0;
    double advance = 0;
    uint8_t level = 0;
    bool tab = false;
    bool space = false;
    int run = -1;
    size_t glyphStart = 0;
    size_t glyphCount = 0;
};

struct LaidLine
{
    TextLayout::Line line;
    size_t firstCluster = 0;
    size_t clusterCount = 0;
    // Per cluster of the line, logical order: its left edge and advance.
    std::vector<double> left;
    std::vector<double> advance;
};

class Collector final : public SkShaper::RunHandler
{
public:
    Collector(std::vector<GlyphRun>& runs, std::vector<Cluster>& clusters, size_t base, uint8_t level, bool bold,
              bool italic, bool roundAdvances)
        : m_runs(runs)
        , m_clusters(clusters)
        , m_base(base)
        , m_level(level)
        , m_bold(bold)
        , m_italic(italic)
        , m_round(roundAdvances)
    {
    }

    void beginLine() override {}
    void runInfo(const RunInfo&) override {}
    void commitRunInfo() override {}

    Buffer runBuffer(const RunInfo& info) override
    {
        m_glyphs.assign(info.glyphCount, 0);
        m_positions.assign(info.glyphCount + 1, SkPoint::Make(0, 0));
        m_offsets.assign(info.glyphCount, SkPoint::Make(0, 0));
        m_clusterIndices.assign(info.glyphCount, 0);
        return {m_glyphs.data(), m_positions.data(), m_offsets.data(), m_clusterIndices.data(), SkPoint::Make(0, 0)};
    }

    void commitRunBuffer(const RunInfo& info) override
    {
        const size_t count = info.glyphCount;
        m_positions[count] = SkPoint::Make(info.fAdvance.x(), 0);
        GlyphRun run;
        run.typeface = info.fFont.refTypeface();
        run.bold = m_bold;
        run.italic = m_italic;
        const int runIndex = int(m_runs.size());
        run.glyphs.assign(m_glyphs.begin(), m_glyphs.begin() + long(count));
        run.x.resize(count);
        run.y.resize(count);
        // Glyphs grouped by cluster value, in the run's (visual) order.
        size_t i = 0;
        std::vector<Cluster>& made = m_made;
        made.clear();
        while (i < count) {
            const uint32_t value = m_clusterIndices[i];
            size_t j = i;
            Cluster cluster;
            cluster.start = m_base + value;
            cluster.level = m_level;
            cluster.run = runIndex;
            cluster.glyphStart = i;
            double pen = 0;
            while (j < count && m_clusterIndices[j] == value) {
                const double advance = m_positions[j + 1].x() - m_positions[j].x();
                run.x[j] = float(pen + m_offsets[j].x());
                run.y[j] = m_offsets[j].y();
                pen += m_round ? std::round(advance) : advance;
                ++j;
            }
            cluster.glyphCount = j - i;
            cluster.advance = pen;
            made.push_back(cluster);
            i = j;
        }
        // Logical order: right to left runs come reversed.
        if (m_level & 1)
            std::reverse(made.begin(), made.end());
        // Ends from the next cluster's start, in logical order.
        for (size_t k = 0; k < made.size(); ++k)
            made[k].end = k + 1 < made.size() ? made[k + 1].start : m_base + info.utf8Range.end();
        m_runs.push_back(std::move(run));
        m_pending.insert(m_pending.end(), made.begin(), made.end());
    }

    void commitLine() override {}

    // Clusters of all runs, in logical order.
    void finish()
    {
        const auto byStart = [](const Cluster& a, const Cluster& b) { return a.start < b.start; };
        if (!std::is_sorted(m_pending.begin(), m_pending.end(), byStart))
            std::sort(m_pending.begin(), m_pending.end(), byStart);
        m_clusters.insert(m_clusters.end(), m_pending.begin(), m_pending.end());
    }

private:
    std::vector<GlyphRun>& m_runs;
    std::vector<Cluster>& m_clusters;
    size_t m_base;
    uint8_t m_level;
    bool m_bold;
    bool m_italic;
    bool m_round;
    std::vector<SkGlyphID> m_glyphs;
    std::vector<SkPoint> m_positions;
    std::vector<SkPoint> m_offsets;
    std::vector<uint32_t> m_clusterIndices;
    std::vector<Cluster> m_pending;
    std::vector<Cluster> m_made;
};

} // namespace

struct TextLayoutPrivate
{
    std::string text;
    FontSpec font;
    double scale = 1;
    bool hintedLayout = true;
    TextLayout::Wrap wrap = TextLayout::Wrap::None;
    double tabStop = 0;
    std::vector<TextFormatRange> formats;
    Color defaultColor = Color::rgb(0x000000);

    bool shaped = false;
    double pixelSize = 0;
    bool roundAdvances = false;
    double ascent = 0;
    double descent = 0;
    double spaceAdvance = 0;
    double underlinePosition = 0;
    double underlineThickness = 1;
    std::vector<GlyphRun> runs;
    std::vector<Cluster> clusters;
    // Whether a line may break before the cluster.
    std::vector<uint8_t> breakBefore;
    // Grapheme boundaries in UTF-8 offsets, for breaking anywhere.
    std::vector<uint8_t> graphemeStart;

    std::vector<LaidLine> lines;
    size_t nextCluster = 0;
    bool laidOut = false;

    // Drawing cache: blobs per colour and the underlines, in device pixels
    // from the layout's origin.
    struct Batch
    {
        Color color;
        sk_sp<SkTextBlob> blob;
    };
    std::vector<Batch> batches;
    std::vector<std::pair<SkRect, Color>> underlines;
    bool drawCached = false;
    double drawScale = 0;

    void shape();
    void invalidate()
    {
        shaped = false;
        drawCached = false;
        lines.clear();
    }
    const TextFormatRange* formatAt(size_t offset) const;
    void buildDrawCache();
};

const TextFormatRange* TextLayoutPrivate::formatAt(size_t offset) const
{
    const TextFormatRange* found = nullptr;
    for (const TextFormatRange& range : formats) {
        if (offset >= range.start && offset < range.start + range.length)
            found = &range;
    }
    return found;
}

static std::unique_ptr<SkShaper> s_shaper;

void TextLayoutPrivate::shape()
{
    if (shaped)
        return;
    shaped = true;
    runs.clear();
    clusters.clear();
    drawCached = false;

    // Measured once per font and scale: every line of a document shares it.
    static std::map<std::tuple<std::string, double, bool, bool, double, bool>, FontMetrics> metricsCache;
    const auto key = std::make_tuple(font.family, font.pointSize, font.bold, font.italic, scale, hintedLayout);
    auto cached = metricsCache.find(key);
    if (cached == metricsCache.end())
        cached = metricsCache.emplace(key, FontMetrics(font, scale, hintedLayout)).first;
    const FontMetrics& metrics = cached->second;
    pixelSize = metrics.pixelSize();
    ascent = metrics.ascentF();
    descent = metrics.descentF();
    underlinePosition = metrics.underlinePosF();
    underlineThickness = metrics.lineWidthF();
    spaceAdvance = metrics.horizontalAdvanceF(" ");
    roundAdvances = Application::instance()->text().roundsMetrics(scale, hintedLayout);

    // UTF-16, for ICU and the line break rules, and the way back to UTF-8.
    std::u16string utf16;
    std::vector<size_t> utf8Of;
    std::vector<size_t> utf16Of(text.size() + 1, 0);
    {
        int32_t i = 0;
        const int32_t size = int32_t(text.size());
        while (i < size) {
            const int32_t start = i;
            UChar32 c;
            U8_NEXT(text.data(), i, size, c);
            if (c < 0)
                c = 0xFFFD;
            for (int32_t k = start; k < i; ++k)
                utf16Of[size_t(k)] = utf16.size();
            if (c > 0xFFFF) {
                utf16.push_back(char16_t(0xD7C0 + (c >> 10)));
                utf16.push_back(char16_t(0xDC00 | (c & 0x3FF)));
                utf8Of.push_back(size_t(start));
                utf8Of.push_back(size_t(start));
            } else {
                utf16.push_back(char16_t(c));
                utf8Of.push_back(size_t(start));
            }
        }
        utf16Of[text.size()] = utf16.size();
        utf8Of.push_back(text.size());
    }

    // ASCII text, most of a code editor's, has level 0 throughout and a
    // grapheme per byte but for CR LF.
    const bool ascii = utf16.size() == text.size();

    // Bidirectional embedding levels, for splitting runs.
    std::vector<uint8_t> levels(text.size(), 0);
    if (!utf16.empty() && !ascii) {
        UErrorCode status = U_ZERO_ERROR;
        UBiDi* bidi = ubidi_openSized(int32_t(utf16.size()), 0, &status);
        if (U_SUCCESS(status)) {
            ubidi_setPara(bidi, reinterpret_cast<const UChar*>(utf16.data()), int32_t(utf16.size()),
                          UBIDI_DEFAULT_LTR, nullptr, &status);
            if (U_SUCCESS(status)) {
                for (size_t i = 0; i < text.size(); ++i)
                    levels[i] = ubidi_getLevelAt(bidi, int32_t(utf16Of[i]));
            }
            ubidi_close(bidi);
        }
    }

    graphemeStart.assign(text.size() + 1, ascii ? 1 : 0);
    graphemeStart[0] = 1;
    graphemeStart[text.size()] = 1;
    if (ascii) {
        for (size_t k = 1; k < text.size(); ++k) {
            if (text[k - 1] == '\r' && text[k] == '\n')
                graphemeStart[k] = 0;
        }
    } else if (!utf16.empty()) {
        UErrorCode status = U_ZERO_ERROR;
        UBreakIterator* graphemes = ubrk_open(UBRK_CHARACTER, nullptr, reinterpret_cast<const UChar*>(utf16.data()),
                                              int32_t(utf16.size()), &status);
        if (U_SUCCESS(status)) {
            for (int32_t b = ubrk_first(graphemes); b != UBRK_DONE; b = ubrk_next(graphemes))
                graphemeStart[utf8Of[size_t(b)]] = 1;
            ubrk_close(graphemes);
        }
    }

    if (!s_shaper)
        s_shaper = SkShapers::HB::ShapeDontWrapOrReorder(Application::instance()->text().unicode(), nullptr);

    // Pieces of one typeface, level and style; tabs stand alone, unshaped.
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\t') {
            Cluster tab;
            tab.start = i;
            tab.end = i + 1;
            tab.level = levels[i];
            tab.tab = true;
            tab.space = true;
            clusters.push_back(tab);
            ++i;
            continue;
        }
        const TextFormatRange* format = formatAt(i);
        const bool bold = format ? format->bold || font.bold : font.bold;
        const bool italic = format ? format->italic || font.italic : font.italic;
        FontSet& set = fontSet(font.family, bold, italic);
        int32_t next = int32_t(i);
        UChar32 c;
        U8_NEXT(text.data(), next, int32_t(text.size()), c);
        sk_sp<SkTypeface> face = presentedTypeface(set, text, c, size_t(next));
        const uint8_t level = levels[i];
        size_t end = size_t(next);
        // Marks and joiners stay with what they follow.
        while (end < text.size() && text[end] != '\t' && levels[end] == level) {
            const TextFormatRange* nextFormat = formatAt(end);
            const bool nextBold = nextFormat ? nextFormat->bold || font.bold : font.bold;
            const bool nextItalic = nextFormat ? nextFormat->italic || font.italic : font.italic;
            if (nextBold != bold || nextItalic != italic)
                break;
            if (static_cast<unsigned char>(text[end]) < 0x80) {
                if (typefaceFor(set, char32_t(text[end])) != face)
                    break;
                ++end;
                continue;
            }
            int32_t after = int32_t(end);
            UChar32 d;
            U8_NEXT(text.data(), after, int32_t(text.size()), d);
            const int8_t category = int8_t(u_charType(std::max<UChar32>(d, 0)));
            const bool joins = category == U_NON_SPACING_MARK || category == U_ENCLOSING_MARK
                    || category == U_COMBINING_SPACING_MARK || d == 0x200D || (d >= 0xFE00 && d <= 0xFE0F)
                    || (d >= 0x1F3FB && d <= 0x1F3FF) || (d >= 0xE0020 && d <= 0xE007F);
            if (!joins && presentedTypeface(set, text, d, size_t(after)) != face)
                break;
            end = size_t(after);
        }

        SkFont skFont(face, SkScalar(pixelSize));
        skFont.setHinting(SkFontHinting::kNone);
        skFont.setLinearMetrics(true);
        skFont.setSubpixel(true);
        if (bold && face->fontStyle().weight() < SkFontStyle::kBold_Weight && !face->isFixedPitch())
            skFont.setEmbolden(true);
        if (italic && face->fontStyle().slant() == SkFontStyle::kUpright_Slant)
            skFont.setSkewX(-float(0x0366A) / 65536.0f);
        const size_t length = end - i;
        SkShaper::TrivialFontRunIterator fontRuns(skFont, length);
        SkShaper::TrivialBiDiRunIterator bidiRuns(level, length);
        std::unique_ptr<SkShaper::ScriptRunIterator> script = SkShapers::HB::ScriptRunIterator(text.data() + i, length);
        const std::string& locale = Application::instance()->text().locale();
        SkShaper::TrivialLanguageRunIterator language(locale.empty() ? "en" : locale.c_str(), length);
        Collector collector(runs, clusters, i, level, bold, italic, roundAdvances);
        if (s_shaper && script) {
            s_shaper->shape(text.data() + i, length, fontRuns, bidiRuns, *script, language, nullptr, 0,
                            std::numeric_limits<SkScalar>::max(), &collector);
        }
        collector.finish();
        i = end;
    }
    const auto byStart = [](const Cluster& a, const Cluster& b) { return a.start < b.start; };
    if (!std::is_sorted(clusters.begin(), clusters.end(), byStart))
        std::sort(clusters.begin(), clusters.end(), byStart);
    const TextContext& context = Application::instance()->text();
    // Distinct typefaces are few, however many runs alternate between them.
    std::vector<std::pair<const SkTypeface*, VerticalMetrics>> measured;
    for (GlyphRun& run : runs) {
        auto it = std::find_if(measured.begin(), measured.end(),
                               [&](const auto& entry) { return entry.first == run.typeface.get(); });
        if (it == measured.end()) {
            measured.push_back({run.typeface.get(), context.metrics(run.typeface.get(), pixelSize, scale, hintedLayout)});
            it = measured.end() - 1;
        }
        run.ascent = it->second.ascent;
        run.descent = it->second.descent;
    }
    for (Cluster& cluster : clusters) {
        if (!cluster.tab) {
            const unsigned char first = static_cast<unsigned char>(text[cluster.start]);
            cluster.space = first == ' ' || first == '\t';
            if (first == 0xC2 && cluster.start + 1 < text.size() && static_cast<unsigned char>(text[cluster.start + 1]) == 0xA0)
                cluster.space = false;
        }
    }

    // Line break opportunities, at cluster starts.
    const std::vector<uint8_t> breaks = lineBreaks(utf16);
    breakBefore.assign(clusters.size(), 0);
    for (size_t k = 0; k < clusters.size(); ++k) {
        const size_t position = utf16Of[clusters[k].start];
        breakBefore[k] = position < breaks.size() ? breaks[position] : 0;
    }
}

TextLayout::TextLayout()
    : d(std::make_unique<TextLayoutPrivate>())
{
}

TextLayout::TextLayout(std::string text, const FontSpec& font, double scale, bool hintedLayout)
    : d(std::make_unique<TextLayoutPrivate>())
{
    d->text = std::move(text);
    d->font = font;
    d->scale = scale;
    d->hintedLayout = hintedLayout;
}

TextLayout::~TextLayout() = default;
TextLayout::TextLayout(TextLayout&&) noexcept = default;
TextLayout& TextLayout::operator=(TextLayout&&) noexcept = default;

void TextLayout::setText(std::string text)
{
    d->text = std::move(text);
    d->invalidate();
}

const std::string& TextLayout::text() const
{
    return d->text;
}

void TextLayout::setFont(const FontSpec& font)
{
    d->font = font;
    d->invalidate();
}

void TextLayout::setScale(double scale, bool hintedLayout)
{
    d->scale = scale;
    d->hintedLayout = hintedLayout;
    d->invalidate();
}

void TextLayout::setWrap(Wrap wrap)
{
    d->wrap = wrap;
}

void TextLayout::setTabStopDistance(double distance)
{
    d->tabStop = distance;
}

void TextLayout::setFormats(std::vector<TextFormatRange> formats)
{
    d->formats = std::move(formats);
    d->invalidate();
}

void TextLayout::setDefaultColor(Color color)
{
    d->defaultColor = color;
    d->drawCached = false;
}

void TextLayout::beginLayout()
{
    d->shape();
    d->lines.clear();
    d->nextCluster = 0;
    d->laidOut = false;
    d->drawCached = false;
}

int TextLayout::createLine(double width, PointF position)
{
    TextLayoutPrivate& p = *d;
    const bool empty = p.clusters.empty();
    if (p.nextCluster >= p.clusters.size() && !(empty && p.lines.empty()))
        return -1;

    const double tabStop = p.tabStop > 0 ? p.tabStop : p.spaceAdvance * 8;
    const auto tabAdvance = [&](double x) {
        if (tabStop <= 0)
            return 0.0;
        const double next = (std::floor(x / tabStop + 1e-9) + 1) * tabStop;
        return next - x;
    };

    // Takes clusters while they fit; a break is allowed before a cluster
    // whose breakBefore is set, and trailing spaces never overflow.
    const size_t first = p.nextCluster;
    size_t end = first;
    double x = 0;
    size_t lastBreak = first;
    std::vector<double> advances;
    if (!empty) {
        for (size_t k = first; k < p.clusters.size(); ++k) {
            const Cluster& cluster = p.clusters[k];
            const double advance = cluster.tab ? tabAdvance(x) : cluster.advance;
            if (k > first && p.breakBefore[k])
                lastBreak = k;
            const bool wraps = p.wrap == Wrap::WordOrAnywhere;
            if (wraps && k > first && !cluster.space && x + advance > width + 1e-6) {
                // Back to the last break; a word wider than the line breaks
                // at a grapheme instead.
                if (lastBreak > first) {
                    end = lastBreak;
                } else {
                    end = k;
                    while (end > first + 1 && !p.graphemeStart[p.clusters[end].start])
                        --end;
                }
                break;
            }
            x += advance;
            advances.push_back(advance);
            end = k + 1;
        }
    }

    LaidLine laid;
    laid.firstCluster = first;
    laid.clusterCount = end - first;
    laid.line.textStart = empty ? 0 : p.clusters[first].start;
    laid.line.textLength = empty ? 0 : (end > first ? p.clusters[end - 1].end : laid.line.textStart) - laid.line.textStart;
    laid.line.width = width;
    laid.line.position = position;
    // The largest of the fonts the line uses, fallbacks included: each run
    // takes the ascent of every font its glyphs came from.
    laid.line.ascent = p.ascent;
    laid.line.descent = p.descent;
    int lastRun = -1;
    for (size_t k = first; k < end; ++k) {
        const int run = p.clusters[k].run;
        if (run < 0 || run == lastRun)
            continue;
        lastRun = run;
        laid.line.ascent = std::max(laid.line.ascent, p.runs[size_t(run)].ascent);
        laid.line.descent = std::max(laid.line.descent, p.runs[size_t(run)].descent);
    }
    advances.resize(laid.clusterCount);
    double natural = 0;
    for (double advance : advances)
        natural += advance;
    laid.line.naturalTextWidth = natural;

    // Visual order by levels (UAX #9 L2), then each cluster's left edge.
    std::vector<size_t> order(laid.clusterCount);
    for (size_t k = 0; k < order.size(); ++k)
        order[k] = k;
    uint8_t highest = 0;
    uint8_t lowestOdd = 255;
    for (size_t k = 0; k < order.size(); ++k) {
        const uint8_t level = p.clusters[first + k].level;
        highest = std::max(highest, level);
        if (level & 1)
            lowestOdd = std::min(lowestOdd, level);
    }
    for (int level = highest; level >= int(lowestOdd) && level > 0; --level) {
        size_t k = 0;
        while (k < order.size()) {
            if (p.clusters[first + order[k]].level >= level) {
                size_t j = k;
                while (j < order.size() && p.clusters[first + order[j]].level >= level)
                    ++j;
                std::reverse(order.begin() + long(k), order.begin() + long(j));
                k = j;
            } else {
                ++k;
            }
        }
    }
    laid.left.assign(laid.clusterCount, 0);
    laid.advance = advances;
    double pen = 0;
    for (size_t index : order) {
        laid.left[index] = pen;
        pen += advances[index];
    }

    p.nextCluster = end;
    p.lines.push_back(std::move(laid));
    p.drawCached = false;
    return int(p.lines.size()) - 1;
}

void TextLayout::endLayout()
{
    d->laidOut = true;
}

int TextLayout::lineCount() const
{
    return int(d->lines.size());
}

const TextLayout::Line& TextLayout::lineAt(int index) const
{
    static const Line none;
    return index >= 0 && index < lineCount() ? d->lines[size_t(index)].line : none;
}

int TextLayout::lineForTextPosition(size_t offset) const
{
    for (int i = 0; i < lineCount(); ++i) {
        const Line& line = d->lines[size_t(i)].line;
        if (offset < line.textStart + line.textLength)
            return i;
    }
    return lineCount() - 1;
}

double TextLayout::maximumWidth() const
{
    double widest = 0;
    for (const LaidLine& line : d->lines)
        widest = std::max(widest, line.line.naturalTextWidth);
    return widest;
}

double TextLayout::cursorToX(int lineIndex, size_t offset) const
{
    if (lineIndex < 0 || lineIndex >= lineCount())
        return 0;
    const LaidLine& laid = d->lines[size_t(lineIndex)];
    const Line& line = laid.line;
    const double base = line.position.x;
    if (laid.clusterCount == 0)
        return base;
    double x = 0;
    bool found = false;
    for (size_t k = 0; k < laid.clusterCount && !found; ++k) {
        const Cluster& cluster = d->clusters[laid.firstCluster + k];
        if (offset < cluster.start || offset >= cluster.end)
            continue;
        found = true;
        // At the start of a run of another direction, the caret stays
        // after what comes before.
        if (offset == cluster.start && k > 0) {
            const Cluster& previous = d->clusters[laid.firstCluster + k - 1];
            if (previous.level != cluster.level) {
                x = (previous.level & 1) ? laid.left[k - 1] : laid.left[k - 1] + laid.advance[k - 1];
                break;
            }
        }
        // Inside a ligature, the share of its graphemes; a position inside
        // a grapheme goes to its end.
        int graphemes = 0;
        int before = 0;
        size_t snapped = offset;
        while (snapped < cluster.end && !d->graphemeStart[snapped])
            ++snapped;
        for (size_t i = cluster.start; i < cluster.end; ++i) {
            if (d->graphemeStart[i]) {
                ++graphemes;
                if (i < snapped)
                    ++before;
            }
        }
        const double fraction = graphemes > 0 ? double(before) / graphemes : 0;
        const double along = laid.advance[k] * fraction;
        const bool rtl = cluster.level & 1;
        x = rtl ? laid.left[k] + laid.advance[k] - along : laid.left[k] + along;
    }
    if (!found) {
        // The end of the line: after its last logical cluster.
        const size_t last = laid.clusterCount - 1;
        const Cluster& cluster = d->clusters[laid.firstCluster + last];
        x = (cluster.level & 1) ? laid.left[last] : laid.left[last] + laid.advance[last];
    }
    // Wrapped, trailing spaces past the width stop at its edge.
    if (d->wrap != Wrap::None && x > line.width)
        x = line.width;
    return base + x;
}

size_t TextLayout::xToCursor(int lineIndex, double x) const
{
    if (lineIndex < 0 || lineIndex >= lineCount())
        return 0;
    const LaidLine& laid = d->lines[size_t(lineIndex)];
    const Line& line = laid.line;
    const double local = x - line.position.x;
    if (laid.clusterCount == 0)
        return line.textStart;
    double leftmost = std::numeric_limits<double>::max();
    double rightmost = std::numeric_limits<double>::lowest();
    for (size_t k = 0; k < laid.clusterCount; ++k) {
        leftmost = std::min(leftmost, laid.left[k]);
        rightmost = std::max(rightmost, laid.left[k] + laid.advance[k]);
        if (local < laid.left[k] || local >= laid.left[k] + laid.advance[k])
            continue;
        const Cluster& cluster = d->clusters[laid.firstCluster + k];
        const bool before = local < laid.left[k] + laid.advance[k] / 2;
        const bool rtl = cluster.level & 1;
        return before != rtl ? cluster.start : cluster.end;
    }
    const bool rtlLine = d->clusters[laid.firstCluster].level & 1;
    if (local < leftmost)
        return rtlLine ? line.textStart + line.textLength : line.textStart;
    return rtlLine ? line.textStart : line.textStart + line.textLength;
}

void TextLayoutPrivate::buildDrawCache()
{
    batches.clear();
    underlines.clear();
    TextContext& context = Application::instance()->text();
    struct Pending
    {
        Color color;
        std::vector<std::pair<int, std::pair<SkGlyphID, SkPoint>>> glyphs;
    };
    std::vector<Pending> pending;
    const auto pendingFor = [&pending](Color color) -> Pending& {
        for (Pending& entry : pending) {
            if (entry.color == color)
                return entry;
        }
        pending.push_back({color, {}});
        return pending.back();
    };
    for (const LaidLine& laid : lines) {
        const double baseline = std::round((laid.line.position.y + laid.line.ascent) * drawScale);
        for (size_t k = 0; k < laid.clusterCount; ++k) {
            const Cluster& cluster = clusters[laid.firstCluster + k];
            const TextFormatRange* format = formatAt(cluster.start);
            const Color color = format && format->color ? *format->color : defaultColor;
            const double left = (laid.line.position.x + laid.left[k]) * drawScale;
            if (format && format->underline) {
                const double top = baseline + std::round(underlinePosition * drawScale);
                underlines.push_back({SkRect::MakeXYWH(SkScalar(left), SkScalar(top), SkScalar(laid.advance[k] * drawScale),
                                                       SkScalar(std::max(1.0, std::round(underlineThickness * drawScale)))),
                                      color});
            }
            if (cluster.run < 0)
                continue;
            Pending& entry = pendingFor(color);
            const GlyphRun& run = runs[size_t(cluster.run)];
            for (size_t g = 0; g < cluster.glyphCount; ++g) {
                const size_t index = cluster.glyphStart + g;
                entry.glyphs.push_back({cluster.run,
                                        {run.glyphs[index], SkPoint::Make(SkScalar(left + run.x[index] * drawScale),
                                                                          SkScalar(baseline + run.y[index] * drawScale))}});
            }
        }
    }
    for (Pending& entry : pending) {
        // One blob run per glyph run (font), in order met.
        std::stable_sort(entry.glyphs.begin(), entry.glyphs.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        SkTextBlobBuilder builder;
        size_t i = 0;
        while (i < entry.glyphs.size()) {
            const int runIndex = entry.glyphs[i].first;
            size_t j = i;
            while (j < entry.glyphs.size() && entry.glyphs[j].first == runIndex)
                ++j;
            const GlyphRun& run = runs[size_t(runIndex)];
            SkFont font(run.typeface, SkScalar(pixelSize * drawScale));
            font.setEdging(context.edging());
            // As labels: unhinted at fractional scales, else fontconfig's
            // hinting, on whole pixels when that hints horizontally too.
            const bool unhinted = TextContext::drawsUnhinted(drawScale);
            const SkFontHinting hinting = unhinted ? SkFontHinting::kNone : context.fontconfigHinting();
            font.setHinting(hinting);
            font.setSubpixel(unhinted
                             || (context.subpixelPositioning() && hinting != SkFontHinting::kNormal
                                 && hinting != SkFontHinting::kFull));
            if (run.bold && run.typeface->fontStyle().weight() < SkFontStyle::kBold_Weight
                && !run.typeface->isFixedPitch())
                font.setEmbolden(true);
            if (run.italic && run.typeface->fontStyle().slant() == SkFontStyle::kUpright_Slant)
                font.setSkewX(-float(0x0366A) / 65536.0f);
            const SkTextBlobBuilder::RunBuffer& buffer = builder.allocRunPos(font, int(j - i));
            for (size_t k = i; k < j; ++k) {
                buffer.glyphs[k - i] = entry.glyphs[k].second.first;
                buffer.points()[k - i] = entry.glyphs[k].second.second;
            }
            i = j;
        }
        batches.push_back({entry.color, builder.make()});
    }
    drawCached = true;
}

void TextLayout::draw(Painter& painter, PointF origin)
{
    if (painter.scale() != d->drawScale) {
        d->drawScale = painter.scale();
        d->drawCached = false;
    }
    if (!d->drawCached)
        d->buildDrawCache();
    SkCanvas* canvas = painter.canvas();
    canvas->save();
    // Whole device pixels, so a layout drawn again elsewhere looks the same.
    canvas->translate(SkScalar(painter.toDevice(painter.offset().x + origin.x)),
                      SkScalar(painter.toDevice(painter.offset().y + origin.y)));
    const double opacity = painter.opacity();
    for (const auto& [rect, color] : d->underlines) {
        SkPaint paint;
        paint.setColor(SkColorSetARGB(uint8_t(std::lround(color.a * opacity)), color.r, color.g, color.b));
        canvas->drawRect(rect, paint);
    }
    for (const TextLayoutPrivate::Batch& batch : d->batches) {
        if (!batch.blob)
            continue;
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(SkColorSetARGB(uint8_t(std::lround(batch.color.a * opacity)), batch.color.r, batch.color.g,
                                      batch.color.b));
        drawTextBlobForTarget(canvas, batch.blob, 0, 0, paint);
    }
    canvas->restore();
}

FontSpec systemFixedFont()
{
    const Theme& theme = Theme::current();
    return {theme.fixedFontFamily.empty() ? std::string("Noto Mono") : theme.fixedFontFamily, theme.fontPointSize,
            false, false};
}

} // namespace cutefish::ui
