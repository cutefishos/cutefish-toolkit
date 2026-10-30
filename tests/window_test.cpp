// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/test_input.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "ui/window.h"
#include "ui/window_header.h"

using namespace cutefish::ui;

TEST(WindowTest, HeaderDoubleClickTogglesMaximized)
{
    Window window;
    auto column = std::make_unique<Column>();
    WindowHeader* header = column->addChild<WindowHeader>();
    header->setLayoutHints({.fillWidth = true});
    window.resize({400, 300});
    window.setContentView(std::move(column));
    window.show();
    window.grab();
    int changes = 0;
    window.onStatesChanged = [&changes] { ++changes; };

    TestInput::click(window, {250, 20});
    TestInput::click(window, {250, 20});
    EXPECT_TRUE(window.states().maximized);
    EXPECT_EQ(changes, 1);
    // The maximize button, third from the start, restores.
    TestInput::click(window, {9 + 2 * (22 + 1) + 11, 20});
    EXPECT_FALSE(window.states().maximized);
}

// A configure leaving the size to the window, once it is neither maximized,
// full screen nor tiled, brings back the size it had before.
TEST(WindowTest, UnconstrainedConfigureWithoutSizeRestoresTheNormalSize)
{
    for (const WindowStates constrained : {WindowStates{.maximized = true}, WindowStates{.fullScreen = true},
                                           WindowStates{.tiled = true}}) {
        Window window;
        window.setContentView(std::make_unique<View>());
        window.resize({400, 300});
        window.show();
        TestInput::configure(window, 0, 0, {});
        TestInput::configure(window, 1280, 760, constrained);
        EXPECT_EQ(window.size().width, 1280);
        TestInput::configure(window, 0, 0, {});
        EXPECT_EQ(window.size().width, 400);
        EXPECT_EQ(window.size().height, 300);
    }
}

// The headless platform answers maximize and full screen with a configure
// leaving the size to the window: the state holds, and the size comes back.
TEST(WindowTest, HeadlessMaximizeKeepsTheStateAndTheSize)
{
    Window window;
    window.setContentView(std::make_unique<View>());
    window.resize({400, 300});
    window.show();
    window.grab();

    window.setMaximized(true);
    window.grab();
    EXPECT_TRUE(window.states().maximized);
    window.setFullScreen(true);
    EXPECT_TRUE(window.states().maximized);
    EXPECT_TRUE(window.states().fullScreen);
    window.setFullScreen(false);
    EXPECT_TRUE(window.states().maximized);
    EXPECT_FALSE(window.states().fullScreen);
    window.setMaximized(false);
    window.grab();
    EXPECT_FALSE(window.states().maximized);
    EXPECT_EQ(window.size().width, 400);
    EXPECT_EQ(window.size().height, 300);
}

// FishUI puts the buttons' row in a RowLayout cell after a start margin item:
// the row starts on a whole logical pixel, a half rounding up, while the
// buttons keep the fractional top margin.
TEST(WindowTest, HeaderButtonsStartOnAWholePixel)
{
    for (const auto& [margin, rowX] : {std::pair{4.0, 9.0}, std::pair{9.375, 14.0}, std::pair{8.6, 14.0},
                                       std::pair{8.5, 14.0}, std::pair{8.4, 13.0}}) {
        Window window;
        auto column = std::make_unique<Column>();
        WindowHeader* header = column->addChild<WindowHeader>(48);
        header->setLayoutHints({.fillWidth = true});
        View* content = header->setContent<View>();
        header->setButtonMargin(margin);
        window.resize({1100, 300});
        window.setContentView(std::move(column));
        window.show();
        window.grab();
        // The buttons sit in one row that is hovered as a whole.
        std::vector<const View*> rows;
        for (const auto& child : header->children()) {
            if (child.get() != content)
                rows.push_back(child.get());
        }
        ASSERT_EQ(rows.size(), 1u);
        std::vector<RectF> buttons;
        for (const auto& child : rows[0]->children())
            buttons.push_back(child->geometry());
        ASSERT_EQ(buttons.size(), 3u);
        SCOPED_TRACE(margin);
        EXPECT_EQ(rows[0]->geometry(), (RectF{rowX, margin + 5, 68, 22}));
        EXPECT_EQ(buttons[0], (RectF{0, 0, 22, 22}));
        EXPECT_EQ(buttons[1].x, 23);
        EXPECT_EQ(buttons[2].x, 46);
        EXPECT_EQ(content->geometry().x, rowX + 68 + 1);
        EXPECT_EQ(content->geometry().width, 1100 - (rowX + 68 + 1));
    }
}

// As Qt's ShortcutOverride: a focused text field keeps plain and Shift keys
// and its editing sequences, and a window shortcut with Control, Alt or
// Super runs before it.
TEST(WindowTest, ModifiedShortcutsGoBeforeTextFields)
{
    Window window;
    auto column = std::make_unique<Column>();
    TextField* field = column->addChild<TextField>("text");
    window.resize({300, 100});
    window.setContentView(std::move(column));
    window.show();
    window.grab();
    TestInput::setKeyboardFocus(window, true);
    field->setFocus();
    int accepted = 0;
    field->onAccepted = [&accepted] { ++accepted; };
    std::vector<std::string> ran;
    window.addShortcut(ControlModifier, key::Return, [&ran] { ran.push_back("Ctrl+Return"); });
    window.addShortcut(ControlModifier, key::letter('a'), [&ran] { ran.push_back("Ctrl+A"); });
    window.addShortcut(NoModifier, key::Return, [&ran] { ran.push_back("Return"); });

    TestInput::typeKey(window, key::Return, ControlModifier);
    EXPECT_EQ(ran, std::vector<std::string>{"Ctrl+Return"});
    EXPECT_EQ(accepted, 0);
    TestInput::typeKey(window, key::letter('a'), ControlModifier);
    EXPECT_EQ(ran.size(), 1u);
    EXPECT_EQ(field->selectedText(), "text");
    TestInput::typeKey(window, key::Return);
    EXPECT_EQ(ran.size(), 1u);
    EXPECT_EQ(accepted, 1);
}

namespace {

// Takes presses, and counts the releases that land inside it.
class PressView : public View
{
public:
    bool acceptsPointer() const override { return true; }

    int presses = 0;
    int releasesInside = 0;
    int releasesOutside = 0;

protected:
    void pointerPress(const PointerEvent&) override { ++presses; }
    void pointerRelease(const PointerEvent& event) override
    {
        if (localRect().contains(event.position))
            ++releasesInside;
        else
            ++releasesOutside;
    }
};

// What the content of a header and the bar beside it hold.
class HeaderRow : public View
{
public:
    HeaderRow()
    {
        moving = addChild<PressView>();
        moving->setMovesWindow(true);
        still = addChild<PressView>();
    }

    PressView* moving;
    PressView* still;

protected:
    void layout() override
    {
        moving->setGeometry({0, 0, 100, 40});
        still->setGeometry({100, 0, 100, 40});
    }
};

} // namespace

static void dragBy(Window& window, PointF from, PointF delta)
{
    TestInput::movePointer(window, from);
    TestInput::press(window, from);
    TestInput::movePointer(window, {from.x + delta.x, from.y + delta.y});
    TestInput::release(window, {from.x + delta.x, from.y + delta.y});
}

TEST(WindowTest, HeaderDragFromViewsInItMovesTheWindow)
{
    Window window;
    auto column = std::make_unique<Column>();
    WindowHeader* header = column->addChild<WindowHeader>();
    header->setLayoutHints({.fillWidth = true});
    HeaderRow* row = header->setContent<HeaderRow>();
    PressView* outside = column->addChild<PressView>();
    outside->setMovesWindow(true);
    outside->setLayoutHints({.fillWidth = true, .fillHeight = true});
    window.resize({400, 300});
    window.setContentView(std::move(column));
    window.show();
    window.grab();
    const PointF maximizeButton = {9 + 2 * (22 + 1) + 11, 20};

    // The header itself moves at once.
    TestInput::movePointer(window, {350, 20});
    TestInput::press(window, {350, 20});
    TestInput::release(window, {350, 20});
    EXPECT_EQ(TestInput::moveRequests(window), 1);

    // A wobble within the drag distance is still a click.
    dragBy(window, maximizeButton, {5, 5});
    EXPECT_TRUE(window.states().maximized);
    EXPECT_EQ(TestInput::moveRequests(window), 1);

    // Further, the window moves, maximized or not, and nothing is clicked.
    dragBy(window, maximizeButton, {0, 20});
    EXPECT_EQ(TestInput::moveRequests(window), 2);
    EXPECT_TRUE(window.states().maximized);

    dragBy(window, {153, 20}, {30, 0});
    EXPECT_EQ(TestInput::moveRequests(window), 3);
    EXPECT_EQ(row->moving->presses, 1);
    EXPECT_EQ(row->moving->releasesInside, 0);
    EXPECT_EQ(row->moving->releasesOutside, 1);

    // A view that keeps its drags, and one outside the header, move nothing.
    dragBy(window, {243, 20}, {30, 0});
    EXPECT_EQ(row->still->releasesInside, 1);
    dragBy(window, {200, 150}, {30, 0});
    EXPECT_EQ(outside->releasesInside, 1);
    EXPECT_EQ(TestInput::moveRequests(window), 3);

    // Nor does anything while full screen.
    window.setFullScreen(true);
    dragBy(window, {133, 20}, {30, 0});
    EXPECT_EQ(TestInput::moveRequests(window), 3);
    EXPECT_EQ(row->moving->releasesInside, 1);
}

TEST(WindowTest, ConfirmDialogAnswersOnce)
{
    Window parent;
    parent.resize({400, 300});
    parent.setContentView(std::make_unique<View>());
    parent.show();

    ConfirmDialog dialog(&parent, "Remove the file?", "It cannot be brought back.");
    dialog.setDestructive(true);
    int accepted = 0;
    int rejected = 0;
    dialog.onAccepted = [&accepted] { ++accepted; };
    dialog.onRejected = [&rejected] { ++rejected; };
    dialog.show();
    Window* window = dialog.window();
    ASSERT_NE(window, nullptr);
    window->grab();
    EXPECT_GE(window->size().width, 336);
    // A destructive dialog starts on Cancel; Escape cancels too.
    TestInput::setKeyboardFocus(*window, true);
    TestInput::typeKey(*window, key::Escape);
    EXPECT_EQ(rejected, 1);
    EXPECT_EQ(accepted, 0);
}

// A confirmation: Return confirms even while a destructive dialog's Cancel has
// the focus; Space presses the focused button.
TEST(WindowTest, ConfirmDialogReturnConfirms)
{
    Window parent;
    parent.resize({400, 300});
    parent.setContentView(std::make_unique<View>());
    parent.show();

    for (const bool space : {false, true}) {
        ConfirmDialog dialog(&parent, "Remove the file?", "It cannot be brought back.");
        dialog.setDestructive(true);
        int accepted = 0;
        int rejected = 0;
        dialog.onAccepted = [&accepted] { ++accepted; };
        dialog.onRejected = [&rejected] { ++rejected; };
        dialog.show();
        Window* window = dialog.window();
        window->grab();
        TestInput::setKeyboardFocus(*window, true);
        TestInput::typeKey(*window, space ? key::Space : key::Return);
        EXPECT_EQ(accepted, space ? 0 : 1);
        EXPECT_EQ(rejected, space ? 1 : 0);
    }
}

TEST(WindowTest, ConfirmDialogTextChangesWhileShown)
{
    Window parent;
    parent.resize({400, 300});
    parent.setContentView(std::make_unique<View>());
    parent.show();

    ConfirmDialog dialog(&parent, "Pairing");
    dialog.show();
    Window* window = dialog.window();
    window->grab();
    const double height = window->size().height;
    dialog.setText("Confirm that the code 123456 shows on the other device.");
    window->grab();
    EXPECT_GT(window->size().height, height);
    dialog.setTitle("Pairing with the keyboard");
    const View* column = window->contentView()->children().front().get();
    const View* texts = column->children().front().get();
    EXPECT_EQ(static_cast<const Label*>(texts->children().front().get())->text(), "Pairing with the keyboard");
    dialog.setText({});
    window->grab();
    EXPECT_EQ(window->size().height, height);
}

TEST(WindowTest, DialogTakesTheHeightOfItsContent)
{
    Window parent;
    parent.resize({400, 300});
    parent.setContentView(std::make_unique<View>());
    parent.show();

    DialogWindow dialog(&parent, "Details", 320);
    auto column = std::make_unique<Column>();
    column->addChild<View>()->setLayoutHints({.preferredHeight = 100});
    dialog.setContentView(std::move(column));
    dialog.show();
    // A 44-pixel header, the content, 16 below it.
    EXPECT_EQ(dialog.window()->size().height, 44 + 100 + 16);
    EXPECT_EQ(dialog.window()->size().width, 320);
}

TEST(WindowTest, ThemeChangesReachLabels)
{
    Window window;
    auto label = std::make_unique<Label>("Theme");
    label->setFontSize(30);
    window.resize({100, 50});
    window.setContentView(std::move(label));
    window.show();

    const auto darkest = [](const Image& image) {
        int value = 255;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x)
                value = std::min(value, int(image.pixel(x, y).g));
        }
        return value;
    };
    const Theme original = Theme::current();
    EXPECT_LT(darkest(window.grab()), 100);
    Theme::setCurrent(Theme::dark());
    // White text on the dark background: nothing dark but the background.
    EXPECT_EQ(darkest(window.grab()), Theme::dark().background.g);
    Theme::setCurrent(original);
}

TEST(WindowTest, DialogKeepsItsMarginAtEveryScale)
{
    Window parent;
    parent.resize({760, 720});
    parent.setContentView(std::make_unique<View>());
    parent.setScale(1.5);
    parent.show();

    ConfirmDialog dialog(&parent, "Move the file to the trash?", "It can be put back from the trash later.");
    dialog.show();
    Window* window = dialog.window();
    EXPECT_EQ(window->scale(), 1.5);
    // Text lines are taller at 150%; the window grows instead of the margin shrinking.
    for (const double scale : {1.5, 1.0, 1.25}) {
        window->setScale(scale);
        window->grab();
        const View* actions = window->contentView()->children().front()->children().back().get();
        const double margin = window->size().height - actions->geometry().bottom();
        EXPECT_GE(margin, 12) << scale;
        EXPECT_LT(margin, 13) << scale;
    }
}

TEST(WindowTest, DialogFitsTextLinesAtFractionalScale)
{
    Window parent;
    parent.resize({400, 300});
    parent.setContentView(std::make_unique<View>());
    parent.setScale(1.5);
    parent.show();

    DialogWindow dialog(&parent, "Network details", 360);
    auto column = std::make_unique<Column>();
    column->setSpacing(8);
    for (const char* line : {"Address 192.168.1.20", "Gateway 192.168.1.1", "DNS 1.1.1.1"})
        column->addChild<Label>(line);
    View* content = column.get();
    dialog.setContentView(std::move(column));
    dialog.show();
    Window* window = dialog.window();
    window->grab();
    const double bottom = content->geometry().y + content->children().back()->geometry().bottom();
    EXPECT_GE(window->size().height - bottom, 16);
    EXPECT_LT(window->size().height - bottom, 17);
}

// A tool tip is a whole number of pixels wide; its text keeps one gap to both
// sides, whatever the rounding leaves over.
TEST(WindowTest, ToolTipCentresItsText)
{
    for (const double scale : {1.0, 1.5}) {
        Window window;
        window.resize({400, 200});
        window.setContentView(std::make_unique<View>());
        View* target = window.contentView()->addChild<View>();
        target->setGeometry({180, 120, 40, 40});
        window.setScale(scale);
        window.show();
        for (const char* text : {"Close", "Wi-Fi", "Capture Selected Window", "Record Selected Portion"}) {
            window.pinToolTip(target, target->localRect(), text);
            const View* tip = TestInput::toolTip(window);
            ASSERT_NE(tip, nullptr);
            const auto* label = static_cast<const Label*>(tip->children().front().get());
            const RectF box = label->geometry();
            SCOPED_TRACE(std::string(text) + " at " + std::to_string(scale));
            EXPECT_EQ(1u, label->lineCount());
            EXPECT_DOUBLE_EQ(label->sizeHint().width, box.width);
            EXPECT_NEAR(box.x * scale, (tip->geometry().width - box.right()) * scale, 0.5);
            window.unpinToolTip(target);
        }
    }
}

// A label in no window, given a window's text layout mode, measures as the
// same label drawn in that window.
TEST(WindowTest, LabelMeasuredOutsideTheWindowMatchesItsDrawnWidth)
{
    for (const bool hinted : {true, false}) {
        for (const double scale : {1.0, 1.5}) {
            Window window;
            window.resize({600, 200});
            window.setContentView(std::make_unique<View>());
            window.setScale(scale);
            window.setHintedTextLayout(hinted);
            window.show();
            for (const char* text : {"Close", "Wi-Fi", "Capture Selected Window", "Record Selected Portion"}) {
                Label* drawn = window.contentView()->addChild<Label>(text);
                drawn->setGeometry({0, 0, 600, 40});
                window.grab();
                Label measure(text);
                const SizeF unset = measure.sizeHint();
                measure.setTextLayoutMode(window.textLayoutMode());
                SCOPED_TRACE(std::string(text) + " at " + std::to_string(scale) + (hinted ? " hinted" : ""));
                EXPECT_EQ(drawn->sizeHint(), measure.sizeHint());
                // Out of any window, without a mode: scale 1, hinted.
                if (scale == 1.0 && hinted)
                    EXPECT_EQ(drawn->sizeHint(), unset);
                window.contentView()->takeChild(drawn);
            }
        }
    }
}

// A window stays active while its dialog has the focus.
TEST(WindowTest, ParentStaysActiveWhileItsDialogIsFocused)
{
    Window parent;
    parent.resize({200, 100});
    parent.show();
    Window dialog;
    dialog.setTransientParent(&parent, true);
    dialog.resize({100, 50});
    dialog.show();
    Window other;
    other.resize({100, 50});
    other.show();

    // Headless windows show activated.
    TestInput::configure(dialog, 100, 50, {});
    TestInput::configure(other, 100, 50, {});
    TestInput::configure(parent, 200, 100, {.activated = true});
    EXPECT_TRUE(parent.isActive());
    EXPECT_TRUE(dialog.isActive());
    EXPECT_FALSE(other.isActive());

    TestInput::configure(parent, 200, 100, {});
    TestInput::configure(dialog, 100, 50, {.activated = true});
    EXPECT_TRUE(parent.isActive());
    EXPECT_TRUE(dialog.isActive());

    TestInput::configure(dialog, 100, 50, {});
    TestInput::configure(other, 100, 50, {.activated = true});
    EXPECT_FALSE(parent.isActive());
    EXPECT_FALSE(dialog.isActive());
    EXPECT_TRUE(other.isActive());
}

// A toast: over the content at the bottom centre once faded in.
TEST(WindowTest, ToastShowsOverTheContent)
{
    Window window;
    window.resize({400, 300});
    window.setContentView(std::make_unique<View>());
    window.show();
    window.grab();
    const Image before = window.grab(1'000'000'000);

    Toast toast(&window);
    toast.show("Saved");
    window.grab(1'000'000'000 + 500'000'000);
    const Image shown = window.grab(1'000'000'000 + 600'000'000);
    int changed = 0;
    for (int y = 0; y < 300; ++y) {
        for (int x = 0; x < 400; ++x) {
            if (shown.pixel(x, y).argb() != before.pixel(x, y).argb()) {
                ++changed;
                // Only near the bottom, centred.
                EXPECT_GT(y, 200);
            }
        }
    }
    EXPECT_GT(changed, 0);
}

TEST(WindowTest, ConfirmDialogClosesFromItsCornerOnlyWithoutCancel)
{
    Window parent;
    parent.resize({400, 300});
    parent.setContentView(std::make_unique<View>());
    parent.show();
    for (const bool cancel : {true, false}) {
        ConfirmDialog dialog(&parent, "Close the window?");
        dialog.setCancelVisible(cancel);
        bool rejected = false;
        dialog.onRejected = [&] { rejected = true; };
        dialog.show();
        Window* window = dialog.window();
        window->grab();
        // Where the window's close button is: 8 from the top and the start, in the 32-pixel slot.
        TestInput::click(*window, {8 + 16, 8 + 16});
        EXPECT_EQ(rejected, !cancel);
    }
}

namespace {

class ResizingView : public View
{
public:
    Window* target = nullptr;
    SizeF size;
    bool resized = false;

protected:
    void layout() override
    {
        if (!target || resized)
            return;
        resized = true;
        target->resize(size);
    }
};

} // namespace

TEST(WindowTest, ResizeFromLayoutLaysTheContentOutAtTheNewSize)
{
    Window window;
    auto view = std::make_unique<ResizingView>();
    ResizingView* content = view.get();
    content->target = &window;
    content->size = {300, 200};
    window.resize({480, 144});
    window.setContentView(std::move(view));
    window.show();
    window.grab();

    EXPECT_EQ(window.size().width, 300);
    EXPECT_EQ(window.size().height, 200);
    EXPECT_EQ(content->geometry().width, 300);
    EXPECT_EQ(content->geometry().height, 200);
}
