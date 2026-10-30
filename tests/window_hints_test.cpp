// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "ui/header_button.h"
#include "ui/layout.h"
#include "ui/painter.h"
#include "ui/path.h"
#include "ui/scroll_bar.h"
#include "ui/status_item.h"
#include "ui/subsurface.h"
#include "ui/test_input.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_header.h"

using namespace cutefish::ui;

TEST(SubsurfaceTest, HeadlessWindowsHaveNone)
{
    Window window;
    window.resize({200, 100});
    window.setContentView(std::make_unique<View>());
    window.show();
    Subsurface subsurface(&window);
    EXPECT_FALSE(subsurface.isValid());
    subsurface.setGeometry({0, 0, 200, 100});
    EXPECT_EQ(subsurface.geometry(), (RectF{0, 0, 200, 100}));
    EXPECT_TRUE(subsurface.beginFrame().isEmpty());
    EXPECT_FALSE(subsurface.endFrame());
    EXPECT_FALSE(subsurface.waitForFrame(0));
    EXPECT_FALSE(subsurface.makeCurrent());
    EXPECT_EQ(Subsurface::waylandDisplay(), nullptr);
    // A frame of the window still renders.
    EXPECT_GT(window.grab().width(), 0);
}

TEST(WindowHintsTest, GlassAndShapeAreAcceptedEverywhere)
{
    Window window;
    window.resize({300, 200});
    window.setContentView(std::make_unique<View>());
    window.show();
    window.setBackgroundBlur({0, 0, 300, 200}, 0);
    Path shape;
    shape.addRoundedRect({10, 120, 280, 70}, 12, 12);
    shape.addRoundedRect({200, 10, 90, 100}, 12, 12);
    window.setBackgroundBlurShape(shape);
    GlassMaterial material;
    material.blurSigma = 40;
    material.displacement = 0;
    material.chroma = 0;
    material.saturation = 0.65;
    material.tintRed = 0.07;
    material.tintGreen = 0.07;
    material.tintBlue = 0.08;
    material.tintAlpha = 0.62;
    window.setBackgroundGlass(material);
    window.setBackgroundGlass(std::nullopt);
    window.setResizeAroundCenter(true);
    window.setRestoreId("main");
    window.setPlacementAnchor(PlacementAnchor{{700, 12, 0, 0}, PopupAnchor::Bottom, PopupAnchor::Bottom});
    window.setPlacementAnchor(std::nullopt);
    EXPECT_GT(window.grab().width(), 0);
}

// The close button's plate, sampled at its middle.
static Color closeGlyphColor(Window& window, double headerHeight)
{
    const Image image = window.grab();
    const int x = int(7 + 13);
    const int y = int(headerHeight == 44 ? 9 + 13 : 7 + 13);
    return image.pixel(x, y);
}

TEST(WindowHintsTest, HeaderDarkModeTintsTheButtonsWhateverTheTheme)
{
    const bool wasDark = Theme::current().darkMode;
    Window window;
    auto column = std::make_unique<Column>();
    WindowHeader* header = column->addChild<WindowHeader>(44);
    header->setButtonsCentered(true);
    header->setLayoutHints({.fillWidth = true});
    window.resize({300, 200});
    window.setBackgroundColor(Color::rgb(0x808080));
    window.setContentView(std::move(column));
    window.show();
    // The plates are coloured while active; the tint shows on the inactive grey ones.
    TestInput::configure(window, 300, 200, {});

    EXPECT_EQ(window.headerDarkMode(), wasDark);
    window.setHeaderDarkMode(true);
    EXPECT_TRUE(window.headerDarkMode());
    const Color dark = closeGlyphColor(window, 44);
    window.setHeaderDarkMode(false);
    EXPECT_FALSE(window.headerDarkMode());
    const Color light = closeGlyphColor(window, 44);
    // A lighter plate on the grey for a dark header, a darker one for a light one.
    EXPECT_GT(dark.r, 0x90);
    EXPECT_LT(light.r, 0x78);
    window.setHeaderDarkMode(std::nullopt);
    EXPECT_EQ(window.headerDarkMode(), wasDark);
}

TEST(WindowHintsTest, CenteredHeaderButtons)
{
    Window window;
    auto column = std::make_unique<Column>();
    WindowHeader* header = column->addChild<WindowHeader>(44);
    header->setButtonsCentered(true);
    header->setLayoutHints({.fillWidth = true});
    window.resize({300, 200});
    window.setContentView(std::move(column));
    window.show();
    window.grab();
    // Close, minimize and maximize from the start, 7 px from it, 9 px down,
    // in one row hovered as a whole.
    const std::string dump = window.layoutDump();
    EXPECT_NE(dump.find("logical 7 9 80 26"), std::string::npos) << dump;
    EXPECT_NE(dump.find("device 7 9 26 26"), std::string::npos) << dump;
    EXPECT_NE(dump.find("device 34 9 26 26"), std::string::npos) << dump;
    EXPECT_NE(dump.find("device 61 9 26 26"), std::string::npos) << dump;
}

TEST(WindowHintsTest, WindowControlGlyphsShowAcrossTheWholeRow)
{
    Window window;
    auto column = std::make_unique<Column>();
    WindowHeader* header = column->addChild<WindowHeader>(44);
    header->setButtonsCentered(true);
    header->setLayoutHints({.fillWidth = true});
    window.resize({300, 200});
    window.setContentView(std::move(column));
    window.show();
    // The middles of the close and minimize plates, where their glyphs ink.
    const auto centres = [&] {
        const Image image = window.grab();
        return std::pair{int(image.pixel(20, 22).r), int(image.pixel(47, 22).r)};
    };
    const auto [closeRest, minimizeRest] = centres();
    // The gap between close and minimize, over minimize, and over maximize.
    for (const double x : {33.5, 47.0, 74.0}) {
        SCOPED_TRACE(x);
        TestInput::movePointer(window, {x, 22});
        const auto [close, minimize] = centres();
        EXPECT_LT(close, closeRest - 40);
        EXPECT_LT(minimize, minimizeRest - 40);
    }
    TestInput::movePointer(window, {200, 22});
    EXPECT_EQ(centres(), std::pair(closeRest, minimizeRest));
}

TEST(WindowHintsTest, ScrollBarDarkModeOverridesTheTheme)
{
    Window window;
    window.resize({40, 200});
    window.setBackgroundColor(Color::rgb(0x000000));
    auto bar = std::make_unique<ScrollBar>(ScrollBar::Orientation::Vertical);
    ScrollBar* scrollBar = bar.get();
    window.setContentView(std::move(bar));
    window.show();
    scrollBar->setRange(100, 200, 0);
    scrollBar->setAlwaysShown(true);
    // Let the bar fade in.
    for (int frame = 0; frame < 120; ++frame)
        window.grab(uint64_t(frame) * 16666667 + 1000000000);
    scrollBar->setDarkMode(true);
    const Color light = window.grab(5000000000).pixel(4, 40);
    scrollBar->setDarkMode(false);
    const Color dark = window.grab(5000000000).pixel(4, 40);
    // A white handle shows on the black background; a black one does not.
    EXPECT_GT(light.r, 0x60);
    EXPECT_LT(dark.r, 0x10);
}

TEST(WindowHintsTest, HeaderButtonGlyphColour)
{
    Window window;
    window.resize({32, 32});
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    auto button = std::make_unique<HeaderButton>("close");
    HeaderButton* close = button.get();
    window.setContentView(std::move(button));
    window.show();
    close->setGlyphColor(Color::rgb(0xFF0000));
    const Color red = window.grab().pixel(16, 16);
    EXPECT_GT(red.r, 0xC0);
    EXPECT_LT(red.g, 0x80);
    close->setGlyphColor(std::nullopt);
    const Color tint = window.grab().pixel(16, 16);
    EXPECT_LT(std::abs(int(tint.r) - int(tint.b)), 0x20);
}

namespace {

// Fills itself in a colour, where the item's artwork would be.
class Swatch final : public View
{
public:
    Color color {0, 0, 0, 255};

protected:
    void paint(Painter& painter) override { painter.fillRect(localRect(), color); }
};

} // namespace

// Headless, an item is a window of the size it asks for in a light bar; the
// highlight it asks for is the highlight it has.
TEST(StatusItemTest, HeadlessItemsDrawAtTheirSizeAndHighlightWhenAsked)
{
    EXPECT_FALSE(StatusItem::isSupported());
    StatusItem item;
    auto swatch = std::make_unique<Swatch>();
    Swatch* content = swatch.get();
    item.setContentView(std::move(swatch));
    item.setWidth(57.4);
    EXPECT_DOUBLE_EQ(57, item.width());
    EXPECT_GT(item.height(), 0);
    int highlights = 0;
    item.onHighlightChanged = [&] { ++highlights; };
    item.show();
    EXPECT_FALSE(item.darkAppearance());
    EXPECT_EQ((Color{0, 0, 0, 217}), item.foregroundColor());
    EXPECT_EQ(0, item.screenRect().width);

    content->color = item.foregroundColor();
    const Image image = item.window().grab();
    EXPECT_EQ(57, image.width());
    EXPECT_EQ(int(item.height()), image.height());
    EXPECT_EQ(217, image.pixel(28, image.height() / 2).a);

    item.setHighlighted(true);
    EXPECT_TRUE(item.isHighlighted());
    EXPECT_EQ(1, highlights);
    item.setHighlighted(false);
    EXPECT_FALSE(item.isHighlighted());

    // A popup hangs centred under the whole item and never flips above it.
    const PopupPlacement placement = item.popupPlacement(4);
    EXPECT_EQ((RectF{0, 0, 57, item.height()}), placement.anchorRect);
    EXPECT_EQ(PopupAnchor::Bottom, placement.anchor);
    EXPECT_EQ(PopupAnchor::Bottom, placement.gravity);
    EXPECT_EQ((PointF{0, 4}), placement.offset);
    EXPECT_FALSE(placement.flipY);
}
