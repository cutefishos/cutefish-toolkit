// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <string>

#include "ui/label.h"
#include "ui/layout.h"
#include "ui/theme.h"
#include "ui/window.h"

using namespace cutefish::ui;

// Reference numbers for Noto Sans in labels: font metrics, line layout
// with design metrics and tight ink bounds, measured on the test laptop at
// a device pixel ratio of 1.5 and of 1.
namespace {

struct Measured
{
    Window window;
    Label* label = nullptr;

    Measured(double pixelSize, double scale, const std::string& text, int weight = 400)
    {
        Theme theme = Theme::current();
        theme.fontFamily = "Noto Sans";
        Theme::setCurrent(theme);
        auto root = std::make_unique<Row>();
        label = root->addChild<Label>(text);
        label->setFontSize(pixelSize);
        label->setWeight(weight);
        window.setContentView(std::move(root));
        window.resize({400, 100});
        window.setScale(scale);
        window.show();
        window.grab();
    }
};

void expectRect(const RectF& rect, RectF expected)
{
    EXPECT_EQ(rect.x, expected.x);
    EXPECT_EQ(rect.y, expected.y);
    EXPECT_EQ(rect.width, expected.width);
    EXPECT_EQ(rect.height, expected.height);
}

} // namespace

TEST(TextMetricsTest, LinesKeepReferenceMetricsWhenScaled)
{
    struct Size
    {
        double pixelSize;
        double ascent;
        double descent;
        double lineHeight;
        double xHeight;
        double width;
    };
    const Size sizes[] = {
        {11, 11.765625, 3.21875, 15, 5.890625, 81.5938},
        {12, 12.828125, 3.515625, 17, 6.4375, 89.0},
        {15, 16.03125, 4.390625, 21, 8.046875, 111.25},
        {24, 25.65625, 7.03125, 33, 12.859375, 178.125},
    };
    for (const Size& size : sizes) {
        Measured measured(size.pixelSize, 1.5, "System Monitor");
        EXPECT_DOUBLE_EQ(measured.label->ascent(), size.ascent) << size.pixelSize;
        EXPECT_DOUBLE_EQ(measured.label->descent(), size.descent) << size.pixelSize;
        EXPECT_DOUBLE_EQ(measured.label->xHeight(), size.xHeight) << size.pixelSize;
        EXPECT_DOUBLE_EQ(measured.label->sizeHint().height, size.lineHeight) << size.pixelSize;
        EXPECT_NEAR(measured.label->sizeHint().width, size.width, 0.02) << size.pixelSize;
    }
}

TEST(TextMetricsTest, PointSizesRoundToWholePixels)
{
    // 8 pt is 10.67 logical pixels, laid out at 11.
    Measured measured(8 * 4.0 / 3, 1.5, "System Monitor");
    EXPECT_DOUBLE_EQ(measured.label->ascent(), 11.765625);
    EXPECT_NEAR(measured.label->sizeHint().width, 81.5938, 0.02);
}

TEST(TextMetricsTest, WeightsKeepTheirAdvances)
{
    EXPECT_NEAR(Measured(11, 1.5, "System Monitor", 700).label->sizeHint().width, 86.9375, 0.02);
    EXPECT_NEAR(Measured(11, 1.5, "Name", 500).label->sizeHint().width, 31.5625, 0.02);
}

TEST(TextMetricsTest, TightBoundingRectsRoundToWholePixels)
{
    expectRect(Measured(11, 1.5, "System Monitor").label->tightBoundingRect(), {0, -9, 83, 12});
    expectRect(Measured(11, 1.5, "Name").label->tightBoundingRect(), {1, -8, 30, 9});
    expectRect(Measured(11, 1.5, "H").label->tightBoundingRect(), {1, -8, 7, 8});
    expectRect(Measured(12, 1.5, "Name").label->tightBoundingRect(), {1, -9, 34, 10});
    expectRect(Measured(12, 1.5, "Applications").label->tightBoundingRect(), {0, -10, 70, 13});
    expectRect(Measured(15, 1.5, "16 logical processors").label->tightBoundingRect(), {1, -12, 146, 16});
    expectRect(Measured(24, 1.5, "Name").label->tightBoundingRect(), {2, -18, 66, 19});
    EXPECT_EQ(Measured(11, 1.5, "Text").label->capHeight(), 8);
}

TEST(TextMetricsTest, InkBoundingRectsKeepFractionsWithinTheWholePixelOnes)
{
    for (const double scale : {1.0, 1.5, 2.0}) {
        for (const std::string text : {"H", "42", "99+"}) {
            const Measured measured(13, scale, text, 600);
            const RectF ink = measured.label->inkBoundingRect();
            const RectF tight = measured.label->tightBoundingRect();
            EXPECT_GT(ink.width, 0) << text << " @" << scale;
            EXPECT_GE(ink.x, tight.x - 0.5) << text << " @" << scale;
            EXPECT_LE(ink.x + ink.width, tight.x + tight.width + 0.5) << text << " @" << scale;
            EXPECT_GE(ink.y, tight.y - 0.5) << text << " @" << scale;
            EXPECT_LE(ink.y + ink.height, tight.y + tight.height + 0.5) << text << " @" << scale;
        }
    }
    // A capital sits on the baseline: its ink ends there.
    const Measured capital(24, 2, "H");
    const RectF ink = capital.label->inkBoundingRect();
    EXPECT_NEAR(ink.y + ink.height, 0, 0.01);
}

TEST(TextMetricsTest, WholePixelMetricsUnscaled)
{
    // Unscaled, metrics are hinted with fontconfig's full hinting and rounded.
    struct Size
    {
        double pixelSize;
        double ascent;
        double descent;
        double lineHeight;
    };
    for (const Size& size : {Size{11, 12, 3, 15}, Size{12, 13, 4, 17}, Size{15, 16, 4, 20}}) {
        Measured measured(size.pixelSize, 1, "System Monitor");
        EXPECT_DOUBLE_EQ(measured.label->ascent(), size.ascent) << size.pixelSize;
        EXPECT_DOUBLE_EQ(measured.label->descent(), size.descent) << size.pixelSize;
        EXPECT_DOUBLE_EQ(measured.label->sizeHint().height, size.lineHeight) << size.pixelSize;
    }
}

TEST(TextMetricsTest, MediumWeightUsesTheMediumFace)
{
    // Weight 500 picks NotoSans-Medium.
    EXPECT_NEAR(Measured(12, 1.5, "Today", 500).label->sizeHint().width, 34.8281, 0.02);
}

TEST(TextMetricsTest, SpansCountTheirTextOnce)
{
    Measured measured(24, 1.5, "");
    measured.label->setSpans({{"October", 700, {}}, {" 2026", 0, {}}});
    measured.window.grab();
    // Styled text "<b>October</b> 2026".
    EXPECT_NEAR(measured.label->sizeHint().width, 158.03, 0.05);
}

TEST(TextMetricsTest, FallbackRunsKeepReferenceMetrics)
{
    // Han text, alone and beside Latin, set from the fallback face at 12 px,
    // scale 1.5: the lines keep the height and baseline of Noto Sans.
    struct Line
    {
        const char* text;
        double width;
    };
    const Line lines[] = {
        {"Settings \u8bbe\u7f6e", 72.7188},
        {"\u6587\u4ef6\u7ba1\u7406\u5668 Files", 88.0156},
        {"\u7cfb\u7edf\u8bbe\u7f6e\u4e2d\u5fc3", 72.0},
    };
    for (const Line& line : lines) {
        Measured measured(12, 1.5, line.text);
        EXPECT_NEAR(measured.label->sizeHint().width, line.width, 0.05) << line.text;
        EXPECT_DOUBLE_EQ(measured.label->sizeHint().height, 18) << line.text;
        EXPECT_NEAR(measured.label->baselineOffset(), 13.9219, 1e-4) << line.text;
    }
}
