// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <unistd.h>

#include "base/transform.h"
#include "ui/bitmap.h"
#include "ui/bitmap_painter.h"
#include "ui/path.h"

using namespace cutefish;
using ui::Bitmap;
using ui::BitmapPainter;
using ui::Color;
using ui::Path;
using Format = Bitmap::Format;

static constexpr uint32_t kOpaqueRed = 0xFFFF0000;
static constexpr uint32_t kHalfGreen = 0x80008000;

TEST(BitmapTest, FormatsAndPixels)
{
    EXPECT_TRUE(Bitmap().isNull());
    EXPECT_TRUE(Bitmap(0, 5, Format::Alpha8).isNull());
    for (Format format : {Format::Argb32Premultiplied, Format::Rgba8888Premultiplied}) {
        Bitmap bitmap(3, 2, format);
        ASSERT_FALSE(bitmap.isNull());
        EXPECT_EQ(bitmap.stride(), 12u);
        EXPECT_EQ(bitmap.pixel(1, 1), 0u);
        bitmap.setPixel(1, 1, kHalfGreen);
        EXPECT_EQ(bitmap.pixel(1, 1), kHalfGreen);
        EXPECT_EQ(bitmap.alpha(1, 1), 0x80);
        EXPECT_EQ(bitmap.pixel(5, 5), 0u);
    }
    Bitmap rgba(1, 1, Format::Rgba8888Premultiplied);
    rgba.setPixel(0, 0, 0xFF112233);
    const uint8_t expected[] = {0x11, 0x22, 0x33, 0xFF};
    EXPECT_EQ(std::memcmp(rgba.constBits(), expected, 4), 0);
    Bitmap argb = rgba.converted(Format::Argb32Premultiplied);
    EXPECT_EQ(argb.pixel(0, 0), 0xFF112233u);
    EXPECT_EQ(argb.constBits()[0], 0x33);

    Bitmap alpha(5, 1, Format::Alpha8);
    EXPECT_EQ(alpha.stride(), 8u);
    alpha.fill(kHalfGreen);
    EXPECT_EQ(alpha.pixel(4, 0), 0x80000000u);
    EXPECT_EQ(rgba.converted(Format::Alpha8).alpha(0, 0), 0xFF);
}

TEST(BitmapTest, WrapCopyOnWrite)
{
    uint32_t memory[4] = {kOpaqueRed, 0, 0, 0};
    Bitmap wrapped = Bitmap::wrap(memory, 2, 2, 8, Format::Argb32Premultiplied);
    EXPECT_EQ(wrapped.pixel(0, 0), kOpaqueRed);
    // Alone, it writes into the memory itself.
    wrapped.setPixel(1, 0, kHalfGreen);
    EXPECT_EQ(memory[1], kHalfGreen);
    // A copy detaches on write.
    Bitmap copy = wrapped;
    copy.setPixel(0, 1, kOpaqueRed);
    EXPECT_EQ(memory[2], 0u);
    EXPECT_EQ(copy.pixel(0, 1), kOpaqueRed);
    EXPECT_EQ(copy.pixel(1, 0), kHalfGreen);

    const Bitmap owned = Bitmap::copyOf(memory, 2, 2, 8, Format::Argb32Premultiplied);
    memory[0] = 0;
    EXPECT_EQ(owned.pixel(0, 0), kOpaqueRed);
}

TEST(BitmapTest, CopyMirrorScale)
{
    Bitmap bitmap(4, 4, Format::Argb32Premultiplied);
    bitmap.fill(kOpaqueRed);
    bitmap.setPixel(0, 0, kHalfGreen);

    const Bitmap part = bitmap.copy(base::Rect(-1, -1, 3, 3));
    EXPECT_EQ(part.size(), base::Size(3, 3));
    EXPECT_EQ(part.pixel(0, 0), 0u);
    EXPECT_EQ(part.pixel(1, 1), kHalfGreen);
    EXPECT_EQ(part.pixel(2, 2), kOpaqueRed);

    EXPECT_EQ(bitmap.mirrored(false, true).pixel(0, 3), kHalfGreen);
    EXPECT_EQ(bitmap.mirrored(true, false).pixel(3, 0), kHalfGreen);
    EXPECT_EQ(bitmap.mirrored(true, true).pixel(3, 3), kHalfGreen);

    Bitmap red(64, 32, Format::Rgba8888Premultiplied);
    red.fill(kOpaqueRed);
    for (bool smooth : {true, false}) {
        const Bitmap small = red.scaled(base::Size(13, 7), smooth);
        ASSERT_EQ(small.size(), base::Size(13, 7));
        EXPECT_EQ(small.format(), Format::Rgba8888Premultiplied);
        EXPECT_EQ(small.pixel(6, 3), kOpaqueRed);
        const Bitmap large = red.scaled(base::Size(100, 50), smooth);
        EXPECT_EQ(large.pixel(50, 25), kOpaqueRed);
    }
}

TEST(BitmapTest, PngRoundTrip)
{
    Bitmap bitmap(5, 3, Format::Argb32Premultiplied);
    bitmap.fill(kOpaqueRed);
    bitmap.setPixel(2, 1, 0xFF00FF00);
    bitmap.setPixel(4, 2, 0);
    const std::optional<std::string> png = bitmap.encodePng();
    ASSERT_TRUE(png);
    EXPECT_EQ(png->substr(1, 3), "PNG");
    EXPECT_EQ(Bitmap::decode(*png), bitmap);

    const std::string file = (std::filesystem::temp_directory_path()
                              / ("bitmap-test-" + std::to_string(getpid()) + ".png")).string();
    ASSERT_TRUE(bitmap.converted(Format::Rgba8888Premultiplied).savePng(file));
    EXPECT_EQ(Bitmap::decodeFile(file), bitmap);
    const Bitmap boxed = Bitmap::decodeFile(file, base::Size(10, 10), Bitmap::Scaling::Fit);
    EXPECT_EQ(boxed.size(), base::Size(10, 6));
    std::filesystem::remove(file);
    EXPECT_TRUE(Bitmap::decode("not an image").isNull());
    EXPECT_TRUE(Bitmap::decodeFile("/nonexistent.png").isNull());
    EXPECT_EQ(ui::scaledSize(base::Size(400, 300), base::Size(100, 100), Bitmap::Scaling::Cover),
              base::Size(133, 100));
}

TEST(BitmapTest, DecodesEveryFormatFromBytes)
{
    // One red pixel.
    const std::string gif("GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\x00\x00\x00\x00\x00"
                          "\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b",
                          35);
    const Bitmap fromGif = Bitmap::decode(gif);
    ASSERT_EQ(fromGif.size(), base::Size(1, 1));
    EXPECT_EQ(fromGif.pixel(0, 0), kOpaqueRed);

    // Drawn at the box it is decoded into.
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='5'>"
                            "<rect width='10' height='5' fill='#ff0000'/></svg>";
    EXPECT_EQ(Bitmap::decode(svg).size(), base::Size(10, 5));
    const Bitmap fromSvg = Bitmap::decode(svg, base::Size(40, 40));
    ASSERT_EQ(fromSvg.size(), base::Size(40, 20));
    EXPECT_EQ(fromSvg.pixel(20, 10), kOpaqueRed);
}

TEST(PathTest, ElementsFollowTheCommonLayout)
{
    Path path;
    EXPECT_TRUE(path.isEmpty());
    path.moveTo(5, 5);
    EXPECT_TRUE(path.isEmpty());
    path.moveTo(0, 0);
    path.lineTo(10, 0);
    path.cubicTo(10, 5, 5, 10, 0, 10);
    path.close();
    path.lineTo(3, 3);
    using Type = Path::Element::Type;
    const std::vector<Path::Element> expected = {
        {Type::MoveTo, 0, 0},       {Type::LineTo, 10, 0},     {Type::CurveTo, 10, 5},
        {Type::CurveToData, 5, 10}, {Type::CurveToData, 0, 10}, {Type::LineTo, 0, 0},
        {Type::MoveTo, 0, 0},       {Type::LineTo, 3, 3},
    };
    EXPECT_EQ(path.elements(), expected);
    EXPECT_EQ(path.elementCount(), expected.size());

    Path quad;
    quad.moveTo(0, 0);
    quad.quadTo(3, 3, 6, 0);
    const auto elements = quad.elements();
    ASSERT_EQ(elements.size(), 4u);
    EXPECT_DOUBLE_EQ(elements[1].x, 2);
    EXPECT_DOUBLE_EQ(elements[1].y, 2);
    EXPECT_DOUBLE_EQ(elements[2].x, 4);
    EXPECT_DOUBLE_EQ(elements[2].y, 2);
}

TEST(PathTest, BoundsAndContains)
{
    Path ellipse;
    ellipse.addEllipse({10, 20, 40, 20});
    const ui::RectF bounds = ellipse.boundingRect();
    EXPECT_NEAR(bounds.x, 10, 1e-9);
    EXPECT_NEAR(bounds.y, 20, 1e-9);
    EXPECT_NEAR(bounds.width, 40, 1e-9);
    EXPECT_NEAR(bounds.height, 20, 1e-9);
    EXPECT_TRUE(ellipse.contains({30, 30}));
    EXPECT_FALSE(ellipse.contains({11, 21}));

    Path curve;
    curve.moveTo(0, 0);
    curve.cubicTo(0, 10, 10, 10, 10, 0);
    EXPECT_NEAR(curve.boundingRect().height, 7.5, 1e-9);
    EXPECT_EQ(curve.controlPointRect(), (ui::RectF{0, 0, 10, 10}));

    // Two nested squares: a hole under even-odd, filled under winding.
    Path nested;
    nested.addRect({0, 0, 10, 10});
    nested.addRect({2, 2, 6, 6});
    EXPECT_TRUE(nested.contains({5, 5}));
    nested.setFillRule(Path::FillRule::EvenOdd);
    EXPECT_FALSE(nested.contains({5, 5}));
    EXPECT_TRUE(nested.contains({1, 1}));

    Path rounded;
    rounded.addRoundedRect({0, 0, 20, 10}, 30, 2);
    EXPECT_NEAR(rounded.boundingRect().width, 20, 1e-9);
    EXPECT_FALSE(rounded.contains({0.2, 0.2}));
    EXPECT_TRUE(rounded.contains({10, 0.2}));
}

TEST(PathTest, TransformsPolygonsAndOperations)
{
    Path square;
    square.addRect({0, 0, 10, 10});
    EXPECT_EQ(square.translated(5, 1).boundingRect(), (ui::RectF{5, 1, 10, 10}));
    base::AffineTransform turn = base::AffineTransform::fromRotation(90);
    turn.scale(2, 2);
    EXPECT_EQ(square.transformed(turn).boundingRect(), (ui::RectF{-20, 0, 20, 20}));

    const auto polygons = square.toSubpathPolygons();
    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].size(), 5u);

    Path circle;
    circle.addEllipse({0, 0, 100, 100});
    const auto flattened = circle.toSubpathPolygons(0.25);
    ASSERT_EQ(flattened.size(), 1u);
    for (const ui::PointF& point : flattened[0])
        EXPECT_NEAR(std::hypot(point.x - 50, point.y - 50), 50, 0.1);

    Path other;
    other.addRect({5, 5, 10, 10});
    const ui::RectF both = square.intersected(other).boundingRect();
    EXPECT_NEAR(both.x, 5, 1e-4);
    EXPECT_NEAR(both.width, 5, 1e-4);
    EXPECT_NEAR(square.united(other).boundingRect().width, 15, 1e-4);
    EXPECT_FALSE(square.subtracted(other).contains({7, 7}));
    EXPECT_TRUE(square.subtracted(other).contains({2, 2}));
    EXPECT_TRUE(square.intersected(Path()).isEmpty());

    Path line;
    line.moveTo(0, 0);
    line.lineTo(10, 0);
    const ui::RectF band = line.stroked(4, ui::LineCap::Butt).boundingRect();
    EXPECT_NEAR(band.y, -2, 1e-4);
    EXPECT_NEAR(band.height, 4, 1e-4);
    EXPECT_NEAR(band.width, 10, 1e-4);
    EXPECT_NEAR(line.stroked(4).boundingRect().width, 14, 1e-4);
}

TEST(BitmapPainterTest, FillsWithTransformAndModes)
{
    Bitmap bitmap(20, 20, Format::Argb32Premultiplied);
    {
        BitmapPainter painter(bitmap);
        ASSERT_TRUE(painter.isActive());
        painter.scale(2, 2);
        painter.fillRect({1, 1, 3, 3}, Color::fromArgb(kOpaqueRed));
    }
    EXPECT_EQ(bitmap.pixel(2, 2), kOpaqueRed);
    EXPECT_EQ(bitmap.pixel(7, 7), kOpaqueRed);
    EXPECT_EQ(bitmap.pixel(8, 8), 0u);
    EXPECT_EQ(bitmap.pixel(1, 2), 0u);

    {
        BitmapPainter painter(bitmap);
        painter.setCompositionMode(BitmapPainter::CompositionMode::DestinationOut);
        painter.fillRect({2, 2, 2, 2}, Color::fromArgb(0xFFFFFFFF));
    }
    EXPECT_EQ(bitmap.pixel(3, 3), 0u);
    EXPECT_EQ(bitmap.pixel(5, 5), kOpaqueRed);

    Bitmap mask(16, 16, Format::Alpha8);
    {
        BitmapPainter painter(mask);
        painter.setAntialiasing(true);
        painter.fillEllipse({0, 0, 16, 16}, Color::fromArgb(0xFFFFFFFF));
    }
    EXPECT_EQ(mask.alpha(8, 8), 255);
    EXPECT_EQ(mask.alpha(0, 0), 0);
    const uint8_t edge = mask.alpha(8, 0);
    EXPECT_GT(edge, 0);
    EXPECT_LE(edge, 255);
}

TEST(BitmapPainterTest, PolygonsLinesAndBitmaps)
{
    Bitmap bitmap(10, 10, Format::Rgba8888Premultiplied);
    {
        BitmapPainter painter(bitmap);
        painter.drawPolygon({{1, 1}, {8, 1}, {8, 8}, {1, 8}}, Color::fromArgb(0xFF000000),
                            Color::fromArgb(0xFFFFFFFF), 1);
    }
    EXPECT_EQ(bitmap.pixel(4, 4), 0xFF000000u);
    EXPECT_EQ(bitmap.pixel(1, 4), 0xFFFFFFFFu);

    Bitmap source(2, 2, Format::Argb32Premultiplied);
    source.fill(kHalfGreen);
    Bitmap target(6, 6, Format::Argb32Premultiplied);
    {
        BitmapPainter painter(target);
        painter.drawBitmap(base::PointF(3, 3), source);
        painter.setOpacity(1);
        painter.drawLine({0, 0.5}, {6, 0.5}, Color::fromArgb(kOpaqueRed), 1, ui::LineCap::Butt);
    }
    EXPECT_EQ(target.pixel(3, 3), kHalfGreen);
    EXPECT_EQ(target.pixel(4, 4), kHalfGreen);
    EXPECT_EQ(target.pixel(2, 2), 0u);
    EXPECT_EQ(target.pixel(3, 0), kOpaqueRed);

    Bitmap empty;
    BitmapPainter inactive(empty);
    EXPECT_FALSE(inactive.isActive());
    inactive.fillRect({0, 0, 1, 1}, Color());
}
