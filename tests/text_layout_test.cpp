// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "ui/painter.h"
#include "ui/text_layout.h"
#include "ui/view.h"
#include "ui/window.h"

using namespace cutefish::ui;

namespace {

const FontSpec kMono{"Noto Mono", 10, false, false};

bool monoInstalled()
{
    return FontMetrics(kMono).family() == "Noto Mono";
}

struct Laid
{
    size_t start;
    size_t length;
    double width;

    bool operator==(const Laid&) const = default;
};

std::vector<Laid> layOut(TextLayout& layout, double width)
{
    layout.setWrap(width > 0 ? TextLayout::Wrap::WordOrAnywhere : TextLayout::Wrap::None);
    layout.beginLayout();
    while (layout.createLine(width > 0 ? width : 100000, {0, 0}) >= 0) {
    }
    layout.endLayout();
    std::vector<Laid> lines;
    for (int i = 0; i < layout.lineCount(); ++i) {
        const TextLayout::Line& line = layout.lineAt(i);
        lines.push_back({line.textStart, line.textLength, line.naturalTextWidth});
    }
    return lines;
}

} // namespace

// Reference line breaks and widths for the same text, font and widths
// (word or anywhere wrapping, trailing spaces counted), measured on the
// laptop with hinted metrics at scale 1.
TEST(TextLayoutTest, BreaksMatchReference)
{
    if (!monoInstalled())
        GTEST_SKIP() << "Noto Mono is not installed";
    TextLayout code("int main(int argc, char** argv) { return 0; }", kMono);
    EXPECT_EQ(layOut(code, 120), (std::vector<Laid>{{0, 13, 104}, {13, 13, 104}, {26, 15, 120}, {41, 4, 32}}));
    TextLayout word("a_very_long_identifier_without_any_break_opportunity_at_all", kMono);
    EXPECT_EQ(layOut(word, 100), (std::vector<Laid>{{0, 12, 96}, {12, 12, 96}, {24, 12, 96}, {36, 12, 96}, {48, 11, 88}}));
    TextLayout spaced("word1 word2   word3 word4 word5 word6", {"Noto Mono", 12, false, false});
    EXPECT_EQ(layOut(spaced, 70), (std::vector<Laid>{{0, 6, 60}, {6, 8, 80}, {14, 6, 60}, {20, 6, 60}, {26, 6, 60}, {32, 5, 50}}));
    TextLayout empty("", kMono);
    EXPECT_EQ(layOut(empty, 0), (std::vector<Laid>{{0, 0, 0}}));
}

TEST(TextLayoutTest, TabsAdvanceToStopsFromTheLineStart)
{
    if (!monoInstalled())
        GTEST_SKIP() << "Noto Mono is not installed";
    TextLayout layout("\tif (x)\t{ y = 1; }   ", kMono);
    EXPECT_EQ(layOut(layout, 0), (std::vector<Laid>{{0, 21, 232}}));
    // Eight spaces of 8 px per stop.
    EXPECT_DOUBLE_EQ(layout.cursorToX(0, 1), 64);
    EXPECT_DOUBLE_EQ(layout.cursorToX(0, 7), 112);
    EXPECT_DOUBLE_EQ(layout.cursorToX(0, 8), 128);
    EXPECT_DOUBLE_EQ(layout.cursorToX(0, 21), 232);
    // Hits go to the nearest boundary; past the end, the line's end.
    EXPECT_EQ(layout.xToCursor(0, 66), 1u);
    EXPECT_EQ(layout.xToCursor(0, 125), 8u);
    EXPECT_EQ(layout.xToCursor(0, 1000), 21u);
    EXPECT_EQ(layout.xToCursor(0, -5), 0u);
}

TEST(TextLayoutTest, CaretSnapsToGraphemes)
{
    if (!monoInstalled())
        GTEST_SKIP() << "Noto Mono is not installed";
    // e and a combining acute accent are one grapheme.
    TextLayout layout("e\xCC\x81t", kMono);
    layOut(layout, 0);
    EXPECT_DOUBLE_EQ(layout.cursorToX(0, 1), 8);
    EXPECT_DOUBLE_EQ(layout.cursorToX(0, 3), 8);
    EXPECT_EQ(layout.xToCursor(0, 5), 3u);
}

TEST(TextLayoutTest, LinesHaveTheirOwnWidthsAndPositions)
{
    if (!monoInstalled())
        GTEST_SKIP() << "Noto Mono is not installed";
    TextLayout layout("    hanging indent for wrapped lines of code", kMono);
    layout.setWrap(TextLayout::Wrap::WordOrAnywhere);
    layout.beginLayout();
    layout.createLine(200, {0, 0});
    // A hanging indent: the rest start four spaces in, narrower.
    while (layout.createLine(168, {32, 20.0 * layout.lineCount()}) >= 0) {
    }
    layout.endLayout();
    ASSERT_GE(layout.lineCount(), 2);
    EXPECT_DOUBLE_EQ(layout.lineAt(1).position.x, 32);
    EXPECT_LE(layout.lineAt(1).naturalTextWidth, 168 + 8 * 4);
    EXPECT_DOUBLE_EQ(layout.cursorToX(1, layout.lineAt(1).textStart), 32);
    EXPECT_EQ(layout.lineForTextPosition(layout.lineAt(1).textStart), 1);
    EXPECT_EQ(layout.lineForTextPosition(layout.text().size()), layout.lineCount() - 1);
}

namespace {

class Canvas : public View
{
public:
    explicit Canvas(std::function<void(Painter&)> draw)
        : m_draw(std::move(draw))
    {
    }

protected:
    void paint(Painter& painter) override { m_draw(painter); }

private:
    std::function<void(Painter&)> m_draw;
};

} // namespace

TEST(TextLayoutTest, DrawsFormatsInTheirColours)
{
    TextLayout layout("red green", kMono);
    layout.setDefaultColor(Color::rgb(0x000000));
    layout.setFormats({{0, 3, Color::rgb(0xFF0000)}, {4, 5, Color::rgb(0x00A000), false, false, true}});
    layOut(layout, 0);
    Window window;
    window.resize({100, 30});
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    window.setContentView(std::make_unique<Canvas>([&](Painter& painter) { layout.draw(painter, {2, 4}); }));
    window.show();
    const Image image = window.grab();
    bool red = false;
    bool green = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const Color c = image.pixel(x, y);
            red = red || (c.r > 200 && c.g < 80 && c.b < 80);
            green = green || (c.g > 120 && c.r < 80 && c.b < 80);
        }
    }
    EXPECT_TRUE(red);
    EXPECT_TRUE(green);
}

// A heart asked for as emoji (U+2764 U+FE0F) comes from the colour emoji
// font, though text fonts have a plain black heart.
TEST(TextLayoutTest, VariationSelectorAsksForEmoji)
{
    // Also where the character follows text the text font draws.
    TextLayout layout("a\u2764\uFE0Fb", {"Inter", 24, false, false});
    layout.setDefaultColor(Color::rgb(0x000000));
    layOut(layout, 0);
    Window window;
    window.resize({100, 40});
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    window.setContentView(std::make_unique<Canvas>([&](Painter& painter) { layout.draw(painter, {2, 4}); }));
    window.show();
    const Image image = window.grab();
    bool red = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const Color c = image.pixel(x, y);
            red = red || (c.r > 180 && c.g < 80 && c.b < 80);
        }
    }
    EXPECT_TRUE(red);
}

TEST(TextLayoutTest, CodeLinesLayOutQuickly)
{
    std::string line = "    for (std::size_t index = 0; index < values.size(); ++index) { total += values[index]; } // sum";
    TextLayout warm(line, kMono);
    layOut(warm, 0);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) {
        TextLayout layout(line, kMono);
        layout.setTabStopDistance(32);
        layOut(layout, 0);
    }
    const double each = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 200;
    printf("one %zu-byte code line: %.1f us\n", line.size(), each);
    std::string segment;
    while (segment.size() < 1024)
        segment += "lorem ipsum dolor sit amet, ";
    segment.resize(1024);
    const auto segmentStart = std::chrono::steady_clock::now();
    TextLayout layout(segment, kMono);
    layOut(layout, 600);
    const double segmentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - segmentStart).count();
    printf("one 1024-byte segment wrapped: %.2f ms (%d lines)\n", segmentMs, layout.lineCount());
    EXPECT_LT(each, 500);
}

TEST(TextLayoutTest, SystemFixedFontFollowsTheTheme)
{
    const FontSpec font = systemFixedFont();
    EXPECT_FALSE(font.family.empty());
    EXPECT_GT(font.pointSize, 0);
}

// A line takes the largest ascent and descent of the fonts it uses: CJK
// through fallback sits lower under Noto Mono.
TEST(TextLayoutTest, LinesTakeTheTallestFontsMetrics)
{
    if (!monoInstalled())
        GTEST_SKIP() << "Noto Mono is not installed";
    for (const double scale : {1.0, 1.5}) {
        const FontMetrics metrics(kMono, scale);
        TextLayout latin("int main()", kMono, scale);
        layOut(latin, 0);
        EXPECT_DOUBLE_EQ(latin.lineAt(0).ascent, metrics.ascentF()) << scale;
        EXPECT_DOUBLE_EQ(latin.lineAt(0).descent, metrics.descentF()) << scale;

        TextLayout mixed("int \u4E2D\u6587 main()\nint main()", kMono, scale);
        mixed.setWrap(TextLayout::Wrap::WordOrAnywhere);
        mixed.beginLayout();
        mixed.createLine(80, {0, 0});
        mixed.createLine(1000, {0, 20});
        mixed.endLayout();
        EXPECT_GE(mixed.lineAt(0).ascent, metrics.ascentF()) << scale;
        EXPECT_GE(mixed.lineAt(0).descent, metrics.descentF()) << scale;
        EXPECT_GT(mixed.lineAt(0).ascent + mixed.lineAt(0).descent, metrics.heightF()) << scale;
        // The reference for the same text at scale 1: 15 and 4.
        if (scale == 1) {
            EXPECT_DOUBLE_EQ(mixed.lineAt(0).ascent, 15);
            EXPECT_DOUBLE_EQ(mixed.lineAt(0).descent, 4);
        }
        EXPECT_DOUBLE_EQ(mixed.lineAt(1).ascent, metrics.ascentF()) << scale;
    }
}
