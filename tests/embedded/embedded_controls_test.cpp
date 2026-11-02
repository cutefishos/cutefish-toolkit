// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ui/animation.h"
#include "ui/embedded_window.h"
#include "ui/icon_image.h"
#include "ui/label.h"
#include "ui/styled_label.h"
#include "ui/text_field.h"
#include "ui/view.h"

using namespace cutefish::ui;
using namespace std::chrono_literals;

namespace {

class Free : public View
{
};

} // namespace

static int inkedPixels(const EmbeddedWindow::Frame& frame, int top, int bottom)
{
    int count = 0;
    for (int y = top; y < bottom && y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (frame.pixels[size_t(y) * frame.stride + size_t(x) * 4 + 3] > 0)
                ++count;
        }
    }
    return count;
}

// Reference text measured as the compositor's lock screen lays it out,
// unrounded at every scale (Noto Sans).
TEST(EmbeddedControlsTest, TextLaysOutAsTheOffscreenPlatform)
{
    struct Case
    {
        const char* text;
        double size;
        int weight;
        double width;
        double height;
        double baseline;
    };
    const Case cases[] = {
        {"tester", 13, 400, 36.6094, 18, 13.8906},
        {"tester", 16, 600, 46.375, 22, 17.1094},
        {"Thu Oct 15", 20, 600, 104.219, 28, 21.375},
        {"Enter Password", 13, 400, 94.7188, 18, 13.8906},
    };
    for (const double scale : {1.0, 1.5}) {
        EmbeddedWindow window;
        // The lock screen's placement: unrounded.
        window.setHintedTextLayout(false);
        auto content = std::make_unique<Free>();
        std::vector<Label*> labels;
        for (const Case& item : cases) {
            Label* label = content->addChild<Label>(item.text);
            label->setFontFamily("Noto Sans");
            label->setFontSize(item.size);
            label->setWeight(item.weight);
            labels.push_back(label);
        }
        window.setContentView(std::move(content));
        window.setScale(scale);
        window.setSize({300, 100});
        window.updateLayout();
        for (size_t i = 0; i < labels.size(); ++i) {
            const SizeF size = labels[i]->sizeHint();
            EXPECT_NEAR(size.width, cases[i].width, 1e-3) << cases[i].text << " at " << scale;
            EXPECT_NEAR(size.height, cases[i].height, 1e-3) << cases[i].text << " at " << scale;
            EXPECT_NEAR(labels[i]->baselineOffset(), cases[i].baseline, 1e-3) << cases[i].text << " at " << scale;
        }
    }
}

TEST(EmbeddedControlsTest, RaisedTextDrawsItsStyleBelow)
{
    int inked[2] = {};
    for (int raised = 0; raised < 2; ++raised) {
        EmbeddedWindow window;
        // The lock screen's placement: unrounded.
        window.setHintedTextLayout(false);
        auto content = std::make_unique<Free>();
        StyledLabel* label = content->addChild<StyledLabel>("Raised");
        label->setFontSize(16);
        label->setColor(Color::rgb(0xffffff));
        if (raised)
            label->setStyle(StyledLabel::Style::Raised, Color{0, 0, 0, 77});
        label->setGeometry({4, 4, 100, 22});
        window.setContentView(std::move(content));
        window.setSize({120, 40});
        inked[raised] = inkedPixels(window.render(), 0, 40);
        if (raised)
            EXPECT_EQ(label->visualRect().height, 23);
    }
    EXPECT_GT(inked[1], inked[0]);
}

TEST(EmbeddedControlsTest, BarePasswordInput)
{
    EmbeddedWindow window;
    // The lock screen's placement: unrounded.
    window.setHintedTextLayout(false);
    auto content = std::make_unique<Free>();
    TextField* field = content->addChild<TextField>();
    field->setGeometry({14, 0, 186, 32});
    field->setFrameVisible(false);
    field->setTextMargins(0, 0);
    field->setFadesWhenDisabled(false);
    field->setEchoMode(TextField::EchoMode::Password);
    field->setPasswordCharacter("•");
    field->setFontFamily("DejaVu Sans Mono");
    field->setFontSize(13);
    field->setTextColor(Color::rgb(0xffffff));
    field->setSelectionColors(Color::rgb(0x000080), Color::rgb(0xffffff));
    field->setCaretWidth(2);
    field->setSelectByMouse(false);
    field->setClipboardEnabled(false);
    field->setContextMenuEnabled(false);
    window.setContentView(std::move(content));
    window.setScale(1.5);
    window.setSize({220, 32});
    window.setActive(true);
    window.setKeyboardFocus(true);
    field->setFocus();

    const auto type = [&window](uint32_t keysym, std::string text, uint32_t modifiers = NoModifier) {
        KeyEvent event;
        event.key = keysym;
        event.text = std::move(text);
        event.modifiers = modifiers;
        window.key(event, true);
        window.key(event, false);
    };
    for (const char* letter : {"s", "e", "c", "r", "e", "t"})
        type(letter[0], letter);
    EXPECT_EQ(field->text(), "secret");
    EXPECT_FALSE(window.render().damage.empty());

    // No undo, copy or paste; keyboard selection and Ctrl+A still work.
    type(XKB_KEY_z, "\x1a", ControlModifier);
    EXPECT_EQ(field->text(), "secret");
    type(XKB_KEY_Left, "", ShiftModifier);
    EXPECT_EQ(field->selectedText(), "t");
    type(XKB_KEY_a, "\x01", ControlModifier);
    EXPECT_EQ(field->selectedText(), "secret");
    type(XKB_KEY_End, "");

    // Presses only move the cursor; a drag selects nothing.
    window.pointerEnter({16, 16});
    window.pointerButton(MouseButton::Left, true);
    window.pointerMove({150, 16});
    window.pointerButton(MouseButton::Left, false);
    EXPECT_FALSE(field->hasSelection());
    EXPECT_EQ(field->cursorPosition(), 0u);
    EXPECT_EQ(window.cursorShape(), CursorShape::Text);

    // A password is one word.
    type(XKB_KEY_End, "");
    type(XKB_KEY_BackSpace, "\b", ControlModifier);
    EXPECT_EQ(field->text(), "");

    type(XKB_KEY_x, "x");
    field->setEnabled(false);
    EXPECT_DOUBLE_EQ(field->opacity(), 1);
    field->setEnabled(true);
    field->clear();
    EXPECT_EQ(field->text(), "");
}

TEST(EmbeddedControlsTest, IconPixmapsByNameOrPath)
{
    char directory[] = "/tmp/cutefish-toolkit-icons-XXXXXX";
    ASSERT_NE(mkdtemp(directory), nullptr);
    const std::string path = std::string(directory) + "/square.svg";
    std::ofstream(path) << R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">)"
                        << R"(<rect width="16" height="16" fill="#ff0000"/></svg>)";

    EXPECT_TRUE(IconImage::preload(path, 48));
    EXPECT_FALSE(IconImage::preload("no-such-icon-anywhere", 48));

    EmbeddedWindow window;

    // The lock screen's placement: unrounded.
    window.setHintedTextLayout(false);
    auto content = std::make_unique<Free>();
    IconImage* icon = content->addChild<IconImage>("no-such-icon-anywhere", 48);
    icon->setFallbackName(path);
    icon->setGeometry({0, 0, 32, 32});
    window.setContentView(std::move(content));
    window.setScale(1.5);
    window.setSize({40, 40});
    const EmbeddedWindow::Frame& frame = window.render();
    const uint8_t* centre = frame.pixels + size_t(24) * frame.stride + 24 * 4;
    EXPECT_EQ(centre[0], 255);
    EXPECT_EQ(centre[3], 255);
    EXPECT_EQ(inkedPixels(frame, 0, frame.height), 48 * 48);

    // At its own size, on the rounded device pixel and unfiltered: 22
    // logical is 38.5 device at 1.75.
    EmbeddedWindow exact;
    auto row = std::make_unique<Free>();
    IconImage* whole = row->addChild<IconImage>(path, 168);
    whole->setGeometry({22, 2, 96, 96});
    exact.setContentView(std::move(row));
    exact.setScale(1.75);
    exact.setSize({140, 100});
    const EmbeddedWindow::Frame& placed = exact.render();
    const auto alphaAt = [&placed](int x, int y) { return placed.pixels[size_t(y) * placed.stride + size_t(x) * 4 + 3]; };
    EXPECT_EQ(alphaAt(38, 80), 0);
    EXPECT_EQ(alphaAt(39, 80), 255);
    EXPECT_EQ(alphaAt(39 + 167, 80), 255);
    EXPECT_EQ(alphaAt(39 + 168, 80), 0);
    EXPECT_EQ(alphaAt(80, 3), 0);
    EXPECT_EQ(alphaAt(80, 4), 255);

    std::remove(path.c_str());
    rmdir(directory);
}

TEST(EmbeddedControlsTest, QuadEasings)
{
    EmbeddedWindow window;
    // The lock screen's placement: unrounded.
    window.setHintedTextLayout(false);
    window.setContentView(std::make_unique<Free>());
    window.setSize({10, 10});
    const uint64_t start = 1000000000;
    for (const auto easing : {Animation::Easing::OutQuad, Animation::Easing::InQuad}) {
        Animation animation(&window);
        animation.setDuration(100ms);
        animation.setEasing(easing);
        double value = -1;
        animation.onUpdate = [&value](double v) { value = v; };
        animation.start();
        window.render(start);
        window.render(start + 50000000);
        EXPECT_DOUBLE_EQ(value, easing == Animation::Easing::OutQuad ? 0.75 : 0.25);
        animation.stop();
    }
}
