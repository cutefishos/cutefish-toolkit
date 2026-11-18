// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <unistd.h>

#include "ui/font_file.h"
#include "ui/painter.h"
#include "ui/image.h"
#include "ui/view.h"
#include "ui/window.h"

using namespace cutefish::ui;
namespace fs = std::filesystem;

namespace {

const char kRegular[] = "/usr/share/fonts/opentype/inter/Inter-Regular.otf";
const char kBold[] = "/usr/share/fonts/opentype/inter/Inter-Bold.otf";
const char kBoldItalic[] = "/usr/share/fonts/opentype/inter/Inter-BoldItalic.otf";
const char kCollection[] = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";

// One per process: ctest runs the tests of this suite side by side.
const fs::path kDirectory = "/tmp/cutefish-toolkit-font-file-test-" + std::to_string(getpid());

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

Image render(std::function<void(Painter&)> draw)
{
    Window window;
    window.setScale(1);
    window.resize({200, 100});
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    window.setContentView(std::make_unique<Canvas>(std::move(draw)));
    window.show();
    return window.grab();
}

int inkedPixels(const Image& image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            count += image.pixel(x, y).g < 128;
    }
    return count;
}

bool samePixels(const Image& a, const Image& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y).argb() != b.pixel(x, y).argb())
                return false;
        }
    }
    return true;
}

} // namespace

#define REQUIRE_FONT(path) \
    if (!fs::exists(path)) \
        GTEST_SKIP() << path << " is not installed"

TEST(FontFileTest, ReadsTheFaceOfAFont)
{
    REQUIRE_FONT(kRegular);
    const std::vector<FontFace> faces = readFontFaces(kRegular);
    ASSERT_EQ(faces.size(), 1u);
    const FontFace& face = faces.front();
    EXPECT_EQ(face.index, 0);
    EXPECT_EQ(face.family, "Inter");
    EXPECT_EQ(face.style, "Regular");
    EXPECT_EQ(face.postScriptName, "Inter-Regular");
    EXPECT_GT(face.revision, 0);
    EXPECT_EQ(face.weight, 400);
    EXPECT_FALSE(face.italic);
    EXPECT_TRUE(face.covers(U'A'));
    EXPECT_TRUE(face.covers("Ag 0"));
    EXPECT_FALSE(face.covers(char32_t(0x4E00)));
    EXPECT_FALSE(face.covers("A\u4E00"));
    EXPECT_EQ(face.characters(U'A', U'Z', 3), "ABC");
    EXPECT_EQ(face.characters(U'X', U'Z', 10), "XYZ");
    EXPECT_TRUE(face.characters(0x4E00, 0x9FFF, 4).empty());
}

TEST(FontFileTest, ReadsWeightAndSlant)
{
    REQUIRE_FONT(kBoldItalic);
    const std::vector<FontFace> faces = readFontFaces(kBoldItalic);
    ASSERT_EQ(faces.size(), 1u);
    EXPECT_EQ(faces.front().weight, 700);
    EXPECT_TRUE(faces.front().italic);
}

TEST(FontFileTest, ReadsEveryFaceOfACollection)
{
    REQUIRE_FONT(kCollection);
    const std::vector<FontFace> faces = readFontFaces(kCollection);
    ASSERT_GT(faces.size(), 1u);
    std::set<int> indexes;
    std::set<std::string> names;
    for (const FontFace& face : faces) {
        indexes.insert(face.index);
        names.insert(face.postScriptName);
        EXPECT_FALSE(face.family.empty());
        EXPECT_TRUE(face.covers(char32_t(0x4E00)));
    }
    EXPECT_EQ(indexes.size(), faces.size());
    EXPECT_EQ(names.size(), faces.size());
    EXPECT_EQ(*indexes.begin(), 0);
    EXPECT_EQ(*indexes.rbegin(), int(faces.size()) - 1);
    // Two characters of three bytes each.
    EXPECT_EQ(faces.front().characters(0x4E00, 0x9FFF, 2).size(), 6u);
}

TEST(FontFileTest, OtherFilesHaveNoFaces)
{
    fs::create_directories(kDirectory);
    const std::string text = (kDirectory / "notes.ttf").string();
    std::ofstream(text) << "Not a font.\n";
    EXPECT_TRUE(readFontFaces(text).empty());
    EXPECT_TRUE(readFontFaces((kDirectory / "gone.ttf").string()).empty());
    fs::remove_all(kDirectory);
}

TEST(FontFileTest, TheDefaultFaceIsTheRegularUprightOne)
{
    const auto face = [](int weight, bool italic) {
        FontFace result;
        result.weight = weight;
        result.italic = italic;
        return result;
    };
    EXPECT_EQ(defaultFontFace({}), 0);
    EXPECT_EQ(defaultFontFace({face(700, false), face(400, true), face(400, false), face(300, false)}), 2);
    EXPECT_EQ(defaultFontFace({face(900, false), face(400, true), face(600, false)}), 2);
    EXPECT_EQ(defaultFontFace({face(700, true), face(300, true)}), 1);
}

TEST(FontFileTest, FindsTheInstalledFilesOfAFace)
{
    REQUIRE_FONT(kRegular);
    const FontFace face = readFontFaces(kRegular).front();
    const std::vector<std::string> files = installedFontFiles(face);
    EXPECT_NE(std::find(files.begin(), files.end(), kRegular), files.end());

    // A copy elsewhere is the same font, and is not installed itself.
    fs::create_directories(kDirectory);
    const std::string copy = (kDirectory / "copy.otf").string();
    fs::copy_file(kRegular, copy, fs::copy_options::overwrite_existing);
    const std::vector<FontFace> copied = readFontFaces(copy);
    ASSERT_EQ(copied.size(), 1u);
    EXPECT_EQ(installedFontFiles(copied.front()), files);
    fs::remove_all(kDirectory);

    FontFace other = face;
    other.revision += 1;
    EXPECT_TRUE(installedFontFiles(other).empty());
    other = face;
    other.postScriptName = "NoSuchFont-Regular";
    EXPECT_TRUE(installedFontFiles(other).empty());
}

TEST(FontFileTest, SamplesMeasureAndDrawInTheFace)
{
    REQUIRE_FONT(kRegular);
    REQUIRE_FONT(kBold);
    const FontSample regular(kRegular, 0);
    const FontSample bold(kBold, 0);
    ASSERT_TRUE(regular.isValid());
    ASSERT_TRUE(bold.isValid());

    const SizeF small = regular.measure("Sample", 20);
    const SizeF large = regular.measure("Sample", 40);
    EXPECT_GT(small.width, 0);
    EXPECT_GT(small.height, 20);
    EXPECT_GT(large.width, small.width * 1.8);
    EXPECT_GT(regular.measure("Sample text", 20).width, small.width);
    EXPECT_GT(bold.measure("Sample", 40).width, large.width);
    EXPECT_EQ(regular.measure("", 20).width, 0);

    const Image light = render([&](Painter& painter) { regular.draw(painter, "HHH", 40, {10, 10}, Color::rgb(0)); });
    const Image heavy = render([&](Painter& painter) { bold.draw(painter, "HHH", 40, {10, 10}, Color::rgb(0)); });
    EXPECT_GT(inkedPixels(light), 100);
    EXPECT_GT(inkedPixels(heavy), inkedPixels(light));
}

TEST(FontFileTest, SamplesNeverFallBackToAnotherFont)
{
    REQUIRE_FONT(kRegular);
    REQUIRE_FONT(kCollection);
    const FontSample sample(kRegular, 0);
    ASSERT_TRUE(sample.isValid());
    // Two characters the face lacks and an installed font has: both show the
    // face's one missing glyph.
    const std::string first = "\u4E00";
    const std::string second = "\u9F8D";
    EXPECT_EQ(sample.measure(first, 40).width, sample.measure(second, 40).width);
    const Image a = render([&](Painter& painter) { sample.draw(painter, first, 40, {10, 10}, Color::rgb(0)); });
    const Image b = render([&](Painter& painter) { sample.draw(painter, second, 40, {10, 10}, Color::rgb(0)); });
    EXPECT_TRUE(samePixels(a, b));
}

TEST(FontFileTest, SamplesLoadAFaceOfACollection)
{
    REQUIRE_FONT(kCollection);
    const std::vector<FontFace> faces = readFontFaces(kCollection);
    ASSERT_FALSE(faces.empty());
    const FontSample sample(kCollection, faces.back().index);
    ASSERT_TRUE(sample.isValid());
    const std::string text = faces.back().characters(0x4E00, 0x9FFF, 2);
    const Image image = render([&](Painter& painter) { sample.draw(painter, text, 40, {10, 10}, Color::rgb(0)); });
    EXPECT_GT(inkedPixels(image), 50);
}

TEST(FontFileTest, AFileThatIsNoFontGivesNoSample)
{
    const FontSample sample("/nonexistent/font.ttf", 0);
    EXPECT_FALSE(sample.isValid());
    EXPECT_EQ(sample.measure("Sample", 20).width, 0);
    const Image image = render([&](Painter& painter) { sample.draw(painter, "Sample", 20, {10, 10}, Color::rgb(0)); });
    EXPECT_EQ(inkedPixels(image), 0);
}

// The rows and columns with ink, as {left, top, right, bottom}; right and
// bottom are the last ones.
static std::array<int, 4> inkBounds(const Bitmap& bitmap)
{
    std::array<int, 4> bounds {bitmap.width(), bitmap.height(), -1, -1};
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            if (bitmap.alpha(x, y) < 128)
                continue;
            bounds = {std::min(bounds[0], x), std::min(bounds[1], y), std::max(bounds[2], x), std::max(bounds[3], y)};
        }
    }
    return bounds;
}

TEST(FontFileTest, GlyphsRenderOnABitmapCutToTheirInk)
{
    REQUIRE_FONT(kRegular);
    const Bitmap small = renderFontGlyphs(kRegular, 0, "Ag", 50, Color::rgb(0x102030));
    const Bitmap large = renderFontGlyphs(kRegular, 0, "Ag", 100, Color::rgb(0x102030));
    ASSERT_FALSE(small.isNull());
    ASSERT_FALSE(large.isNull());
    EXPECT_NEAR(large.width(), small.width() * 2, 3);
    EXPECT_NEAR(large.height(), small.height() * 2, 3);
    // Cap height and the descender of the g: about the size.
    EXPECT_GT(large.height(), 80);
    EXPECT_LT(large.height(), 110);

    // The ink reaches every edge.
    const std::array<int, 4> ink = inkBounds(large);
    EXPECT_LE(ink[0], 1);
    EXPECT_LE(ink[1], 1);
    EXPECT_GE(ink[2], large.width() - 2);
    EXPECT_GE(ink[3], large.height() - 2);

    // The stem of the g, in the colour.
    bool solid = false;
    for (int x = large.width() / 2; x < large.width() && !solid; ++x)
        solid = large.pixel(x, large.height() / 2) == 0xFF102030;
    EXPECT_TRUE(solid);
}

TEST(FontFileTest, GlyphsOfNoFontRenderNothing)
{
    EXPECT_TRUE(renderFontGlyphs("/nonexistent/font.ttf", 0, "Ag", 50, Color::rgb(0)).isNull());
    REQUIRE_FONT(kRegular);
    EXPECT_TRUE(renderFontGlyphs(kRegular, 0, "", 50, Color::rgb(0)).isNull());
    EXPECT_TRUE(renderFontGlyphs(kRegular, 0, " ", 50, Color::rgb(0)).isNull());
}

TEST(FontFileTest, GlyphsRenderFromAFaceOfACollection)
{
    REQUIRE_FONT(kCollection);
    const std::vector<FontFace> faces = readFontFaces(kCollection);
    ASSERT_FALSE(faces.empty());
    const Bitmap bitmap = renderFontGlyphs(kCollection, faces.back().index, faces.back().characters(0x4E00, 0x9FFF, 2),
                                           60, Color::rgb(0));
    ASSERT_FALSE(bitmap.isNull());
    EXPECT_GT(bitmap.width(), 60);
}
