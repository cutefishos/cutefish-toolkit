// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/text_lines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <optional>
#include <unordered_map>
#include <variant>

#include "include/core/SkBlender.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkM44.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkRuntimeEffect.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/src/ParagraphPainterImpl.h"

#include "ui/application.h"
#include "ui/embedded_files.h"
#include "ui/line_breaks.h"
#include "ui/text_context.h"

namespace textlayout = skia::textlayout;

namespace cutefish::ui {

std::vector<TextLineBox> textLineBoxes(textlayout::Paragraph* paragraph, const SkTypeface* typeface,
                                       double pixelSize, double scale, bool hintedLayout)
{
    const TextContext& context = Application::instance()->text();
    // The typefaces each line's glyphs use: fallback fonts count, each run
    // taking the ascent of the fonts its glyphs came from.
    const size_t count = std::max<size_t>(1, paragraph ? paragraph->lineNumber() : 0);
    std::vector<std::vector<sk_sp<SkTypeface>>> typefaces(count);
    if (paragraph) {
        paragraph->visit([&](int lineNumber, const textlayout::Paragraph::VisitorInfo* info) {
            if (!info || info->count == 0 || lineNumber < 0 || size_t(lineNumber) >= count)
                return;
            auto& used = typefaces[size_t(lineNumber)];
            const sk_sp<SkTypeface> face = info->font.refTypeface();
            if (face && std::none_of(used.begin(), used.end(), [&](const sk_sp<SkTypeface>& other) {
                    return other->uniqueID() == face->uniqueID();
                }))
                used.push_back(face);
        });
    }

    std::vector<TextLineBox> lines(count);
    double top = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t ascent = 0;
        int64_t descent = 0;
        const auto add = [&](const SkTypeface* face) {
            const VerticalMetrics metrics = context.metrics(face, pixelSize, scale, hintedLayout);
            ascent = std::max(ascent, int64_t(std::lround(metrics.ascent * 64)));
            descent = std::max(descent, int64_t(std::lround(metrics.descent * 64)));
        };
        if (typefaces[i].empty())
            add(typeface);
        for (const sk_sp<SkTypeface>& face : typefaces[i])
            add(face.get());
        // The 26.6 sum rounded up to whole pixels.
        const double height = ((ascent + descent + 63) & -64) / 64.0;
        lines[i] = {top, height, ascent / 64.0};
        top += height;
    }
    return lines;
}

double textHeight(const std::vector<TextLineBox>& lines)
{
    return lines.empty() ? 0 : lines.back().top + lines.back().height;
}

// Advances in 26.6 fixed point, truncated from FreeType's linear advance.
static double truncated(double advance)
{
    return std::floor(advance * 64) / 64;
}

namespace {

// A run of glyphs of one font and colour on a line, at the paragraph's own
// positions.
struct GlyphRun
{
    SkFont font;
    std::vector<SkGlyphID> glyphs;
    // From the paragraph's left, one per glyph.
    std::vector<double> x;
    // Where each glyph's cluster starts in the text.
    std::vector<size_t> clusters;
    size_t utf8Offset = 0;
    double clipRight = 0;
};

// A line's runs in visual order, each glyph with its truncated advance.
struct GlyphLine
{
    std::vector<GlyphRun> runs;
    std::vector<std::vector<double>> advances;
    // The advance, which alignment uses, and the width, which adds the last
    // glyph's overhang and sizes the text.
    double advance = 0;
    double width = 0;
};

} // namespace

// The right bearing: a whole-pixel advance less the glyph's whole-pixel
// right edge.
static double rightBearing(const SkFont& runFont, SkGlyphID glyph, double advance)
{
    SkFont font = runFont;
    font.setSubpixel(false);
    font.setHinting(SkFontHinting::kNone);
    font.setLinearMetrics(true);
    SkRect bounds;
    font.getBounds({&glyph, 1}, {&bounds, 1}, nullptr);
    if (bounds.isEmpty())
        return 0;
    return std::floor(advance + 0.5) - std::ceil(bounds.right());
}

static std::vector<GlyphLine> glyphLines(textlayout::Paragraph* paragraph, const AdvanceRule& rule)
{
    std::vector<GlyphLine> lines(paragraph->lineNumber());
    paragraph->visit([&](int lineNumber, const textlayout::Paragraph::VisitorInfo* info) {
        if (!info || info->count == 0 || lineNumber < 0 || size_t(lineNumber) >= lines.size())
            return;
        GlyphRun run;
        run.font = info->font;
        run.glyphs.assign(info->glyphs, info->glyphs + info->count);
        for (int i = 0; i < info->count; ++i) {
            run.x.push_back(info->origin.fX + info->positions[i].fX);
            run.clusters.push_back(info->utf8Starts[i]);
        }
        run.utf8Offset = info->utf8Starts[0];
        // The run's clip, already from the paragraph's left.
        run.clipRight = info->advanceX;
        lines[size_t(lineNumber)].runs.push_back(std::move(run));
    });

    for (GlyphLine& line : lines) {
        std::sort(line.runs.begin(), line.runs.end(),
                  [](const GlyphRun& a, const GlyphRun& b) { return a.x.front() < b.x.front(); });
        // Each glyph reaches to the next one, the last by its own advance.
        for (size_t r = 0; r < line.runs.size(); ++r) {
            const GlyphRun& run = line.runs[r];
            std::vector<double> advances(run.glyphs.size());
            for (size_t i = 0; i < run.glyphs.size(); ++i) {
                double advance;
                if (i + 1 < run.glyphs.size()) {
                    advance = run.x[i + 1] - run.x[i];
                } else if (r + 1 < line.runs.size()) {
                    advance = line.runs[r + 1].x.front() - run.x[i];
                } else {
                    SkScalar width = 0;
                    run.font.getWidths({&run.glyphs[i], 1}, {&width, 1});
                    advance = width;
                }
                advance = truncated(advance);
                // Spacing comes on top of the glyph's own advance, after
                // rounding.
                advances[i] = (rule.round ? std::floor(advance + 0.5) : advance) + rule.letterSpacing;
                line.width += advances[i];
            }
            line.advances.push_back(std::move(advances));
        }
        line.advance = line.width;
        // The width counts the last glyph's overhang past its advance.
        if (!line.runs.empty()) {
            const GlyphRun& last = line.runs.back();
            const double bearing = rightBearing(last.font, last.glyphs.back(), line.advances.back().back());
            if (bearing < 0)
                line.width -= bearing;
        }
    }
    return lines;
}

struct LineGlyphs
{
    std::vector<GlyphLine> lines;
};

std::shared_ptr<const LineGlyphs> lineGlyphs(textlayout::Paragraph* paragraph, const AdvanceRule& roundAdvances)
{
    return std::make_shared<const LineGlyphs>(LineGlyphs{glyphLines(paragraph, roundAdvances)});
}

static double widestLine(const std::vector<GlyphLine>& lines)
{
    double widest = 0;
    for (const GlyphLine& line : lines)
        widest = std::max(widest, line.width);
    return widest;
}

double naturalWidth(textlayout::Paragraph* paragraph, const AdvanceRule& roundAdvances)
{
    return widestLine(glyphLines(paragraph, roundAdvances));
}

double naturalWidth(const LineGlyphs& glyphs)
{
    return widestLine(glyphs.lines);
}

namespace {

// A glyph cluster in text order, with what line breaking asks of the
// characters it starts at.
struct Cluster
{
    size_t start = 0;
    double advance = 0;
    // Its run's, in the glyph lines the clusters were made from.
    const SkFont* font = nullptr;
    SkGlyphID lastGlyph = 0;
    double lastAdvance = 0;
    bool hasGlyph = false;
    bool space = false;
    bool lineBreak = false;
    bool separator = false;
};

constexpr double kBearingNotCalculated = 1;

} // namespace

static bool isSeparator(const std::string& text, size_t offset)
{
    if (text[offset] == '\n')
        return true;
    // U+2028 and U+2029.
    return offset + 2 < text.size() && static_cast<unsigned char>(text[offset]) == 0xE2
            && static_cast<unsigned char>(text[offset + 1]) == 0x80
            && (static_cast<unsigned char>(text[offset + 2]) == 0xA8
                || static_cast<unsigned char>(text[offset + 2]) == 0xA9);
}

static std::vector<Cluster> clustersOf(const std::vector<GlyphLine>& lines, const std::string& text)
{
    std::vector<Cluster> clusters;
    for (const GlyphLine& line : lines) {
        for (size_t r = 0; r < line.runs.size(); ++r) {
            const GlyphRun& run = line.runs[r];
            for (size_t i = 0; i < run.glyphs.size(); ++i) {
                const double advance = line.advances[r][i];
                if (clusters.empty() || clusters.back().start != run.clusters[i])
                    clusters.push_back({run.clusters[i]});
                Cluster& cluster = clusters.back();
                cluster.advance += advance;
                cluster.font = &run.font;
                cluster.lastGlyph = run.glyphs[i];
                cluster.lastAdvance = advance;
                cluster.hasGlyph = true;
            }
        }
    }
    // Separators end lines whether or not they have glyphs of their own.
    std::vector<Cluster> separators;
    for (size_t i = 0; i < text.size(); ++i) {
        if (isSeparator(text, i))
            separators.push_back({i});
    }
    const auto byStart = [](const Cluster& a, const Cluster& b) { return a.start < b.start; };
    // Left-to-right text comes in order already; ties keep glyphs first.
    if (!std::is_sorted(clusters.begin(), clusters.end(), byStart))
        std::stable_sort(clusters.begin(), clusters.end(), byStart);
    std::vector<Cluster> ordered;
    ordered.reserve(clusters.size() + separators.size());
    std::merge(clusters.begin(), clusters.end(), separators.begin(), separators.end(), std::back_inserter(ordered),
               byStart);
    clusters = std::move(ordered);
    std::vector<Cluster> merged;
    for (Cluster& cluster : clusters) {
        if (!merged.empty() && merged.back().start == cluster.start) {
            if (cluster.hasGlyph)
                merged.back() = cluster;
            continue;
        }
        merged.push_back(cluster);
    }

    // The break attributes, at the UTF-16 position of each cluster.
    std::u16string utf16;
    std::vector<size_t> utf16At(text.size() + 1, 0);
    for (size_t i = 0; i < text.size();) {
        utf16At[i] = utf16.size();
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        uint32_t c = length == 1 ? lead : lead & (0xFF >> (length + 1));
        for (size_t k = 1; k < length && i + k < text.size(); ++k)
            c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        if (c >= 0x10000) {
            utf16.push_back(char16_t(0xD800 + ((c - 0x10000) >> 10)));
            utf16.push_back(char16_t(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            utf16.push_back(char16_t(c));
        }
        i += length;
    }
    utf16At[text.size()] = utf16.size();
    const BreakAttributes attributes = breakAttributes(utf16);
    for (Cluster& cluster : merged) {
        const size_t position = utf16At[std::min(cluster.start, text.size())];
        cluster.separator = isSeparator(text, cluster.start);
        cluster.space = !cluster.separator && attributes.breakableSpace[position];
        cluster.lineBreak = attributes.lineBreak[position];
        if (cluster.separator)
            cluster.advance = 0;
    }
    return merged;
}

// Fills one line with clusters, from the cluster that starts the line: the
// cluster the next line starts at and the line's width.
static std::pair<size_t, double> layoutLine(const std::vector<Cluster>& clusters, size_t from, double width,
                                            bool manualWrap, bool breakany)
{
    const size_t count = clusters.size();
    double lineWidth = 0;
    size_t lineLength = 0;
    double tmpWidth = 0;
    size_t tmpLength = 0;
    double spaceWidth = 0;
    size_t spaceLength = 0;
    double rightBearing = kBearingNotCalculated;
    bool whiteSpaceOrObject = true;
    ptrdiff_t previousGlyph = -1;

    const auto bearingOf = [&clusters](ptrdiff_t index) {
        if (index < 0 || !clusters[size_t(index)].hasGlyph)
            return 0.0;
        const Cluster& cluster = clusters[size_t(index)];
        return std::min(ui::rightBearing(*cluster.font, cluster.lastGlyph, cluster.lastAdvance), 0.0);
    };
    const auto negativeBearing = [&rightBearing] {
        return rightBearing == kBearingNotCalculated ? 0.0 : -rightBearing;
    };
    const auto newWidth = [&] { return lineWidth + tmpWidth + spaceWidth + negativeBearing(); };
    // LineBreakHelper::checkFullOtherwiseExtend.
    const auto full = [&] {
        if (lineLength && !manualWrap && newWidth() > width)
            return true;
        lineWidth += tmpWidth + spaceWidth;
        lineLength += tmpLength + spaceLength;
        tmpWidth = spaceWidth = 0;
        tmpLength = spaceLength = 0;
        return false;
    };

    size_t k = from;
    for (;;) {
        if (k >= count) {
            full();
            break;
        }
        rightBearing = kBearingNotCalculated;
        const Cluster& cluster = clusters[k];
        if (cluster.separator) {
            whiteSpaceOrObject = true;
            ++tmpLength;
            ++k;
            rightBearing = bearingOf(previousGlyph);
            lineWidth += tmpWidth;
            lineLength += tmpLength;
            tmpWidth = 0;
            tmpLength = 0;
            break;
        }
        if (cluster.space) {
            if (k > 0 && !clusters[k - 1].space)
                previousGlyph = ptrdiff_t(k) - 1;
            whiteSpaceOrObject = true;
            while (k < count && clusters[k].space) {
                spaceWidth += clusters[k].advance;
                ++spaceLength;
                ++k;
            }
            continue;
        }
        if (!manualWrap && spaceWidth > width)
            break;
        whiteSpaceOrObject = false;
        if (k == 0 || previousGlyph < 0 || !clusters[k - 1].space)
            previousGlyph = ptrdiff_t(k) - 1;
        bool boundary = false;
        do {
            tmpWidth += clusters[k].advance;
            ++tmpLength;
            ++k;
            if (k >= count || clusters[k].space || clusters[k].lineBreak || clusters[k].separator) {
                boundary = true;
                break;
            }
        } while (!breakany);
        if (boundary || breakany) {
            // Could be skipped while the font's worst overhang fits; the result is the same.
            const double previousBearing = rightBearing;
            rightBearing = bearingOf(ptrdiff_t(k) - 1);
            if (full()) {
                rightBearing = previousBearing != kBearingNotCalculated ? previousBearing : bearingOf(previousGlyph);
                break;
            }
        }
        previousGlyph = ptrdiff_t(k) - 1;
    }

    if (rightBearing == kBearingNotCalculated && !whiteSpaceOrObject && k > 0)
        rightBearing = bearingOf(ptrdiff_t(k) - 1);
    double textWidth = lineWidth + negativeBearing();
    if (lineLength == 0) {
        textWidth += tmpWidth;
        lineLength += tmpLength;
    }
    return {from + lineLength + spaceLength, textWidth};
}

std::vector<size_t> lineStarts(textlayout::Paragraph* unbounded, const std::string& text, double width, LineWrap wrap,
                                 const AdvanceRule& roundAdvances)
{
    std::vector<size_t> starts = {0};
    const std::vector<GlyphLine> glyphs = glyphLines(unbounded, roundAdvances);
    const std::vector<Cluster> clusters = clustersOf(glyphs, text);
    const bool manualWrap = wrap == LineWrap::None;
    size_t from = 0;
    while (from < clusters.size()) {
        auto [next, textWidth] = layoutLine(clusters, from, width, manualWrap, wrap == LineWrap::Anywhere);
        // A word wider than the line starts it and breaks anywhere.
        if (wrap == LineWrap::WordOrAnywhere && textWidth > width)
            next = layoutLine(clusters, from, width, manualWrap, true).first;
        next = std::max(next, from + 1);
        if (next >= clusters.size()) {
            // A separator at the very end leaves an empty last line.
            if (clusters.back().separator)
                starts.push_back(text.size());
            break;
        }
        starts.push_back(clusters[next].start);
        from = next;
    }
    return starts;
}

std::vector<double> lineWidths(textlayout::Paragraph* paragraph, const AdvanceRule& roundAdvances)
{
    std::vector<double> widths;
    for (const GlyphLine& line : glyphLines(paragraph, roundAdvances))
        widths.push_back(line.width);
    return widths;
}

// Subpixel text, also on translucent targets: the glyph's per-channel
// coverage m, times the text's alpha, blended with the text colour as the
// blend constant, and the alpha the average coverage: rgb = C m + dst
// (1 - m), a = avg(m) + dst.a (1 - avg(m)).
static const sk_sp<SkBlender>& subpixelBlender(SkColor color, sk_sp<SkBlender>* made)
{
    static const sk_sp<SkRuntimeEffect> effect = [] {
        const SkRuntimeEffect::Result result = SkRuntimeEffect::MakeForBlender(SkString(embeddedText("subpixel_text.sksl").c_str()));
        if (!result.effect)
            fprintf(stderr, "cutefish-toolkit: %s\n", result.errorText.c_str());
        return result.effect;
    }();
    if (!effect) {
        made->reset();
        return *made;
    }
    const SkV4 value = {SkColorGetR(color) / 255.0f, SkColorGetG(color) / 255.0f, SkColorGetB(color) / 255.0f,
                        SkColorGetA(color) / 255.0f};
    *made = effect->makeBlender(SkData::MakeWithCopy(&value, sizeof(value)));
    return *made;
}

// Whether the canvas may be translucent under text: it has no pixel
// geometry, or it is the canvas of a window with a transparent background.
static bool translucentTarget(SkCanvas* canvas)
{
    return canvas->getBaseProps().pixelGeometry() == kUnknown_SkPixelGeometry
            || Application::instance()->text().transparentWindow();
}

// Whether glyphs drawn on the canvas go through subpixelBlender(): the
// fonts ask for subpixel edging, but the target is translucent, where Skia
// would draw them grey or, given a pixel geometry, with opaque edges.
static bool drawsSubpixelBlended(SkCanvas* canvas, SkFont::Edging edging)
{
    return edging == SkFont::Edging::kSubpixelAntiAlias && Application::instance()->text().translucentSubpixel()
            && translucentTarget(canvas) && Application::instance()->text().pixelGeometry() != kUnknown_SkPixelGeometry;
}

// Subpixel glyphs Skia would draw itself on a translucent target: greyscale,
// as Skia's subpixel masks assume an opaque one and leave edges opaque.
static void greyscaleOnTranslucent(SkCanvas* canvas, SkFont& font)
{
    if (font.getEdging() == SkFont::Edging::kSubpixelAntiAlias && translucentTarget(canvas))
        font.setEdging(SkFont::Edging::kAntiAlias);
}

// Whether the canvas draws at a fractional device scale, where glyphs go
// unhinted (TextContext::drawsUnhinted).
static bool drawsUnhinted(SkCanvas* canvas)
{
    const SkMatrix& matrix = canvas->getTotalMatrix();
    return matrix.isScaleTranslate() && TextContext::drawsUnhinted(matrix.getScaleY());
}

// Unhinted at quarter-pixel positions at a fractional scale; at a whole
// scale hinted as fontconfig asks, on whole pixels when that hints
// horizontally too, as the hinted glyphs of the reference are.
static void hintFor(SkFont& font, bool unhinted)
{
    if (unhinted) {
        font.setHinting(SkFontHinting::kNone);
        font.setSubpixel(true);
        font.setBaselineSnap(true);
        return;
    }
    const SkFontHinting hinting = Application::instance()->text().fontconfigHinting();
    font.setHinting(hinting);
    if (hinting == SkFontHinting::kNormal || hinting == SkFontHinting::kFull)
        font.setSubpixel(false);
}

// The blob with its fonts hinted as hintFor() says; Skia's paragraph
// positions each glyph.
static sk_sp<SkTextBlob> hinted(const sk_sp<SkTextBlob>& blob, bool unhinted, SkCanvas* greyscaleOn = nullptr)
{
    SkTextBlobBuilder builder;
    SkTextBlob::Iter it(*blob);
    SkTextBlob::Iter::ExperimentalRun run;
    while (it.experimentalNext(&run)) {
        if (!run.positions)
            return blob;
        SkFont font = run.font;
        hintFor(font, unhinted);
        if (greyscaleOn)
            greyscaleOnTranslucent(greyscaleOn, font);
        const auto& buffer = builder.allocRunPos(font, run.count);
        std::copy(run.glyphs, run.glyphs + run.count, buffer.glyphs);
        std::copy(run.positions, run.positions + run.count, buffer.points());
    }
    sk_sp<SkTextBlob> made = builder.make();
    return made ? made : blob;
}

namespace {

struct CoverageRun
{
    SkFont font;
    std::vector<SkGlyphID> glyphs;
    std::vector<SkPoint> positions;
};

// What a coverage image was rasterised from: its glyphs, the matrix of the
// canvas they were drawn on and where, its size and the pixel geometry.
struct CoverageEntry
{
    uint64_t hash;
    std::vector<CoverageRun> runs;
    SkM44 matrix;
    SkPoint origin;
    SkISize size;
    SkPixelGeometry geometry;
    sk_sp<SkImage> image;
};

} // namespace

// Coverage of recently drawn runs, most recent first: text painted again as
// it was is blended from the image it had, which on the GPU is also the
// texture it had, instead of being rasterised and uploaded again.
static constexpr size_t kCoverageCacheBytes = 4 << 20;
static constexpr size_t kCoverageCacheEntries = 512;
static std::list<CoverageEntry> s_coverage;
static std::unordered_map<uint64_t, std::list<CoverageEntry>::iterator> s_coverageIndex;
static size_t s_coverageBytes = 0;
// Lookups since the last hit: text that moves by fractions of a pixel every
// frame never hits, and is only sampled once this many have missed.
static constexpr unsigned kCoverageMissesBeforeSampling = 256;
static constexpr unsigned kCoverageSampling = 16;
static unsigned s_coverageMisses = 0;

static uint64_t hashBytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

template<typename T>
static uint64_t hashValue(uint64_t hash, const T& value)
{
    return hashBytes(hash, &value, sizeof(value));
}

// None for a blob with a run that has no positions of its own.
static std::optional<uint64_t> coverageHash(const SkTextBlob& blob, const SkM44& matrix, SkPoint origin, SkISize size,
                                            SkPixelGeometry geometry)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    SkTextBlob::Iter iterator(blob);
    SkTextBlob::Iter::ExperimentalRun run;
    while (iterator.experimentalNext(&run)) {
        if (!run.positions)
            return std::nullopt;
        hash = hashValue(hash, run.font.getTypeface() ? run.font.getTypeface()->uniqueID() : 0);
        hash = hashValue(hash, run.font.getSize());
        hash = hashValue(hash, run.count);
        hash = hashBytes(hash, run.glyphs, size_t(run.count) * sizeof(SkGlyphID));
        hash = hashBytes(hash, run.positions, size_t(run.count) * sizeof(SkPoint));
    }
    SkScalar values[16];
    matrix.getColMajor(values);
    hash = hashBytes(hash, values, sizeof(values));
    hash = hashValue(hash, origin.x());
    hash = hashValue(hash, origin.y());
    hash = hashValue(hash, size.width());
    hash = hashValue(hash, size.height());
    return hashValue(hash, int(geometry));
}

static bool sameRuns(const std::vector<CoverageRun>& runs, const SkTextBlob& blob)
{
    SkTextBlob::Iter iterator(blob);
    SkTextBlob::Iter::ExperimentalRun run;
    size_t i = 0;
    for (; iterator.experimentalNext(&run); ++i) {
        if (i >= runs.size() || !(runs[i].font == run.font) || runs[i].glyphs.size() != size_t(run.count)
            || !std::equal(run.glyphs, run.glyphs + run.count, runs[i].glyphs.begin())
            || std::memcmp(run.positions, runs[i].positions.data(), size_t(run.count) * sizeof(SkPoint)))
            return false;
    }
    return i == runs.size();
}

static std::vector<CoverageRun> coverageRuns(const SkTextBlob& blob)
{
    std::vector<CoverageRun> runs;
    SkTextBlob::Iter iterator(blob);
    SkTextBlob::Iter::ExperimentalRun run;
    while (iterator.experimentalNext(&run)) {
        runs.push_back({run.font, std::vector<SkGlyphID>(run.glyphs, run.glyphs + run.count),
                        std::vector<SkPoint>(run.positions, run.positions + run.count)});
    }
    return runs;
}

static void evictCoverage()
{
    const CoverageEntry& oldest = s_coverage.back();
    s_coverageBytes -= oldest.image->imageInfo().computeMinByteSize();
    s_coverageIndex.erase(oldest.hash);
    s_coverage.pop_back();
}

void clearSubpixelCoverage()
{
    s_coverageMisses = 0;
    s_coverageIndex.clear();
    s_coverage.clear();
    s_coverageBytes = 0;
}

// The run's per-channel coverage, white subpixel text on opaque black with
// the fontconfig's pixel geometry and Skia's linear masks, then blended into
// the canvas at whole device pixels.
static void drawSubpixelBlended(SkCanvas* canvas, const sk_sp<SkTextBlob>& blob, SkPoint origin, SkColor color)
{
    const SkMatrix matrix = canvas->getTotalMatrix();
    SkRect bounds = blob->bounds().makeOffset(origin.x(), origin.y());
    bounds = matrix.mapRect(bounds);
    const SkIRect whole = bounds.roundOut().makeOutset(1, 1);
    SkIRect device = whole;
    if (!device.intersect(canvas->getDeviceClipBounds()))
        return;
    const SkPixelGeometry geometry = Application::instance()->text().pixelGeometry();

    // As translate() and concat() would make it on the coverage canvas.
    SkM44 coverageMatrix;
    if (device.x() || device.y())
        coverageMatrix.preTranslate(SkScalar(-device.x()), SkScalar(-device.y()));
    if (!matrix.isIdentity())
        coverageMatrix.preConcat(SkM44(matrix));

    // Only runs the clip leaves whole: where the image starts decides how
    // the glyph positions round.
    std::optional<uint64_t> hash;
    if (device == whole && int64_t(whole.width()) * whole.height() <= int64_t(kCoverageCacheBytes / 16))
        hash = coverageHash(*blob, coverageMatrix, origin, device.size(), geometry);
    sk_sp<SkImage> image;
    if (hash) {
        const auto found = s_coverageIndex.find(*hash);
        if (found != s_coverageIndex.end()) {
            CoverageEntry& entry = *found->second;
            if (entry.matrix == coverageMatrix && entry.origin == origin && entry.size == device.size()
                && entry.geometry == geometry && sameRuns(entry.runs, *blob)) {
                s_coverage.splice(s_coverage.begin(), s_coverage, found->second);
                image = entry.image;
                s_coverageMisses = 0;
            }
        }
    }

    if (!image) {
        const SkImageInfo info = SkImageInfo::MakeN32(device.width(), device.height(), kOpaque_SkAlphaType);
        const SkSurfaceProps properties(0, geometry, 0, 1);
        // On the CPU on both paths: Ganesh's LCD masks differ from the raster
        // backend's, which is the pixel reference.
        const sk_sp<SkSurface> surface = SkSurfaces::Raster(info, &properties);
        if (!surface)
            return;
        SkCanvas* coverage = surface->getCanvas();
        coverage->clear(SK_ColorBLACK);
        coverage->setMatrix(coverageMatrix);
        SkPaint white;
        white.setAntiAlias(true);
        white.setColor(SK_ColorWHITE);
        coverage->drawTextBlob(blob, origin.x(), origin.y(), white);
        image = surface->makeImageSnapshot();
        if (hash)
            ++s_coverageMisses;
        if (hash && image
            && (s_coverageMisses < kCoverageMissesBeforeSampling || s_coverageMisses % kCoverageSampling == 0)) {
            const auto found = s_coverageIndex.find(*hash);
            if (found != s_coverageIndex.end()) {
                s_coverageBytes -= found->second->image->imageInfo().computeMinByteSize();
                s_coverage.erase(found->second);
                s_coverageIndex.erase(found);
            }
            s_coverage.push_front({*hash, coverageRuns(*blob), coverageMatrix, origin, device.size(), geometry, image});
            s_coverageIndex[*hash] = s_coverage.begin();
            s_coverageBytes += image->imageInfo().computeMinByteSize();
            while (s_coverage.size() > 1
                   && (s_coverageBytes > kCoverageCacheBytes || s_coverage.size() > kCoverageCacheEntries))
                evictCoverage();
        }
    }

    sk_sp<SkBlender> blender;
    SkPaint paint;
    paint.setBlender(subpixelBlender(color, &blender));
    if (!paint.getBlender())
        return;
    canvas->save();
    canvas->resetMatrix();
    canvas->drawImage(image, SkScalar(device.x()), SkScalar(device.y()), SkSamplingOptions(), &paint);
    canvas->restore();
}

namespace {

// Skia's own paragraph painting, with its glyphs drawn by the subpixel blend
// above when the target is translucent.
class SubpixelBlendPainter final : public textlayout::CanvasParagraphPainter
{
public:
    explicit SubpixelBlendPainter(SkCanvas* canvas)
        : CanvasParagraphPainter(canvas)
        , m_canvas(canvas)
    {
    }

    void drawTextBlob(const sk_sp<SkTextBlob>& original, SkScalar x, SkScalar y, const SkPaintOrID& paint) override
    {
        const SkPaint* plain = std::get_if<SkPaint>(&paint);
        if (plain && drawsSubpixelBlended(m_canvas, Application::instance()->text().edging())) {
            drawSubpixelBlended(m_canvas, hinted(original, drawsUnhinted(m_canvas)), SkPoint::Make(x, y),
                                plain->getColor());
            return;
        }
        CanvasParagraphPainter::drawTextBlob(hinted(original, drawsUnhinted(m_canvas), m_canvas), x, y, paint);
    }

private:
    SkCanvas* m_canvas;
};

} // namespace

void paintParagraph(SkCanvas* canvas, textlayout::Paragraph* paragraph, double x, double y)
{
    SubpixelBlendPainter painter(canvas);
    paragraph->paint(&painter, SkScalar(x), SkScalar(y));
}

void paintTextLines(SkCanvas* canvas, textlayout::Paragraph* paragraph, const std::vector<TextLineBox>& lines,
                    PointF offset, double width, double align, const AdvanceRule& roundAdvances,
                    const std::function<SkColor(size_t utf8Offset)>& color, bool subpixel)
{
    paintTextLines(canvas, LineGlyphs{glyphLines(paragraph, roundAdvances)}, lines, offset, width, align, color,
                   subpixel);
}

void paintTextLines(SkCanvas* canvas, const LineGlyphs& lineGlyphs, const std::vector<TextLineBox>& lines,
                    PointF offset, double width, double align, const std::function<SkColor(size_t utf8Offset)>& color,
                    bool subpixel)
{
    const std::vector<GlyphLine>& glyphs = lineGlyphs.lines;
    const bool unhinted = drawsUnhinted(canvas);
    // The clip's rows in logical pixels: lines whose glyphs cannot reach it
    // are skipped, with two line heights of room for ink past the box.
    double clipTop = std::numeric_limits<double>::lowest();
    double clipBottom = std::numeric_limits<double>::max();
    const SkMatrix& total = canvas->getTotalMatrix();
    if (total.isScaleTranslate() && total.getScaleY() > 0) {
        const SkIRect clip = canvas->getDeviceClipBounds();
        clipTop = (clip.top() - total.getTranslateY()) / total.getScaleY();
        clipBottom = (clip.bottom() - total.getTranslateY()) / total.getScaleY();
    }
    for (size_t l = 0; l < glyphs.size() && l < lines.size(); ++l) {
        const GlyphLine& line = glyphs[l];
        if (line.runs.empty())
            continue;
        const double room = 2 * lines[l].height;
        if (offset.y + lines[l].top + lines[l].height + room < clipTop || offset.y + lines[l].top - room > clipBottom)
            continue;
        // Aligned by the line's advance, without the overhang.
        double pen = line.runs.front().x.front() + (width - line.advance) * align;
        double logicalBaseline = offset.y + lines[l].top + lines[l].ascent;
        const SkMatrix& matrix = canvas->getTotalMatrix();
        if (matrix.isScaleTranslate() && matrix.getScaleY() > 0) {
            // On the nearest device row, a half to the row below as Qt rounds
            // it; float noise in the mapping must not flip a half upwards.
            const double device = logicalBaseline * matrix.getScaleY() + matrix.getTranslateY();
            logicalBaseline = (std::floor(device + 0.5 + 1e-3) - matrix.getTranslateY()) / matrix.getScaleY();
        }
        const SkScalar baseline = SkScalar(logicalBaseline);
        for (size_t r = 0; r < line.runs.size(); ++r) {
            const GlyphRun& run = line.runs[r];
            const double runStart = pen;
            SkFont font = run.font;
            if (!subpixel && font.getEdging() == SkFont::Edging::kSubpixelAntiAlias)
                font.setEdging(SkFont::Edging::kAntiAlias);
            hintFor(font, unhinted);
            if (!drawsSubpixelBlended(canvas, font.getEdging()))
                greyscaleOnTranslucent(canvas, font);
            // Each glyph's pen position from the run's start.
            std::vector<SkScalar> positions(run.glyphs.size());
            for (size_t i = 0; i < run.glyphs.size(); ++i) {
                positions[i] = SkScalar(pen - runStart);
                pen += line.advances[r][i];
            }
            // Runs end where the paragraph cuts them, as at an ellipsis: the
            // glyphs it places before its clip, whole. A clip in pixels would
            // cut the rounded advances, wider than the paragraph's at 1.0.
            size_t visible = 0;
            while (visible < run.glyphs.size() && run.x[visible] < run.clipRight - 1e-3)
                ++visible;
            // A run is one font; its colours may change within it, span by span.
            size_t begin = 0;
            while (begin < visible) {
                const SkColor segmentColor = color(run.clusters.empty() ? run.utf8Offset : run.clusters[begin]);
                size_t end = begin + 1;
                while (end < visible && !run.clusters.empty() && color(run.clusters[end]) == segmentColor)
                    ++end;
                SkTextBlobBuilder builder;
                const auto& buffer = builder.allocRunPos(font, int(end - begin));
                std::copy(run.glyphs.begin() + long(begin), run.glyphs.begin() + long(end), buffer.glyphs);
                SkPoint* points = buffer.points();
                for (size_t i = begin; i < end; ++i)
                    points[i - begin] = {positions[i], 0};
                begin = end;
                const sk_sp<SkTextBlob> blob = builder.make();
                if (!blob)
                    continue;
                SkPaint paint;
                paint.setAntiAlias(true);
                paint.setColor(segmentColor);
                if (drawsSubpixelBlended(canvas, font.getEdging()))
                    drawSubpixelBlended(canvas, blob, SkPoint::Make(SkScalar(offset.x + runStart), baseline),
                                   paint.getColor());
                else
                    canvas->drawTextBlob(blob, SkScalar(offset.x + runStart), baseline, paint);
            }
        }
    }
}

void drawTextBlobForTarget(SkCanvas* canvas, const sk_sp<SkTextBlob>& blob, float x, float y, const SkPaint& paint)
{
    bool subpixel = false;
    SkTextBlob::Iter::ExperimentalRun run;
    for (SkTextBlob::Iter it(*blob); it.experimentalNext(&run);)
        subpixel = subpixel || run.font.getEdging() == SkFont::Edging::kSubpixelAntiAlias;
    if (!subpixel || !translucentTarget(canvas)) {
        canvas->drawTextBlob(blob, x, y, paint);
        return;
    }
    if (drawsSubpixelBlended(canvas, SkFont::Edging::kSubpixelAntiAlias)) {
        drawSubpixelBlended(canvas, blob, SkPoint::Make(x, y), paint.getColor());
        return;
    }
    SkTextBlobBuilder builder;
    for (SkTextBlob::Iter it(*blob); it.experimentalNext(&run);) {
        if (!run.positions) {
            canvas->drawTextBlob(blob, x, y, paint);
            return;
        }
        SkFont font = run.font;
        greyscaleOnTranslucent(canvas, font);
        const auto& buffer = builder.allocRunPos(font, run.count);
        std::copy(run.glyphs, run.glyphs + run.count, buffer.glyphs);
        std::copy(run.positions, run.positions + run.count, buffer.points());
    }
    if (const sk_sp<SkTextBlob> grey = builder.make())
        canvas->drawTextBlob(grey, x, y, paint);
}

namespace {

// UTF-16 offsets into UTF-8 ones, walking on from the last offset asked for
// while they grow, so a paragraph's lines cost one pass over the text.
class Utf8Offsets
{
public:
    explicit Utf8Offsets(const std::string& text)
        : m_text(text)
    {
    }

    size_t operator()(size_t utf16)
    {
        if (utf16 < m_units) {
            m_index = 0;
            m_units = 0;
        }
        for (; m_index < m_text.size(); ++m_index) {
            const auto byte = static_cast<unsigned char>(m_text[m_index]);
            if ((byte & 0xc0) == 0x80)
                continue;
            if (m_units >= utf16)
                break;
            m_units += byte >= 0xf0 ? 2 : 1;
        }
        return m_index;
    }

private:
    const std::string& m_text;
    size_t m_index = 0;
    size_t m_units = 0;
};

} // namespace

TextGeometry textGeometry(textlayout::Paragraph* paragraph, const std::string& text, double width, double align,
                          const AdvanceRule& roundAdvances)
{
    return textGeometry(paragraph, LineGlyphs{glyphLines(paragraph, roundAdvances)}, text, width, align);
}

TextGeometry textGeometry(textlayout::Paragraph* paragraph, const LineGlyphs& lineGlyphs, const std::string& text,
                          double width, double align)
{
    TextGeometry geometry;
    std::vector<textlayout::LineMetrics> metrics;
    paragraph->getLineMetrics(metrics);
    const std::vector<GlyphLine>& glyphs = lineGlyphs.lines;
    Utf8Offsets starts(text);
    Utf8Offsets ends(text);
    for (size_t l = 0; l < metrics.size(); ++l) {
        TextGeometry::Line line;
        line.start = starts(metrics[l].fStartIndex);
        line.end = ends(metrics[l].fEndIndex);
        // A line's start excludes the spaces a soft break left; keep them.
        if (l > 0 && geometry.lines.back().end < text.size() && text[geometry.lines.back().end] != '\n')
            line.start = std::min(line.start, geometry.lines.back().end);
        double pen = 0;
        if (l < glyphs.size() && !glyphs[l].runs.empty()) {
            const GlyphLine& glyphLine = glyphs[l];
            pen = glyphLine.runs.front().x.front() + (width - glyphLine.advance) * align;
            for (size_t r = 0; r < glyphLine.runs.size(); ++r) {
                const GlyphRun& run = glyphLine.runs[r];
                for (size_t i = 0; i < run.glyphs.size(); ++i) {
                    if (line.stops.empty() || line.stops.back().first < run.clusters[i])
                        line.stops.emplace_back(run.clusters[i], pen);
                    pen += glyphLine.advances[r][i];
                }
            }
        } else {
            pen = width * align;
        }
        // Spaces at a soft break have no glyphs; they end where the text does.
        if (line.stops.empty() || line.stops.back().first < line.end)
            line.stops.emplace_back(line.end, pen);
        if (line.stops.front().first > line.start)
            line.stops.insert(line.stops.begin(), {line.start, line.stops.front().second});
        geometry.lines.push_back(std::move(line));
    }
    if (geometry.lines.empty())
        geometry.lines.push_back({0, 0, {{0, width * align}}});
    return geometry;
}

size_t TextGeometry::lineOf(size_t offset) const
{
    for (size_t l = 0; l < lines.size(); ++l) {
        // At a soft break the caret goes to the next line's start.
        if (offset < lines[l].end || (offset == lines[l].end && (l + 1 == lines.size() || lines[l + 1].start > offset)))
            return l;
    }
    return lines.empty() ? 0 : lines.size() - 1;
}

double TextGeometry::x(size_t offset) const
{
    const Line& line = lines[lineOf(offset)];
    double result = line.stops.front().second;
    for (const auto& [stop, x] : line.stops) {
        if (stop > offset)
            break;
        result = x;
    }
    return result;
}

size_t TextGeometry::offsetAt(size_t index, double x) const
{
    const Line& line = lines[std::min(index, lines.size() - 1)];
    const auto& stops = line.stops;
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        if (x < (stops[i].second + stops[i + 1].second) / 2)
            return stops[i].first;
    }
    return stops.back().first;
}

RectF tightBoundingRect(textlayout::Paragraph* paragraph, double pixelSize, double scale, bool hintedLayout)
{
    const bool hinted = Application::instance()->text().hintsGlyphMetrics(scale, hintedLayout);
    double left = std::numeric_limits<double>::max();
    double top = std::numeric_limits<double>::max();
    double right = std::numeric_limits<double>::lowest();
    double bottom = std::numeric_limits<double>::lowest();
    paragraph->visit([&](int lineNumber, const textlayout::Paragraph::VisitorInfo* info) {
        if (lineNumber != 0 || !info)
            return;
        // The glyph's box on whole pixels at the pixel size, hinted only
        // when metrics are hinted; the pen follows the shaped
        // advances, in 26.6.
        SkFont font = info->font;
        font.setSize(SkScalar(pixelSize));
        font.setSubpixel(false);
        font.setHinting(hinted ? SkFontHinting::kFull : SkFontHinting::kNone);
        font.setLinearMetrics(!hinted);
        std::vector<SkRect> bounds(size_t(info->count));
        font.getBounds({info->glyphs, size_t(info->count)}, {bounds.data(), bounds.size()}, nullptr);
        for (int i = 0; i < info->count; ++i) {
            const SkRect& box = bounds[size_t(i)];
            if (box.isEmpty())
                continue;
            const double pen = std::floor((info->origin.fX + info->positions[i].fX) * 64) / 64;
            const double x = pen + std::floor(box.left());
            const double width = std::ceil(box.right()) - std::floor(box.left());
            const double y = std::floor(box.top());
            const double height = std::ceil(box.bottom()) - std::floor(box.top());
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, std::ceil(x) + width);
            bottom = std::max(bottom, y + height);
        }
    });
    if (left > right)
        return {};
    return {left, top, right - left, bottom - top};
}

RectF inkBoundingRect(textlayout::Paragraph* paragraph, double pixelSize, double scale, bool hintedLayout)
{
    const bool hinted = Application::instance()->text().hintsGlyphMetrics(scale, hintedLayout);
    double left = std::numeric_limits<double>::max();
    double top = std::numeric_limits<double>::max();
    double right = std::numeric_limits<double>::lowest();
    double bottom = std::numeric_limits<double>::lowest();
    paragraph->visit([&](int lineNumber, const textlayout::Paragraph::VisitorInfo* info) {
        if (lineNumber != 0 || !info)
            return;
        SkFont font = info->font;
        font.setSize(SkScalar(pixelSize * scale));
        font.setSubpixel(!hinted);
        font.setHinting(hinted ? SkFontHinting::kFull : SkFontHinting::kNone);
        font.setLinearMetrics(!hinted);
        std::vector<SkRect> bounds(size_t(info->count));
        font.getBounds({info->glyphs, size_t(info->count)}, {bounds.data(), bounds.size()}, nullptr);
        for (int i = 0; i < info->count; ++i) {
            const SkRect& box = bounds[size_t(i)];
            if (box.isEmpty())
                continue;
            const double pen = std::floor((info->origin.fX + info->positions[i].fX) * 64) / 64;
            left = std::min(left, pen + box.left() / scale);
            top = std::min(top, double(box.top()) / scale);
            right = std::max(right, pen + box.right() / scale);
            bottom = std::max(bottom, double(box.bottom()) / scale);
        }
    });
    if (left > right)
        return {};
    return {left, top, right - left, bottom - top};
}

} // namespace cutefish::ui
