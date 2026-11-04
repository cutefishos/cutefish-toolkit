// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "test_views.h"
#include "ui/keys.h"
#include "ui/layout.h"
#include "ui/menu.h"
#include "ui/popup_glass.h"
#include "ui/test_input.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/window.h"

using namespace cutefish::ui;

namespace {

struct Fixture
{
    Fixture()
    {
        auto content = std::make_unique<Column>();
        anchor = content->addChild<test::Box>(SizeF{80, 30});
        window.resize({400, 300});
        window.setContentView(std::move(content));
        window.show();
        window.grab();

        menu.addItem("Open", [this] { chosen.push_back("Open"); }, "Ctrl+O");
        disabled = menu.addItem("Save", [this] { chosen.push_back("Save"); });
        menu.setItemEnabled(disabled, false);
        menu.addSeparator();
        menu.addItem("Quit", [this] { chosen.push_back("Quit"); });
        menu.onClosed = [this] { ++closed; };
    }

    Window window;
    test::Box* anchor;
    Menu menu;
    size_t disabled = 0;
    std::vector<std::string> chosen;
    int closed = 0;
};

} // namespace

TEST(MenuTest, KeyboardSkipsDisabledRowsAndSeparators)
{
    Fixture f;
    f.menu.popup(&f.window, {20, 20});
    ASSERT_TRUE(f.menu.isOpen());
    Window* popup = f.menu.window();
    popup->grab();
    TestInput::setKeyboardFocus(*popup, true);
    TestInput::typeKey(*popup, key::Down);
    TestInput::typeKey(*popup, key::Down);
    TestInput::typeKey(*popup, key::Return);
    EXPECT_EQ(f.chosen, (std::vector<std::string>{"Quit"}));
    EXPECT_FALSE(f.menu.isOpen());
    EXPECT_EQ(f.closed, 1);
}

// Keys after the choice, before the closed popup goes, choose nothing more.
TEST(MenuTest, KeysAfterChoosingReachNoClosedMenu)
{
    Fixture f;
    f.menu.popup(&f.window, {20, 20});
    ASSERT_TRUE(f.menu.isOpen());
    f.menu.window()->grab();
    TestInput::setKeyboardFocus(f.window, true);
    TestInput::typeKey(f.window, key::Down);
    TestInput::typeKey(f.window, key::Return);
    EXPECT_FALSE(f.menu.isOpen());
    TestInput::typeKey(f.window, key::Down);
    TestInput::typeKey(f.window, key::Return);
    EXPECT_EQ(f.chosen, (std::vector<std::string>{"Open"}));
}

// Measured before it opens, the menu is the size it opens at, at whole and
// fractional scales, with hinted and unhinted text alike.
TEST(MenuTest, ImplicitSizeIsTheOpenedSize)
{
    for (const bool hinted : {true, false}) {
        for (const double scale : {1.0, 1.5}) {
            Fixture f;
            f.window.setScale(scale);
            f.window.setHintedTextLayout(hinted);
            f.menu.addItem("Capture Selected Window", [] {}, "Ctrl+Shift+W");
            const SizeF size = f.menu.implicitSize(&f.window);
            f.menu.popup(&f.window, {20, 20});
            ASSERT_TRUE(f.menu.isOpen());
            SCOPED_TRACE(std::to_string(scale) + (hinted ? " hinted" : " unhinted"));
            EXPECT_EQ(size, f.menu.window()->size());
            f.menu.close();
        }
    }
}

TEST(MenuTest, RowsHaveThemeMetrics)
{
    Fixture f;
    f.menu.popupFor(f.anchor);
    Window* popup = f.menu.window();
    popup->grab();
    // Three rows of 26 and one separator of 12, five pixels above and
    // below.
    EXPECT_EQ(popup->size().height, 3 * 26 + 12 + 2 * 5);
    EXPECT_GE(popup->size().width, 158 + 2 * 5);
}

// A drop-down: half the screen tall at most, opened centred on the checked
// row, nothing lit from the pointer; the arrows go on from the checked row
// and stop at the ends; typing finds a row.
TEST(MenuTest, DropDownScrollsToItsCheckedRow)
{
    Window window;
    window.setScreenSize({1280, 800});
    window.setContentView(std::make_unique<View>());
    window.resize({400, 300});
    window.show();
    Menu menu;
    menu.setDropDown(true);
    std::vector<std::string> chosen;
    for (int i = 0; i < 40; ++i) {
        const std::string text = (i == 30 ? "Zulu " : "Row ") + std::to_string(i);
        const size_t index = menu.addItem(text, [&chosen, text] { chosen.push_back(text); });
        menu.setItemChecked(index, i == 20);
    }
    menu.popup(&window, {10, 10});
    Window* popup = menu.window();
    popup->grab();
    // (800 - 12) / 2, rounded.
    EXPECT_EQ(popup->size().height, 394);
    TestInput::setKeyboardFocus(*popup, true);
    TestInput::typeKey(*popup, key::Down);
    TestInput::typeKey(*popup, key::Return);
    EXPECT_EQ(chosen, (std::vector<std::string>{"Row 21"}));

    menu.popup(&window, {10, 10});
    popup = menu.window();
    popup->grab();
    TestInput::setKeyboardFocus(*popup, true);
    TestInput::typeKey(*popup, key::letter('z'), NoModifier, "z");
    TestInput::typeKey(*popup, key::Return);
    EXPECT_EQ(chosen.back(), "Zulu 30");
}

// A drop-down's arrows, Home, End and typing pass over separators and
// disabled rows, as a desktop menu's arrows do.
TEST(MenuTest, DropDownKeysSkipSeparatorsAndDisabledRows)
{
    Window window;
    window.setScreenSize({1280, 800});
    window.setContentView(std::make_unique<View>());
    window.resize({400, 300});
    window.show();
    Menu menu;
    menu.setDropDown(true);
    std::vector<std::string> chosen;
    const auto add = [&menu, &chosen](const std::string& text, bool enabled) {
        const size_t index = menu.addItem(text, [&chosen, text] { chosen.push_back(text); });
        menu.setItemEnabled(index, enabled);
    };
    add("Alpha", true);
    menu.addSeparator();
    add("Beta", false);
    add("Gamma", true);
    add("Bravo", true);
    add("Delta", false);

    const auto open = [&menu, &window] {
        menu.popup(&window, {10, 10});
        Window* popup = menu.window();
        popup->grab();
        TestInput::setKeyboardFocus(*popup, true);
        return popup;
    };
    Window* popup = open();
    TestInput::typeKey(*popup, key::Down);
    TestInput::typeKey(*popup, key::Down);
    TestInput::typeKey(*popup, key::Return);
    ASSERT_EQ(chosen, (std::vector<std::string>{"Gamma"}));

    popup = open();
    TestInput::typeKey(*popup, key::End);
    TestInput::typeKey(*popup, key::Return);
    EXPECT_EQ(chosen.back(), "Bravo");

    popup = open();
    TestInput::typeKey(*popup, key::letter('b'), NoModifier, "b");
    TestInput::typeKey(*popup, key::Return);
    EXPECT_EQ(chosen.back(), "Bravo");
    EXPECT_EQ(chosen.size(), 3u);
}

TEST(MenuTest, ClickChoosesTheRowUnderThePointer)
{
    Fixture f;
    f.menu.popup(&f.window, {20, 20});
    Window* popup = f.menu.window();
    popup->grab();
    // The second row is disabled; a click there does nothing.
    TestInput::click(*popup, {40, 5 + 26 + 13});
    EXPECT_TRUE(f.chosen.empty());
    EXPECT_TRUE(f.menu.isOpen());
    TestInput::click(*popup, {40, 5 + 13});
    EXPECT_EQ(f.chosen, (std::vector<std::string>{"Open"}));
}

TEST(MenuTest, HoverLightsTheRowUnderThePointer)
{
    Fixture f;
    f.menu.popup(&f.window, {20, 20});
    Window* popup = f.menu.window();
    popup->grab();
    // No button held: the row lights from hover alone, and Return takes it.
    TestInput::movePointer(*popup, {40, 5 + 26 + 26 + 12 + 13});
    TestInput::typeKey(*popup, key::Return);
    EXPECT_EQ(f.chosen, (std::vector<std::string>{"Quit"}));
}

TEST(MenuTest, EscapeAndDismissalClose)
{
    Fixture f;
    f.menu.popup(&f.window, {20, 20});
    TestInput::typeKey(*f.menu.window(), key::Escape);
    EXPECT_FALSE(f.menu.isOpen());
    EXPECT_TRUE(f.chosen.empty());
}

// Without the compositor's blur the bubble is the second background,
// opaque; no glass is asked for.
TEST(MenuTest, OpaqueWithoutGlass)
{
    Fixture f;
    f.menu.popup(&f.window, {20, 20});
    Window* popup = f.menu.window();
    const Image image = popup->grab();
    // Above the first row, clear of the corners.
    const Color margin = image.pixel(image.width() / 2, 2);
    EXPECT_EQ(margin.a, 255);
    EXPECT_NEAR(margin.r, Theme::current().secondBackground.r, 1);
    EXPECT_EQ(image.pixel(0, 0).a, 0);
    EXPECT_FALSE(TestInput::backgroundGlass(*popup));
}

// With the compositor's glass: the popup glass in the theme's tint, no fill
// of its own, and the tint follows the theme while the menu is open.
TEST(MenuTest, PopupGlassFollowsTheTheme)
{
    TestInput::setGlassAvailable(true);
    const Theme original = Theme::current();
    Theme::setCurrent(Theme::light());
    {
        Fixture f;
        f.menu.popup(&f.window, {20, 20});
        Window* popup = f.menu.window();
        const Image image = popup->grab();
        EXPECT_EQ(image.pixel(image.width() / 2, 2).a, 0);
        const std::optional<GlassMaterial> glass = TestInput::backgroundGlass(*popup);
        ASSERT_TRUE(glass);
        EXPECT_EQ(*glass, popupGlass(false));
        EXPECT_EQ(glass->blurSigma, 6);
        EXPECT_EQ(glass->displacement, 20);
        EXPECT_EQ(glass->chroma, 0.03);
        EXPECT_EQ(glass->saturation, 1.2);
        EXPECT_EQ(glass->rim, 0);
        EXPECT_EQ(glass->tintRed, 1);
        EXPECT_EQ(glass->tintAlpha, 0.6);
        // The rest is the compositor's.
        EXPECT_EQ(glass->bevel, GlassMaterial().bevel);
        EXPECT_EQ(glass->ior, GlassMaterial().ior);

        Theme::setCurrent(Theme::dark());
        popup->grab();
        const std::optional<GlassMaterial> dark = TestInput::backgroundGlass(*popup);
        ASSERT_TRUE(dark);
        EXPECT_EQ(*dark, popupGlass(true));
        EXPECT_EQ(dark->tintRed, 0.17);
        EXPECT_EQ(dark->tintBlue, 0.19);
        EXPECT_EQ(dark->tintAlpha, 0.6);
    }
    Theme::setCurrent(original);
    TestInput::setGlassAvailable(false);
}

// Over glass the text sits on transparent pixels: no glyph may come out
// darker than the text colour once composited, as Skia's subpixel masks make
// it when they take the target for opaque.
TEST(MenuTest, TextOverGlassKeepsItsWeight)
{
    TestInput::setGlassAvailable(true);
    const Theme original = Theme::current();
    Theme::setCurrent(Theme::light());
    for (const double scale : {1.0, 1.5}) {
        SCOPED_TRACE(scale);
        Fixture f;
        f.menu.popup(&f.window, {20, 20});
        Window* popup = f.menu.window();
        popup->setScale(scale);
        const Image image = popup->grab();
        const Color text = Theme::current().text;
        int darkest = 255;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                // Premultiplied, over white.
                const Color pixel = image.pixel(x, y);
                darkest = std::min({darkest, pixel.r + 255 - pixel.a, pixel.g + 255 - pixel.a,
                                    pixel.b + 255 - pixel.a});
            }
        }
        EXPECT_GE(darkest, std::min({text.r, text.g, text.b}) - 1);
        EXPECT_LT(darkest, 128);
    }
    Theme::setCurrent(original);
    TestInput::setGlassAvailable(false);
}

// Over glass a menu's glyphs come out as over its fill: the chevron and the
// icon match the menu drawn on the fill once laid over it, and the text is
// greyscale in its own colour, as subpixel coverage needs an opaque target.
TEST(MenuTest, GlyphsOverGlassKeepTheirColour)
{
    const Theme original = Theme::current();
    for (const bool dark : {false, true}) {
        SCOPED_TRACE(dark);
        Theme::setCurrent(dark ? Theme::dark() : Theme::light());
        const auto render = [](bool glass) {
            TestInput::setGlassAvailable(glass);
            Window window;
            window.setContentView(std::make_unique<Column>());
            window.resize({400, 300});
            window.show();
            window.grab();
            Menu menu;
            const size_t plain = menu.addItem("Plain", [] {});
            menu.setItemIcon(plain, "folder");
            menu.addSubmenu("More")->addItem("Inside", [] {});
            menu.popup(&window, {20, 20});
            Window* popup = menu.window();
            popup->setScale(1.5);
            Image image = popup->grab();
            TestInput::setGlassAvailable(false);
            return image;
        };
        const Image glass = render(true);
        const Image fill = render(false);
        ASSERT_EQ(glass.width(), fill.width());
        ASSERT_EQ(glass.height(), fill.height());
        const Color background = Theme::current().secondBackground;
        const Color text = Theme::current().text;
        // The first row's icon at its start, the second row's chevron at its
        // end, labels elsewhere; device pixels at 1.5, clear of the corners.
        const int iconEnd = 59;
        const int chevronStart = glass.width() - 50;
        const int half = glass.height() / 2;
        int glyphWorst = 0;
        int textWorst = 0;
        int chevronInk = 0;
        int iconInk = 0;
        for (int y = 12; y < glass.height() - 12; ++y) {
            for (int x = 12; x < glass.width() - 12; ++x) {
                const Color over = glass.pixel(x, y);
                const bool icon = x < iconEnd && y < half;
                const bool chevron = x >= chevronStart && y >= half;
                if (icon || chevron) {
                    // The premultiplied glass pixel laid over the fill.
                    const Color under = fill.pixel(x, y);
                    const auto channel = [&](uint8_t src, uint8_t back) { return src + back * (255 - over.a) / 255; };
                    glyphWorst = std::max({glyphWorst, std::abs(channel(over.r, background.r) - under.r),
                                           std::abs(channel(over.g, background.g) - under.g),
                                           std::abs(channel(over.b, background.b) - under.b)});
                    if (chevron && over.a == 255)
                        ++chevronInk;
                    if (icon && over.a == 255)
                        ++iconInk;
                } else if (over.a > 0) {
                    // Premultiplied greyscale text: the text colour times coverage.
                    const auto expected = [&](uint8_t channel) { return channel * over.a / 255; };
                    textWorst = std::max({textWorst, std::abs(over.r - expected(text.r)),
                                          std::abs(over.g - expected(text.g)), std::abs(over.b - expected(text.b))});
                }
            }
        }
        EXPECT_LE(glyphWorst, 3);
        EXPECT_GT(chevronInk, 0);
        EXPECT_GT(iconInk, 0);
        EXPECT_LE(textWorst, 2);
    }
    Theme::setCurrent(original);
}

// In dark mode the submenu chevron takes its label's colour; in light mode
// it stays the dimmer grey.
TEST(MenuTest, DarkChevronTakesTheLabelColour)
{
    const Theme original = Theme::current();
    for (const bool dark : {false, true}) {
        SCOPED_TRACE(dark);
        Theme::setCurrent(dark ? Theme::dark() : Theme::light());
        Window window;
        window.setContentView(std::make_unique<Column>());
        window.resize({400, 300});
        window.show();
        window.grab();
        Menu menu;
        menu.addSubmenu("More")->addItem("Inside", [] {});
        menu.popup(&window, {20, 20});
        // At 2 the chevron's strokes cover whole device pixels.
        menu.window()->setScale(2);
        const Image image = menu.window()->grab();
        // The opaque pixel nearest the text colour in the chevron's end of
        // the row; the fill sits far from either colour.
        const Color text = Theme::current().text;
        const Color grey = Theme::current().disabledText;
        int nearestText = 255;
        int nearestGrey = 255;
        for (int y = 16; y < image.height() - 16; ++y) {
            for (int x = image.width() - 60; x < image.width() - 16; ++x) {
                const Color pixel = image.pixel(x, y);
                if (pixel.a != 255)
                    continue;
                const auto distance = [&](const Color& to) {
                    return std::max({std::abs(pixel.r - to.r), std::abs(pixel.g - to.g), std::abs(pixel.b - to.b)});
                };
                nearestText = std::min(nearestText, distance(text));
                nearestGrey = std::min(nearestGrey, distance(grey));
            }
        }
        if (dark)
            EXPECT_LE(nearestText, 3);
        else
            EXPECT_LE(nearestGrey, 3);
    }
    Theme::setCurrent(original);
}

// A bubble with an arrow takes the same glass; the outline masks it.
TEST(MenuTest, BubbleMenuTakesThePopupGlass)
{
    TestInput::setGlassAvailable(true);
    Fixture f;
    f.menu.setArrow(BubbleShape::Edge::Top, {40, 30});
    f.menu.popup(&f.window, {20, 40});
    Window* popup = f.menu.window();
    popup->grab();
    EXPECT_EQ(TestInput::backgroundGlass(*popup), popupGlass(Theme::current().darkMode));
    TestInput::setGlassAvailable(false);
}

TEST(MenuTest, TextFieldOffersItsEditingMenu)
{
    Window window;
    auto content = std::make_unique<Column>();
    TextField* field = content->addChild<TextField>("hello");
    field->setLayoutHints({.fillWidth = true});
    window.resize({300, 100});
    window.setContentView(std::move(content));
    window.show();
    window.grab();
    TestInput::setKeyboardFocus(window, true);

    TestInput::click(window, {30, 15});
    TestInput::typeKey(window, key::letter('a'), ControlModifier);
    TestInput::press(window, {30, 15}, MouseButton::Right);
    TestInput::release(window, {30, 15}, MouseButton::Right);
    // The field keeps its selection while the menu is up.
    EXPECT_EQ(field->selectedText(), "hello");
}
