// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "ui/keys.h"
#include "ui/test_input.h"
#include "ui/window.h"
#include "ui/window_tab_bar.h"

using namespace cutefish::ui;

namespace {

struct Hosted
{
    Window window;
    WindowTabBar* bar = nullptr;
    uint64_t time = 1000000000;

    explicit Hosted(double width = 600, int tabs = 3)
    {
        auto view = std::make_unique<WindowTabBar>();
        bar = view.get();
        window.resize({width, 30});
        window.setContentView(std::move(view));
        window.show();
        for (int i = 0; i < tabs; ++i)
            bar->insertTab(i, "Tab " + std::to_string(i));
        frames(1);
    }

    // Frames of 16 ms on the window's clock.
    void frames(int count)
    {
        for (int i = 0; i < count; ++i) {
            time += 16666667;
            window.grab(time);
        }
    }
};

} // namespace

TEST(WindowTabBarTest, TabsShareTheWidthBetweenTheLimits)
{
    Hosted wide(900, 3);
    // 300 each would be over the 220 maximum.
    EXPECT_DOUBLE_EQ(wide.bar->tabRect(1).x, 220);
    EXPECT_DOUBLE_EQ(wide.bar->tabRect(1).width, 220);
    EXPECT_DOUBLE_EQ(wide.bar->trackWidth(), 660);

    Hosted narrow(300, 3);
    // 100 each would be under the 140 minimum: the strip scrolls.
    EXPECT_DOUBLE_EQ(narrow.bar->tabRect(0).width, 140);
    EXPECT_DOUBLE_EQ(narrow.bar->trackWidth(), 420);

    // The file manager's: no maximum, a track inset.
    Hosted manager(900, 3);
    manager.bar->setTabWidthRange(140, 0);
    manager.bar->setTrackInsets({6, 4, 40, 4});
    manager.frames(1);
    EXPECT_DOUBLE_EQ(manager.bar->tabRect(0).width, (900 - 46) / 3.0);
    EXPECT_DOUBLE_EQ(manager.bar->tabRect(0).x, 6);
    EXPECT_DOUBLE_EQ(manager.bar->tabRect(0).y, 4);
}

TEST(WindowTabBarTest, ClicksSelectCloseAndOpenMenus)
{
    Hosted hosted(660, 3);
    int current = -1;
    int closed = -1;
    int menu = -1;
    hosted.bar->onCurrentChanged = [&](int index) { current = index; };
    hosted.bar->onCloseRequested = [&](int index) { closed = index; };
    hosted.bar->onContextMenuRequested = [&](int index, PointF) { menu = index; };
    TestInput::click(hosted.window, {330, 15});
    EXPECT_EQ(current, 1);
    EXPECT_EQ(hosted.bar->currentIndex(), 1);
    TestInput::click(hosted.window, {550, 15}, MouseButton::Middle);
    EXPECT_EQ(closed, 2);
    TestInput::click(hosted.window, {100, 15}, MouseButton::Right);
    EXPECT_EQ(menu, 0);
    // The current tab's close button: 8 px from its start, 16 wide.
    closed = -1;
    hosted.frames(1);
    TestInput::movePointer(hosted.window, {220 + 16, 15});
    TestInput::click(hosted.window, {220 + 16, 15});
    EXPECT_EQ(closed, 1);
}

TEST(WindowTabBarTest, DraggingReordersAndTheTabsGlide)
{
    Hosted hosted(660, 3);
    std::pair<int, int> moved = {-1, -1};
    hosted.bar->onMoveRequested = [&](int from, int to) { moved = {from, to}; };
    TestInput::press(hosted.window, {100, 15});
    TestInput::movePointer(hosted.window, {130, 15});
    TestInput::movePointer(hosted.window, {350, 15});
    hosted.frames(20);
    // The second tab made room on the left: it glided into slot 0.
    EXPECT_NEAR(hosted.bar->tabRect(1).x, 0, 0.5);
    TestInput::release(hosted.window, {350, 15});
    EXPECT_EQ(moved, std::make_pair(0, 1));
    hosted.bar->moveTab(0, 1);
    hosted.frames(20);
    EXPECT_EQ(hosted.bar->tabTitle(1), "Tab 0");
    EXPECT_NEAR(hosted.bar->tabRect(1).x, 220, 0.5);
}

TEST(WindowTabBarTest, DraggingOffTheBarDetaches)
{
    Hosted hosted(660, 3);
    int detached = -1;
    hosted.bar->onDetachRequested = [&](int index, PointF) { detached = index; };
    TestInput::press(hosted.window, {300, 15});
    TestInput::movePointer(hosted.window, {320, 15});
    TestInput::movePointer(hosted.window, {320, 70});
    EXPECT_EQ(detached, 1);
    hosted.frames(20);
    // The others closed up: two slots.
    EXPECT_DOUBLE_EQ(hosted.bar->trackWidth(), 440);
    hosted.bar->removeTab(1);
    hosted.frames(20);
    EXPECT_EQ(hosted.bar->count(), 2);
    EXPECT_NEAR(hosted.bar->tabRect(1).x, 220, 0.5);
}

TEST(WindowTabBarTest, ALoneTabCanStayPut)
{
    Hosted hosted(660, 1);
    hosted.bar->setLoneTabDraggable(false);
    int detached = -1;
    bool moved = false;
    hosted.bar->onDetachRequested = [&](int index, PointF) { detached = index; };
    hosted.bar->onMoveRequested = [&](int, int) { moved = true; };
    const double x = hosted.bar->tabRect(0).x;
    TestInput::press(hosted.window, {100, 15});
    TestInput::movePointer(hosted.window, {130, 15});
    TestInput::movePointer(hosted.window, {300, 15});
    hosted.frames(20);
    EXPECT_DOUBLE_EQ(hosted.bar->tabRect(0).x, x);
    TestInput::movePointer(hosted.window, {300, 70});
    EXPECT_EQ(detached, 0);
    TestInput::release(hosted.window, {300, 70});
    EXPECT_FALSE(moved);
}

TEST(WindowTabBarTest, IncomingTabsMakeRoom)
{
    Hosted hosted(900, 2);
    hosted.bar->beginIncoming("Other", 230);
    hosted.frames(20);
    EXPECT_EQ(hosted.bar->incomingIndex(), 1);
    EXPECT_NEAR(hosted.bar->tabRect(1).x, 440, 0.5);
    hosted.bar->endIncoming(true);
    hosted.bar->insertTab(1, "Other");
    hosted.frames(20);
    EXPECT_EQ(hosted.bar->tabTitle(1), "Other");
    EXPECT_NEAR(hosted.bar->tabRect(1).x, 220, 0.5);
    EXPECT_EQ(hosted.bar->indexAtPosition({300, 15}), 1);
}

TEST(WindowTabBarTest, RenamesInPlace)
{
    Hosted hosted(660, 2);
    std::string renamed;
    hosted.bar->onRenameRequested = [&](int, const std::string& title) { renamed = title; };
    TestInput::setKeyboardFocus(hosted.window, true);
    hosted.bar->startRename(1);
    hosted.frames(1);
    TestInput::typeText(hosted.window, "logs");
    TestInput::typeKey(hosted.window, key::Return);
    EXPECT_EQ(renamed, "logs");
}

// A tab closing before the one being renamed or dragged, as a terminal tab
// whose shell exits, leaves the rename and the drag with their own tab.
TEST(WindowTabBarTest, RenameAndDragFollowTheirTabWhenOthersGo)
{
    Hosted hosted(660, 3);
    int closed = -1;
    hosted.bar->onCloseRequested = [&](int index) { closed = index; };
    TestInput::setKeyboardFocus(hosted.window, true);
    hosted.bar->startRename(1);
    hosted.frames(1);
    hosted.bar->removeTab(0);
    hosted.frames(20);
    // The other tab, made current, shows its close button.
    TestInput::click(hosted.window, {330, 15});
    EXPECT_EQ(hosted.bar->currentIndex(), 1);
    hosted.frames(1);
    TestInput::movePointer(hosted.window, {220 + 16, 15});
    TestInput::click(hosted.window, {220 + 16, 15});
    EXPECT_EQ(closed, 1);

    Hosted dragged(660, 3);
    std::pair<int, int> moved = {-1, -1};
    dragged.bar->onMoveRequested = [&](int from, int to) { moved = {from, to}; };
    TestInput::press(dragged.window, {550, 15});
    TestInput::movePointer(dragged.window, {530, 15});
    TestInput::movePointer(dragged.window, {520, 15});
    dragged.bar->removeTab(0);
    TestInput::release(dragged.window, {520, 15});
    EXPECT_EQ(moved.first, 1);
    EXPECT_EQ(dragged.bar->currentIndex(), 1);
}

TEST(WindowTabBarTest, CrowdedStripScrolls)
{
    Hosted hosted(300, 5);
    hosted.bar->setCurrentIndex(4);
    hosted.frames(1);
    // The last tab in view at the right end.
    EXPECT_NEAR(hosted.bar->tabRect(4).x + 140, 300, 0.5);
    ScrollEvent event;
    event.deltaY = -1000;
    event.precise = true;
    TestInput::scroll(hosted.window, {150, 15}, event);
    hosted.frames(1);
    EXPECT_NEAR(hosted.bar->tabRect(0).x, 0, 0.5);
}
