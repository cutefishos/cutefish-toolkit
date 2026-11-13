// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "base/transform.h"
#include "ui/bitmap.h"
#include "ui/canvas_2d.h"
#include "ui/path.h"
#include "ui/text_layout.h"
#include "ui/theme.h"

using namespace cutefish;
using ui::Bitmap;
using ui::ColorF;
using ui::Canvas2D;
using ui::Path;
using Format = Bitmap::Format;

static constexpr ColorF kRed = {1, 0, 0, 1};
static constexpr ColorF kBlue = {0, 0, 1, 1};

TEST(Canvas2DTest, FillsThroughTheTransform)
{
    Bitmap bitmap(20, 20, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    ASSERT_TRUE(canvas.isValid());
    canvas.setFillColor(kRed);
    canvas.translate(10, 0);
    canvas.scale(2, 2);
    canvas.fillRect({0, 0, 2, 2});
    EXPECT_EQ(bitmap.pixel(11, 1), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(13, 3), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(9, 1), 0u);
    EXPECT_EQ(bitmap.pixel(15, 1), 0u);

    // Mirrored vertically.
    canvas.setTransform(base::AffineTransform(1, 0, 0, -1, 0, 20));
    canvas.setFillColor(kBlue);
    canvas.fillRect({0, 0, 4, 4});
    EXPECT_EQ(bitmap.pixel(1, 18), 0xFF0000FFu);
    EXPECT_EQ(bitmap.pixel(1, 1), 0u);
}

// Anticlockwise from the top edge: a round upper left corner, a square lower
// left, a smaller round lower right and a square upper right.
static Path mixedCornerOutline()
{
    const double k = 0.5522847498;
    Path path;
    path.moveTo(12, 2);
    path.cubicTo(12 - 10 * k, 2, 2, 12 - 10 * k, 2, 12);
    path.lineTo(2, 30);
    path.lineTo(32, 30);
    path.cubicTo(32 + 6 * k, 30, 38, 24 + 6 * k, 38, 24);
    path.lineTo(38, 2);
    path.close();
    return path;
}

static void expectMixedCorners(const Bitmap& bitmap)
{
    EXPECT_EQ(bitmap.pixel(20, 16), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(3, 3), 0u);
    EXPECT_EQ(bitmap.pixel(6, 6), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(3, 28), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(37, 29), 0u);
    EXPECT_EQ(bitmap.pixel(35, 27), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(36, 3), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(1, 16), 0u);
    EXPECT_EQ(bitmap.pixel(39, 16), 0u);
}

TEST(Canvas2DTest, FillsARoundedOutlineCornerByCorner)
{
    Bitmap bitmap(40, 32, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.setFillColor(kRed);
    canvas.fillPath(mixedCornerOutline());
    expectMixedCorners(bitmap);
}

TEST(Canvas2DTest, ClipsToARoundedOutlineCornerByCorner)
{
    Bitmap bitmap(40, 32, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.clipToPath(mixedCornerOutline());
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 40, 32});
    expectMixedCorners(bitmap);
}

TEST(Canvas2DTest, BlursTheBackdropInsideThePath)
{
    // Red on the left half, blue on the right; the blur is over the middle.
    Bitmap bitmap(40, 20, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    ASSERT_TRUE(canvas.isValid());
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 20, 20});
    canvas.setFillColor(kBlue);
    canvas.fillRect({20, 0, 20, 20});
    const uint32_t outside = bitmap.pixel(12, 10);
    Path path;
    path.addRect({14, 0, 12, 20});
    canvas.blurBackdrop(path, 8);
    // Either side of the edge mixes; the edge's own column is half and half.
    const uint32_t left = bitmap.pixel(18, 10), right = bitmap.pixel(21, 10);
    EXPECT_GT(left & 0xFF, 0x20u);
    EXPECT_GT((right >> 16) & 0xFF, 0x20u);
    EXPECT_EQ(left >> 24, 0xFFu);
    // Outside the path nothing changes.
    EXPECT_EQ(bitmap.pixel(12, 10), outside);
    EXPECT_EQ(bitmap.pixel(30, 10), 0xFF0000FFu);
}

static Bitmap blurredBackdrop(double saturation)
{
    Bitmap bitmap(40, 20, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.setFillColor({0.4, 0.75, 0.3, 1});
    canvas.fillRect({0, 0, 20, 20});
    canvas.setFillColor({0.9, 0.5, 0.6, 1});
    canvas.fillRect({20, 0, 20, 20});
    Path path;
    path.addRoundedRect({6, 2, 28, 16}, 6, 6);
    if (saturation < 0)
        canvas.blurBackdrop(path, 8);
    else
        canvas.blurBackdrop(path, 8, saturation);
    return bitmap;
}

static uint64_t pixelSum(const Bitmap& bitmap)
{
    uint64_t sum = 0;
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x)
            sum = sum * 31 + bitmap.pixel(x, y);
    }
    return sum;
}

TEST(Canvas2DTest, BackdropBlurWithoutSaturationIsUnchanged)
{
    // The plain blur's pixels as they were before saturation existed.
    const Bitmap plain = blurredBackdrop(-1);
    EXPECT_EQ(pixelSum(plain), 1676948802072002144u);
    EXPECT_EQ(pixelSum(blurredBackdrop(1)), pixelSum(plain));
}

TEST(Canvas2DTest, BackdropBlurSaturationDeepensColour)
{
    const Bitmap plain = blurredBackdrop(1), saturated = blurredBackdrop(2);
    const auto chroma = [](uint32_t pixel) {
        const int r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
        return std::max({r, g, b}) - std::min({r, g, b});
    };
    EXPECT_GT(chroma(saturated.pixel(10, 10)), chroma(plain.pixel(10, 10)) + 20);
    EXPECT_GT(chroma(saturated.pixel(30, 10)), chroma(plain.pixel(30, 10)) + 20);
    // Outside the path nothing changes.
    EXPECT_EQ(saturated.pixel(2, 10), plain.pixel(2, 10));
    EXPECT_EQ(saturated.pixel(10, 0), plain.pixel(10, 0));
}

static void fillBackdrop(Canvas2D& canvas, double leftAlpha)
{
    canvas.setFillColor({0.9, 0.1, 0.1, leftAlpha});
    canvas.fillRect({0, 0, 20, 20});
    canvas.setFillColor({0.1, 0.2, 0.9, 1});
    canvas.fillRect({20, 0, 20, 20});
}

static uint32_t over(uint32_t source, uint32_t destination)
{
    // Premultiplied source over destination, per channel.
    const unsigned keep = 255 - (source >> 24);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const unsigned s = (source >> shift) & 0xFF, d = (destination >> shift) & 0xFF;
        out |= std::min(255u, s + (d * keep + 127) / 255) << shift;
    }
    return out;
}

static bool near(uint32_t a, uint32_t b, int tolerance)
{
    for (int shift = 0; shift < 32; shift += 8) {
        if (std::abs(int((a >> shift) & 0xFF) - int((b >> shift) & 0xFF)) > tolerance)
            return false;
    }
    return true;
}

TEST(Canvas2DTest, BackdropBlurSaturationKeepsNeutralColours)
{
    // White and greys have no colour to deepen: saturation leaves them as
    // the plain blur does.
    for (const double grey : {1.0, 0.93, 0.5}) {
        Bitmap plain(40, 20, Format::Argb32Premultiplied), saturated(40, 20, Format::Argb32Premultiplied);
        Canvas2D plainCanvas(plain), saturatedCanvas(saturated);
        for (Canvas2D* canvas : {&plainCanvas, &saturatedCanvas}) {
            canvas->setFillColor({grey, grey, grey, 1});
            canvas->fillRect({0, 0, 40, 20});
        }
        Path path;
        path.addRect({10, 0, 20, 20});
        plainCanvas.blurBackdrop(path, 8);
        saturatedCanvas.blurBackdrop(path, 8, 1.8);
        for (int x : {12, 20, 28})
            EXPECT_TRUE(near(saturated.pixel(x, 10), plain.pixel(x, 10), 1)) << grey << " " << x;
    }
}

TEST(Canvas2DTest, BackdropBelowGroupsOutsideAGroupIsThePlainBlur)
{
    Bitmap plain(40, 20, Format::Argb32Premultiplied), below(40, 20, Format::Argb32Premultiplied);
    Canvas2D plainCanvas(plain), belowCanvas(below);
    fillBackdrop(plainCanvas, 1);
    fillBackdrop(belowCanvas, 1);
    Path path;
    path.addRoundedRect({6, 2, 28, 16}, 6, 6);
    plainCanvas.blurBackdrop(path, 8, 1.5);
    belowCanvas.blurBackdropBelowGroups(path, 8, 1.5);
    EXPECT_EQ(pixelSum(below), pixelSum(plain));
}

TEST(Canvas2DTest, BackdropBelowGroupsSeesThroughAGroup)
{
    Path path;
    path.addRect({10, 0, 20, 20});
    Bitmap reference(40, 20, Format::Argb32Premultiplied);
    Canvas2D referenceCanvas(reference);
    fillBackdrop(referenceCanvas, 1);
    referenceCanvas.blurBackdrop(path, 8);

    // A plain blur inside a group sees only the group's empty layer, so
    // what is under it shows unblurred.
    Bitmap inGroup(40, 20, Format::Argb32Premultiplied);
    Canvas2D inGroupCanvas(inGroup);
    fillBackdrop(inGroupCanvas, 1);
    const uint32_t under = inGroup.pixel(18, 10);
    inGroupCanvas.beginGroup();
    inGroupCanvas.blurBackdrop(path, 8);
    inGroupCanvas.endGroup();
    EXPECT_EQ(inGroup.pixel(18, 10), under);

    Bitmap below(40, 20, Format::Argb32Premultiplied);
    Canvas2D belowCanvas(below);
    fillBackdrop(belowCanvas, 1);
    belowCanvas.beginGroup();
    belowCanvas.blurBackdropBelowGroups(path, 8, 1);
    belowCanvas.endGroup();
    for (int x : {12, 18, 21, 27})
        EXPECT_TRUE(near(below.pixel(x, 10), reference.pixel(x, 10), 1)) << x;
    // Outside the path nothing changes.
    EXPECT_EQ(below.pixel(5, 10), reference.pixel(5, 10));
    EXPECT_EQ(below.pixel(35, 10), reference.pixel(35, 10));
}

TEST(Canvas2DTest, BackdropBelowGroupsOverATranslucentSurface)
{
    // The blurred backdrop is the group's content, composited once over
    // what is under the group, also where that is translucent.
    Path path;
    path.addRect({10, 0, 20, 20});
    Bitmap blurred(40, 20, Format::Argb32Premultiplied);
    Canvas2D blurredCanvas(blurred);
    fillBackdrop(blurredCanvas, 0.5);
    blurredCanvas.blurBackdrop(path, 8);

    Bitmap below(40, 20, Format::Argb32Premultiplied);
    Canvas2D belowCanvas(below);
    fillBackdrop(belowCanvas, 0.5);
    std::vector<uint32_t> under;
    for (int x : {12, 18, 21, 27})
        under.push_back(below.pixel(x, 10));
    belowCanvas.beginGroup();
    belowCanvas.blurBackdropBelowGroups(path, 8, 1);
    belowCanvas.endGroup();
    int i = 0;
    for (int x : {12, 18, 21, 27}) {
        EXPECT_TRUE(near(below.pixel(x, 10), over(blurred.pixel(x, 10), under[i]), 2)) << x;
        ++i;
    }
}

TEST(Canvas2DTest, RetainedBitmapsDrawAsTheirBitmap)
{
    Bitmap source(4, 2, Format::Argb32Premultiplied);
    source.fill(0xFFFF0000u);
    source.setPixel(1, 0, 0x800000FFu);
    const ui::RetainedBitmap retained(source);
    ASSERT_FALSE(retained.isNull());
    EXPECT_EQ(retained.width(), 4);
    EXPECT_EQ(retained.height(), 2);
    EXPECT_EQ(retained.byteCount(), 32u);
    // Taken when made: later changes to the bitmap do not reach it.
    const Bitmap original = Bitmap::copyOf(source.bits(), 4, 2, source.stride(), Format::Argb32Premultiplied);
    source.fill(0xFF00FF00u);

    Bitmap direct(12, 6, Format::Argb32Premultiplied);
    Canvas2D directCanvas(direct);
    directCanvas.setImageSmoothing(Canvas2D::Smoothing::None);
    directCanvas.drawBitmap(original, {2, 1, 8, 4});
    directCanvas.drawBitmap(original, {0, 0, 4, 2}, true);
    Bitmap drawn(12, 6, Format::Argb32Premultiplied);
    Canvas2D drawnCanvas(drawn);
    drawnCanvas.setImageSmoothing(Canvas2D::Smoothing::None);
    drawnCanvas.drawBitmap(retained, {2, 1, 8, 4});
    drawnCanvas.drawBitmap(retained, {0, 0, 4, 2}, true);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 12; ++x)
            EXPECT_EQ(drawn.pixel(x, y), direct.pixel(x, y)) << x << "," << y;
    }
    EXPECT_TRUE(ui::RetainedBitmap().isNull());
}

TEST(Canvas2DTest, RetainedBitmapsTakeChangedParts)
{
    Bitmap source(4, 2, Format::Argb32Premultiplied);
    source.fill(0xFFFF0000u);
    ui::RetainedBitmap retained(source);
    const ui::RetainedBitmap copy = retained;
    source.fill(0xFF0000FFu);
    retained.update(source, {1, 0, 2, 1});

    Bitmap drawn(4, 2, Format::Argb32Premultiplied);
    Canvas2D canvas(drawn);
    canvas.setImageSmoothing(Canvas2D::Smoothing::None);
    canvas.drawBitmap(copy, {0, 0, 4, 2});
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool changed = y == 0 && (x == 1 || x == 2);
            EXPECT_EQ(drawn.pixel(x, y), changed ? 0xFF0000FFu : 0xFFFF0000u) << x << "," << y;
        }
    }
}

TEST(Canvas2DTest, DmaBufImagesNeedAGpuCanvas)
{
    const int fd = memfd_create("canvas-2d-test", MFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    // XRGB8888.
    const ui::DmaBufImage image(4, 2, 0x34325258, 0, {{fd, 0, 16}});
    close(fd);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.width(), 4);
    EXPECT_EQ(image.height(), 2);

    Bitmap bitmap(4, 2, Format::Argb32Premultiplied);
    bitmap.fill(0xFF00FF00u);
    Canvas2D canvas(bitmap);
    EXPECT_FALSE(canvas.drawDmaBufImage(image, {0, 0, 4, 2}));
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(bitmap.pixel(x, y), 0xFF00FF00u) << x << "," << y;
    }
    EXPECT_TRUE(ui::DmaBufImage().isNull());
    EXPECT_TRUE(ui::DmaBufImage(4, 2, 0x34325258, 0, {}).isNull());
}

TEST(Canvas2DTest, ClipsSavesAndAlpha)
{
    Bitmap bitmap(10, 10, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.save();
    canvas.clipToRect({0, 0, 5, 10});
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 10, 10});
    canvas.restore();
    EXPECT_EQ(bitmap.pixel(2, 2), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(7, 2), 0u);
    EXPECT_EQ(canvas.clipBounds().width, 10);

    canvas.setGlobalAlpha(0.5);
    canvas.setFillColor(kBlue);
    canvas.fillRect({5, 0, 5, 10});
    EXPECT_NEAR(double(bitmap.alpha(7, 2)), 128, 1);

    Path triangle;
    triangle.moveTo(0, 0);
    triangle.lineTo(10, 0);
    triangle.lineTo(0, 10);
    triangle.close();
    canvas.setGlobalAlpha(1);
    canvas.clearRect({0, 0, 10, 10});
    canvas.clipToPath(triangle);
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 10, 10});
    EXPECT_EQ(bitmap.pixel(1, 1), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(8, 8), 0u);
}

TEST(Canvas2DTest, ClipsToABitmapsAlpha)
{
    Bitmap mask(2, 2, Format::Argb32Premultiplied);
    mask.fill(0u);
    mask.setPixel(0, 0, 0xFF000000u);
    mask.setPixel(1, 1, 0x80000000u);

    Bitmap bitmap(12, 12, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.setImageSmoothing(Canvas2D::Smoothing::None);
    canvas.setFillColor(kBlue);
    canvas.fillRect({0, 0, 12, 12});

    canvas.save();
    canvas.clipToBitmap(mask, {2, 2, 8, 8});
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 12, 12});
    canvas.restore();
    EXPECT_EQ(bitmap.pixel(3, 3), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(8, 3), 0xFF0000FFu);
    EXPECT_NEAR(double((bitmap.pixel(8, 8) >> 16) & 0xff), 128, 1);
    // Outside the rect nothing is drawn, whatever the bitmap's edge holds.
    EXPECT_EQ(bitmap.pixel(1, 1), 0xFF0000FFu);

    // Clearing reaches what was drawn before the clip.
    canvas.save();
    canvas.clipToBitmap(ui::RetainedBitmap(mask), {2, 2, 8, 8}, true);
    canvas.clearRect({0, 0, 12, 12});
    canvas.restore();
    EXPECT_EQ(bitmap.pixel(3, 8), 0u);
    EXPECT_EQ(bitmap.pixel(3, 3), 0xFFFF0000u);
    EXPECT_NEAR(double(bitmap.alpha(8, 3)), 127, 1);
    EXPECT_EQ(bitmap.pixel(1, 1), 0xFF0000FFu);
}

TEST(Canvas2DTest, StrokesAndDashes)
{
    Bitmap bitmap(20, 5, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.setStrokeColor(kRed);
    canvas.setLineWidth(2);
    canvas.setLineDash({4, 4});
    Path line;
    line.moveTo(0, 2);
    line.lineTo(20, 2);
    canvas.strokePath(line);
    EXPECT_EQ(bitmap.pixel(1, 2), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(6, 2), 0u);
    EXPECT_EQ(bitmap.pixel(9, 2), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(1, 4), 0u);
}

TEST(Canvas2DTest, GradientsStopWhereTold)
{
    Bitmap bitmap(30, 2, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.fillLinearGradient({{0, kRed}, {1, kBlue}}, {10, 0}, {20, 0}, false, true);
    EXPECT_EQ(bitmap.pixel(5, 1), 0u);
    EXPECT_GT((bitmap.pixel(10, 1) >> 16) & 0xFF, 0xE0u);
    EXPECT_EQ(bitmap.pixel(25, 1), 0xFF0000FFu);
    const uint32_t middle = bitmap.pixel(15, 1);
    EXPECT_GT((middle >> 16) & 0xFF, 0x60u);
    EXPECT_GT(middle & 0xFF, 0x60u);
}

TEST(Canvas2DTest, GroupsCompositeWithTheirAlpha)
{
    Bitmap bitmap(4, 4, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.setGlobalAlpha(0.5);
    canvas.beginGroup();
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 4, 4});
    canvas.fillRect({0, 0, 4, 4});
    canvas.endGroup();
    EXPECT_NEAR(double(bitmap.alpha(1, 1)), 128, 1);
    EXPECT_EQ(canvas.globalAlpha(), 0.5);
}

TEST(Canvas2DTest, GroupsCastOneShadow)
{
    Bitmap bitmap(20, 20, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.scale(2, 2);
    canvas.setShadow({6, 6}, 0, {0, 0, 0, 0.5});
    canvas.beginGroup();
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 2, 2});
    canvas.fillRect({0, 0, 2, 2});
    canvas.endGroup();
    EXPECT_EQ(bitmap.pixel(1, 1), 0xFFFF0000u);
    EXPECT_NEAR(double(bitmap.alpha(7, 7)), 128, 2);
    EXPECT_EQ(bitmap.pixel(11, 11), 0u);
}

TEST(Canvas2DTest, ShadowsIgnoreTheTransform)
{
    Bitmap bitmap(20, 20, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.scale(2, 2);
    canvas.setShadow({6, 6}, 0, {0, 0, 0, 1});
    canvas.setFillColor(kRed);
    canvas.fillRect({0, 0, 2, 2});
    EXPECT_EQ(bitmap.pixel(1, 1), 0xFFFF0000u);
    EXPECT_EQ(bitmap.pixel(7, 7), 0xFF000000u);
    EXPECT_EQ(bitmap.pixel(11, 11), 0u);
}

TEST(Canvas2DTest, TextStaysUprightUnderAMirror)
{
    Bitmap bitmap(80, 40, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    canvas.setTransform(base::AffineTransform(1, 0, 0, -1, 0, 40));
    ui::TextLayout layout("Hg", ui::FontSpec{ui::Theme::current().fontFamily, 20}, canvas.deviceScale());
    layout.setDefaultColor(ui::Color::rgb(0x000000));
    layout.beginLayout();
    layout.createLine(1000, {0, 0});
    layout.endLayout();
    // The layout's top left at (2, 38) lands at (2, 2).
    canvas.drawText(layout, {2, 38});
    int top = -1;
    int bottom = -1;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 80; ++x) {
            if (bitmap.alpha(x, y) > 128) {
                if (top < 0)
                    top = y;
                bottom = y;
            }
        }
    }
    ASSERT_GE(top, 0);
    EXPECT_GE(top, 2);
    EXPECT_LT(top, 12);
    EXPECT_GT(bottom, top + 10);
}

TEST(Canvas2DTest, TextTurnsWithTheSpace)
{
    Bitmap bitmap(40, 80, Format::Argb32Premultiplied);
    Canvas2D canvas(bitmap);
    // A quarter turn: x runs down the bitmap and y from its right edge.
    canvas.setTransform(base::AffineTransform(0, 1, -1, 0, 40, 0));
    ui::TextLayout layout("HHHH", ui::FontSpec{ui::Theme::current().fontFamily, 20}, canvas.deviceScale());
    layout.setDefaultColor(ui::Color::rgb(0x000000));
    layout.beginLayout();
    layout.createLine(1000, {0, 0});
    layout.endLayout();
    canvas.drawText(layout, {4, 4});
    int left = 40, right = -1, top = 80, bottom = -1;
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 40; ++x) {
            if (bitmap.alpha(x, y) > 128) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    ASSERT_GE(right, 0);
    // The run reads downwards from the turned origin at (36, 4): taller
    // than it is wide, and left of that origin.
    EXPECT_GT(bottom - top, 2 * (right - left));
    EXPECT_GE(top, 2);
    EXPECT_GE(left, 4);
    EXPECT_LE(right, 37);
}
