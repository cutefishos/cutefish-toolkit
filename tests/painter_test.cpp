// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "ui/icon.h"
#include "ui/layout.h"
#include "ui/painter.h"
#include "ui/raster_image.h"
#include "ui/view.h"
#include "ui/window.h"

using namespace cutefish::ui;

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

Image render(std::function<void(Painter&)> draw, double scale = 1, SizeF size = {100, 100})
{
    Window window;
    window.setScale(scale);
    window.resize(size);
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    window.setContentView(std::make_unique<Canvas>(std::move(draw)));
    window.show();
    return window.grab();
}

bool near(Color a, Color b, int tolerance = 2)
{
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
}

constexpr Color kWhite = Color::rgb(0xFFFFFF);
constexpr Color kRed = Color::rgb(0xFF0000);
constexpr Color kBlue = Color::rgb(0x0000FF);

} // namespace

TEST(PainterTest, FillsAndStrokesPaths)
{
    const Image image = render([](Painter& painter) {
        Path triangle;
        triangle.moveTo({10, 10});
        triangle.lineTo({60, 10});
        triangle.lineTo({10, 60});
        triangle.close();
        painter.fillPath(triangle, kRed);
        Path line;
        line.moveTo({20, 80});
        line.cubicTo({40, 70}, {60, 90}, {90, 80});
        painter.strokePath(line, kBlue, 4);
    });
    EXPECT_TRUE(near(image.pixel(20, 20), kRed));
    EXPECT_TRUE(near(image.pixel(55, 55), kWhite));
    EXPECT_TRUE(near(image.pixel(20, 80), kBlue, 40));
    EXPECT_TRUE(near(image.pixel(90, 95), kWhite));
}

TEST(PainterTest, FillsWithAVerticalGradient)
{
    for (const double scale : {1.0, 1.5}) {
        const Image image = render(
            [](Painter& painter) {
                const RectF rect{0, 0, 100, 100};
                painter.fillRoundedRect(rect, 0, LinearGradient::vertical(rect, kRed, kBlue));
            },
            scale);
        const int bottom = image.height() - 1;
        EXPECT_TRUE(near(image.pixel(50, 0), kRed, 6));
        EXPECT_TRUE(near(image.pixel(50, bottom), kBlue, 6));
        const Color middle = image.pixel(50, image.height() / 2);
        EXPECT_NEAR(middle.r, 128, 6);
        EXPECT_NEAR(middle.b, 128, 6);
    }
}

TEST(PainterTest, ClipsToRoundedCorners)
{
    const Image image = render([](Painter& painter) {
        painter.save();
        painter.clipRoundedRect({10, 10, 80, 80}, 20);
        painter.fillRect({0, 0, 100, 100}, kRed);
        painter.restore();
    });
    EXPECT_TRUE(near(image.pixel(11, 11), kWhite));
    EXPECT_TRUE(near(image.pixel(50, 11), kRed));
    EXPECT_TRUE(near(image.pixel(50, 50), kRed));
    EXPECT_TRUE(near(image.pixel(5, 50), kWhite));
}

TEST(PainterTest, SnapsShapesAsPixelGrid)
{
    Rect card;
    Rect sliver;
    render(
        [&](Painter& painter) {
            card = painter.snappedRect({10.33, 10.5, 180, 42});
            sliver = painter.snappedRect({0.4, 0, 10.2, 0.2});
        },
        1.5);
    // The first edge on the next whole pixel, as many pixels as the rect encloses.
    EXPECT_EQ(card, (Rect{16, 16, 270, 63}));
    EXPECT_EQ(sliver, (Rect{1, 0, 15, 1}));
}

// Reference rounded rectangles at 150%, rendered on the test laptop: the
// top-left corners of white cards on #EEEEEE.
TEST(PainterTest, RoundedRectCornersMatchReference)
{
    constexpr int kContinuous[8][8] = {
        {238, 238, 238, 238, 238, 238, 238, 238},
        {238, 238, 238, 238, 238, 238, 238, 238},
        {238, 238, 238, 238, 238, 238, 238, 238},
        {238, 238, 238, 238, 238, 238, 238, 240},
        {238, 238, 238, 238, 238, 238, 243, 254},
        {238, 238, 238, 238, 238, 244, 255, 255},
        {238, 238, 238, 238, 243, 255, 255, 255},
        {238, 238, 238, 240, 254, 255, 255, 255},
    };
    constexpr int kCircular[8][8] = {
        {238, 238, 238, 238, 238, 238, 238, 238},
        {238, 238, 238, 238, 238, 238, 238, 238},
        {238, 238, 238, 238, 238, 238, 238, 238},
        {238, 238, 238, 238, 238, 238, 239, 248},
        {238, 238, 238, 238, 238, 240, 252, 255},
        {238, 238, 238, 238, 240, 252, 255, 255},
        {238, 238, 238, 239, 252, 255, 255, 255},
        {238, 238, 238, 248, 255, 255, 255, 255},
    };
    Window window;
    window.setScale(1.5);
    window.resize({400, 60});
    window.setBackgroundColor(Color::rgb(0xEEEEEE));
    window.setContentView(std::make_unique<Canvas>([](Painter& painter) {
        painter.drawRoundedRect({10.33, 10.5, 180, 42}, {12, 0, Corners::Continuous}, kWhite);
        painter.drawRoundedRect({200.25, 10.5, 180, 42}, {12, 0, Corners::Circular}, kWhite);
    }));
    window.show();
    const Image image = window.grab();
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            EXPECT_NEAR(image.pixel(16 + x, 16 + y).r, kContinuous[y][x], 1) << x << "," << y;
            EXPECT_NEAR(image.pixel(301 + x, 16 + y).r, kCircular[y][x], 1) << x << "," << y;
        }
    }
}

// A focus ring 3 outside a field: the inset and the border in whole pixels
// from where the field's own shape snaps.
TEST(PainterTest, InsetRingsFollowTheSnappedShape)
{
    const RectF field{20.5, 30.25, 60, 30};
    const Image image = render(
        [&](Painter& painter) {
            painter.drawRoundedRect(field, {6, 0}, kBlue);
            painter.drawRoundedRect({field.x - 3, field.y - 3, field.width + 6, field.height + 6}, {6, -3},
                                    Color{0, 0, 0, 0}, 2, kRed);
        },
        1.5, {120, 100});
    // The field snaps to x 31, y 46; the ring spans 4.5 -> 5 px outside, 2 -> 3 px wide.
    const int middle = 46 + 22;
    EXPECT_TRUE(near(image.pixel(30, middle), kWhite));
    EXPECT_TRUE(near(image.pixel(31, middle), kBlue));
    EXPECT_TRUE(near(image.pixel(25, middle), kWhite));
    EXPECT_TRUE(near(image.pixel(26, middle), kRed));
    EXPECT_TRUE(near(image.pixel(28, middle), kRed));
}

// What Skia's SVG renderer leaves out: a size without a view
// box, <style> class rules with currentColor, and a pattern in the bounding
// box units of the rect it fills.
TEST(PainterTest, DrawsSvgFeaturesSkiaLeavesOut)
{
    // A file each: drawings are cached by path.
    int files = 0;
    const auto draw = [&files](const std::string& svg) {
        const std::string path =
            std::filesystem::temp_directory_path() / ("painter-test-qt-" + std::to_string(files++) + ".svg");
        std::ofstream(path) << svg;
        const Image image = render([&path](Painter& painter) { painter.drawSvg(path, {0, 0, 32, 32}); }, 1, {32, 32});
        std::filesystem::remove(path);
        return image;
    };

    // 16 units wide without a view box: the right half fills the right half.
    const Image unboxed = draw(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
<rect x="8" y="0" width="8" height="16" fill="#0000ff"/></svg>)svg");
    EXPECT_TRUE(near(unboxed.pixel(28, 16), kBlue));
    EXPECT_TRUE(near(unboxed.pixel(4, 16), kWhite));

    const Image styled = draw(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
<defs><style type="text/css">
  .ColorScheme-Text {
    color:#ff0000;
  }
</style></defs>
<rect width="16" height="16" style="fill:currentColor" class="ColorScheme-Text"/></svg>)svg");
    EXPECT_TRUE(near(styled.pixel(16, 16), kRed));

    // A 2-pixel image stretched over the rect: blue left, red right.
    const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAEElEQVR4nGNgYPj//z+QAAAP+QP9HTMzcAAAAABJRU5ErkJggg==";
    std::string base64;
    for (size_t i = 0; i < png.size(); i += 16)
        base64 += png.substr(i, 16) + "\n    ";
    const Image patterned = draw(R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 16 16">
<rect width="16" height="16" fill="url(#p)"/>
<defs><pattern id="p" patternContentUnits="objectBoundingBox" width="1" height="1">
<use xlink:href="#i" transform="matrix(0.5 0 0 1 0 0)"/></pattern>
<image id="i" width="2" height="1" preserveAspectRatio="none" xlink:href="data:image/png;base64,)svg" + base64 + R"svg("/>
</defs></svg>)svg");
    EXPECT_TRUE(near(patterned.pixel(4, 16), kBlue, 8));
    EXPECT_TRUE(near(patterned.pixel(28, 16), kRed, 8));

    // The same filling a rectangle drawn as a path, as Colloid's PNG shadows
    // do: librsvg draws it alike.
    const Image pathPatterned = draw(R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 16 16">
<path fill="url(#p)" d="M0 0h16v16H0z"/>
<defs><pattern id="p" patternContentUnits="objectBoundingBox" width="1" height="1">
<use xlink:href="#i" transform="matrix(0.5 0 0 1 0 0)"/></pattern>
<image id="i" width="2" height="1" preserveAspectRatio="none" xlink:href="data:image/png;base64,)svg" + base64 + R"svg("/>
</defs></svg>)svg");
    EXPECT_TRUE(near(pathPatterned.pixel(4, 16), kBlue, 8));
    EXPECT_TRUE(near(pathPatterned.pixel(28, 16), kRed, 8));
}

TEST(PainterTest, DrawsSvgFilesAndAppGlyphs)
{
    const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8" viewBox="0 0 16 8">
<rect x="0" y="0" width="8" height="8" fill="#00ff00"/></svg>)";
    const std::string path = std::filesystem::temp_directory_path() / "painter-test.svg";
    std::ofstream(path) << svg;

    const Image plain = render([&path](Painter& painter) { painter.drawSvg(path, {0, 0, 32, 16}); }, 1.5);
    EXPECT_TRUE(near(plain.pixel(10, 10), Color::rgb(0x00FF00)));
    EXPECT_TRUE(near(plain.pixel(40, 10), kWhite));
    const Image tinted = render([&path](Painter& painter) { painter.drawSvg(path, {0, 0, 32, 16}, kRed); }, 1.5);
    EXPECT_TRUE(near(tinted.pixel(10, 10), kRed));
    std::filesystem::remove(path);

    Icon::registerGlyph("test-square", R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
<rect width="16" height="16" fill="#000"/></svg>)");
    Window window;
    window.resize({40, 40});
    window.setBackgroundColor(kWhite);
    auto root = std::make_unique<Row>();
    Icon* icon = root->addChild<Icon>("test-square", 16);
    icon->setColor(kBlue);
    window.setContentView(std::move(root));
    window.show();
    const Image image = window.grab();
    const RectF geometry = icon->geometry();
    EXPECT_TRUE(near(image.pixel(int(geometry.x + 8), int(geometry.y + 8)), kBlue));
}

TEST(PainterTest, RasterImagesDrawTheirPixelsScaled)
{
    // Red on the left half, blue on the right, 4 x 2.
    std::vector<uint32_t> pixels;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            pixels.push_back(x < 2 ? 0xffff0000u : 0xff0000ffu);
    const RasterImage image(4, 2, std::move(pixels), true);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.width(), 4);
    EXPECT_EQ(image.sizeInBytes(), 32u);
    const Image drawn = render([&image](Painter& painter) { painter.drawImage(image.skImage(), {10, 10, 40, 20}); }, 2);
    EXPECT_TRUE(near(drawn.pixel(30, 40), kRed));
    EXPECT_TRUE(near(drawn.pixel(90, 40), kBlue));
    EXPECT_TRUE(RasterImage(4, 2, std::vector<uint32_t>(3), true).isNull());
}
