// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

#include "ui/image.h"
#include "ui/painter.h"
#include "ui/test_input.h"
#include "ui/window.h"

using namespace cutefish::ui;

static constexpr uint64_t kFrameNs = 16666667;

namespace {

// Black and white stripes across the window; with `band`, a red band
// across their top.
class Stripes : public View
{
public:
    bool band = false;

protected:
    void paint(Painter& painter) override
    {
        painter.fillRect(localRect(), Color::rgb(0xFFFFFF));
        for (double x = 0; x < geometry().width; x += 8)
            painter.fillRect({x, 0, 4, geometry().height}, Color::rgb(0x000000));
        if (band)
            painter.fillRect({0, 0, geometry().width, 44}, Color::rgb(0xFF0000));
    }
};

// A translucent veil over what lies under it, with a dark mark.
class Veil : public View
{
protected:
    void paint(Painter& painter) override
    {
        painter.fillRect(localRect(), Color::rgb(0xFFFFFF).withAlpha(0.3));
        painter.fillRect({4, 4, 6, 6}, Color::rgb(0x2050A0));
    }
};

struct Scene
{
    Window window;
    Stripes* stripes = nullptr;
    Veil* veil = nullptr;
};

void build(Scene& scene, double scale)
{
    auto root = std::make_unique<View>();
    scene.stripes = root->addChild<Stripes>();
    scene.stripes->setGeometry({0, 0, 200, 120});
    scene.veil = root->addChild<Veil>();
    scene.veil->setGeometry({40, 40, 120, 60});
    scene.veil->setLayered(true);
    scene.window.resize({200, 120});
    scene.window.setContentView(std::move(root));
    scene.window.setScale(scale);
    scene.window.show();
}

int maxDifference(const Image& a, const Image& b, int x0, int y0, int x1, int y1)
{
    int largest = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Color p = a.pixel(x, y);
            const Color q = b.pixel(x, y);
            largest = std::max({largest, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b),
                                std::abs(p.a - q.a)});
        }
    }
    return largest;
}

// How far the green channel ranges along a row, in device pixels.
int greenRange(const Image& image, int x0, int x1, int y)
{
    int low = 255;
    int high = 0;
    for (int x = x0; x < x1; ++x) {
        low = std::min<int>(low, image.pixel(x, y).g);
        high = std::max<int>(high, image.pixel(x, y).g);
    }
    return high - low;
}

const std::vector<View::BackdropArea> kAreas = {{{20, 10, 80, 40}, 8}};

} // namespace

// A layered view without backdrop areas composites exactly as before: as
// one that never had any, and as one whose areas were taken away.
TEST(BackdropTest, NoAreasCompositeTheTextureAlone)
{
    for (const double scale : {1.0, 1.5}) {
        Scene plain;
        build(plain, scale);
        uint64_t now = 1000000000;
        const Image before = TestInput::presentFrame(plain.window, now += kFrameNs);

        Scene cleared;
        build(cleared, scale);
        uint64_t then = 1000000000;
        TestInput::presentFrame(cleared.window, then += kFrameNs);
        cleared.veil->setBackdrop(kAreas, 10, 1.8);
        TestInput::presentFrame(cleared.window, then += kFrameNs);
        cleared.veil->setBackdrop({}, 10);
        const Image after = TestInput::presentFrame(cleared.window, then += kFrameNs);
        EXPECT_EQ(0, maxDifference(before, after, 0, 0, before.width(), before.height())) << scale;
    }
}

// Under the areas the stripes run together; beside them they stay sharp,
// and the view's own paint goes over the blur.
TEST(BackdropTest, AreasBlurWhatLiesUnderTheView)
{
    for (const double scale : {1.0, 1.5}) {
        Scene scene;
        build(scene, scale);
        scene.veil->setBackdrop(kAreas, 10);
        uint64_t now = 1000000000;
        const Image shown = TestInput::presentFrame(scene.window, now += kFrameNs);
        const auto px = [scale](double logical) { return int(logical * scale); };
        // Inside the area, the veil's middle; outside it, the window's bottom.
        EXPECT_LT(greenRange(shown, px(80), px(120), px(70)), 40) << scale;
        EXPECT_GT(greenRange(shown, px(80), px(120), px(110)), 200) << scale;
        // Beside the area, inside the veil: no blur.
        EXPECT_GT(greenRange(shown, px(42), px(58), px(95)), 120) << scale;
        // The mark the veil paints stays over the blur.
        const Color mark = shown.pixel(px(46), px(46));
        EXPECT_LT(mark.r, 0x50) << scale;
    }
}

// What changes as far around an area as its blur reaches is blurred again:
// the frame matches one composited from scratch.
TEST(BackdropTest, ChangesUnderTheAreaBlurItAgain)
{
    for (const double scale : {1.0, 1.5}) {
        Scene scene;
        build(scene, scale);
        scene.veil->setBackdrop(kAreas, 10);
        uint64_t now = 1000000000;
        // Past the platform's fresh buffers, so the next frame paints only
        // what changed.
        for (int i = 0; i < 4; ++i)
            TestInput::presentFrame(scene.window, now += kFrameNs);
        // The band ends above the area, within the blur's reach of it.
        scene.stripes->band = true;
        scene.stripes->requestPaint({0, 0, 200, 44});
        const Image changed = TestInput::presentFrame(scene.window, now += kFrameNs);

        Scene fresh;
        build(fresh, scale);
        fresh.stripes->band = true;
        fresh.veil->setBackdrop(kAreas, 10);
        uint64_t then = 1000000000;
        const Image whole = TestInput::presentFrame(fresh.window, then += kFrameNs);
        EXPECT_LE(maxDifference(changed, whole, 0, 0, whole.width(), whole.height()), 2) << scale;
    }
}
