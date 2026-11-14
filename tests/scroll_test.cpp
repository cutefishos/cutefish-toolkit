// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_views.h"
#include "ui/header_backdrop.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/list_view.h"
#include "ui/painter.h"
#include "ui/scroll_bar.h"
#include "ui/scroll_view.h"
#include "ui/scroller.h"
#include "ui/test_input.h"
#include "ui/window.h"

using namespace cutefish::ui;
using test::Box;

// Mirrors cutefish-framework's scrolling tests, so both toolkits keep one feel.
// Deltas point towards the end of the content, the other way from the
// framework's.

static double now = 0;

static double testClock()
{
    return now;
}

static void touchpad(Window& window, PointF delta, uint32_t time, PointF position = {50, 50})
{
    now = time;
    ScrollEvent event;
    event.deltaX = delta.x;
    event.deltaY = delta.y;
    event.precise = true;
    event.time = time;
    TestInput::scroll(window, position, event);
}

static void lift(Window& window, uint32_t time, PointF position = {50, 50})
{
    now = time;
    ScrollEvent event;
    event.precise = true;
    event.stop = true;
    event.time = time;
    TestInput::scroll(window, position, event);
}

// Shows the frame that samples the fingers at time.
static void frame(Window& window, double time)
{
    now = time + Scroller::resampleLatency;
    window.grab(uint64_t(now * 1e6));
}

static void swipe(Window& window, double step, uint32_t start = 1000, PointF position = {50, 50})
{
    for (uint32_t i = 1; i <= 3; ++i)
        touchpad(window, {0, step}, start + 16 * i, position);
    lift(window, start + 50, position);
}

class ScrollTest : public testing::Test
{
protected:
    void SetUp() override { Scroller::setClock(testClock); }
    void TearDown() override { Scroller::setClock(nullptr); }
};

namespace {

struct Scene
{
    Scene()
    {
        auto content = std::make_unique<Row>();
        left = view(content.get());
        right = view(content.get());
        window.resize({600, 300});
        window.setContentView(std::move(content));
        window.show();
        window.grab(1);
        left->setScrollPosition({0, 500});
        right->setScrollPosition({0, 500});
    }

    static ScrollView* view(Row* row)
    {
        auto* scrollView = row->addChild<ScrollView>();
        scrollView->setLayoutHints({.fillHeight = true, .preferredWidth = 300});
        scrollView->setScrollDirections(true, true);
        scrollView->setContent<Box>(SizeF{2000, 2000});
        return scrollView;
    }

    Window window;
    ScrollView* left;
    ScrollView* right;
};

} // namespace

TEST_F(ScrollTest, TouchpadScrollsAndCoasts)
{
    Scene scene;
    touchpad(scene.window, {0, 10}, 1016);
    frame(scene.window, 1016);
    EXPECT_EQ(scene.left->scrollPosition().y, 510);
    lift(scene.window, 1020);

    swipe(scene.window, 16, 2000);
    frame(scene.window, 2048);
    const double released = scene.left->scrollPosition().y;
    frame(scene.window, 2200);
    EXPECT_GT(scene.left->scrollPosition().y, released);
}

TEST_F(ScrollTest, WheelStepsAtOnce)
{
    Scene scene;
    now = 1000;
    ScrollEvent event;
    event.value120Y = 120;
    event.deltaY = 10;
    TestInput::scroll(scene.window, {50, 50}, event);
    EXPECT_EQ(scene.left->scrollPosition().y, 545);
}

TEST_F(ScrollTest, StopAtBoundsDoesNotOvershoot)
{
    Scene scene;
    scene.left->setScrollPosition({0, 0});
    scene.right->setScrollPosition({0, 0});
    scene.left->setElastic(false);
    swipe(scene.window, -10);
    frame(scene.window, 1040);
    EXPECT_EQ(scene.left->scrollPosition().y, 0);

    swipe(scene.window, -10, 2000, {350, 50});
    frame(scene.window, 2040);
    EXPECT_LT(scene.right->scrollPosition().y, 0);
    for (int time = 2056; time <= 3000; time += 16)
        frame(scene.window, time);
    EXPECT_EQ(scene.right->scrollPosition().y, 0);
}

TEST_F(ScrollTest, GestureStaysWithItsFirstView)
{
    Scene scene;
    touchpad(scene.window, {0, 10}, 1016);
    touchpad(scene.window, {0, 10}, 1032, {350, 50});
    frame(scene.window, 1032);
    EXPECT_EQ(scene.left->scrollPosition().y, 520);
    EXPECT_EQ(scene.right->scrollPosition().y, 500);
}

TEST_F(ScrollTest, PerpendicularGestureReachesTheParent)
{
    Window window;
    auto outer = std::make_unique<ScrollView>();
    auto* column = outer->setContent<Column>();
    auto* inner = column->addChild<ScrollView>();
    inner->setLayoutHints({.fillWidth = true, .preferredHeight = 300});
    inner->setScrollDirections(true, false);
    inner->setContent<Box>(SizeF{2000, 300});
    column->addChild<Box>(SizeF{300, 1700});
    ScrollView* outerView = outer.get();
    window.resize({300, 300});
    window.setContentView(std::move(outer));
    window.show();
    window.grab(1);

    touchpad(window, {0, 10}, 1016);
    frame(window, 1016);
    EXPECT_EQ(inner->scrollPosition().x, 0);
    EXPECT_EQ(outerView->scrollPosition().y, 10);
}

TEST_F(ScrollTest, RestingFingersStopACoast)
{
    Scene scene;
    swipe(scene.window, 16, 2000);
    frame(scene.window, 2100);
    const double stopped = scene.left->scrollPosition().y;
    TestInput::hold(scene.window);
    frame(scene.window, 2200);
    EXPECT_EQ(scene.left->scrollPosition().y, stopped);
}

TEST_F(ScrollTest, PressStopsACoast)
{
    Scene scene;
    swipe(scene.window, 16, 2000);
    frame(scene.window, 2100);
    const double stopped = scene.left->scrollPosition().y;
    TestInput::click(scene.window, {50, 50});
    frame(scene.window, 2200);
    EXPECT_EQ(scene.left->scrollPosition().y, stopped);
}

// A drop-down's content goes to a new popup window at every opening: the
// coast and the bar's fade run on the frames of the window it is in now.
TEST_F(ScrollTest, CoastsAndFadesInTheWindowItMovedTo)
{
    auto first = std::make_unique<Window>();
    auto column = std::make_unique<Column>();
    auto* view = column->addChild<ScrollView>();
    view->setLayoutHints({.fillWidth = true, .fillHeight = true});
    view->setScrollDirections(false, true);
    view->setContent<Box>(SizeF{300, 2000});
    first->resize({300, 300});
    first->setContentView(std::move(column));
    first->show();
    first->grab(1);
    view->setScrollPosition({0, 500});
    swipe(*first, 16, 1000);
    frame(*first, 1048);
    std::unique_ptr<View> scroll = first->contentView()->takeChild(view);
    first.reset();

    Window second;
    second.resize({300, 300});
    second.setContentView(std::move(scroll));
    second.show();
    const Image before = second.grab(uint64_t(4000 * 1e6));
    swipe(second, 16, 5000);
    frame(second, 5048);
    const double released = view->scrollPosition().y;
    frame(second, 5200);
    EXPECT_GT(view->scrollPosition().y, released);

    // Faded in by now, the handle shows at the right edge.
    const Image after = second.grab(uint64_t((5600 + Scroller::resampleLatency) * 1e6));
    bool shown = false;
    for (int y = 0; y < after.height() && !shown; ++y) {
        for (int x = after.width() - int(ScrollBar::thickness); x < after.width(); ++x) {
            if (after.pixel(x, y) != before.pixel(x, y)) {
                shown = true;
                break;
            }
        }
    }
    EXPECT_TRUE(shown);
}

TEST_F(ScrollTest, ListViewMakesOnlyRowsInSight)
{
    Window window;
    auto list = std::make_unique<ListView>();
    int created = 0;
    std::set<size_t> bound;
    list->createItem = [&created] {
        ++created;
        return std::make_unique<Label>();
    };
    list->bindItem = [&bound](View* item, size_t row) {
        static_cast<Label*>(item)->setText("Row " + std::to_string(row));
        bound.insert(row);
    };
    list->setRowHeight(20);
    list->setRowCount(1000);
    ListView* listView = list.get();
    window.resize({200, 100});
    window.setContentView(std::move(list));
    window.show();
    window.grab(1);

    EXPECT_EQ(created, 5);
    EXPECT_EQ(listView->contentSize().height, 20000);
    EXPECT_NE(listView->itemForRow(4), nullptr);
    EXPECT_EQ(listView->itemForRow(5), nullptr);

    listView->setScrollPosition({0, 510});
    window.grab(2);
    // Rows 25 to 30 show, part of the first and last; the views are reused.
    EXPECT_EQ(created, 6);
    EXPECT_NE(listView->itemForRow(25), nullptr);
    EXPECT_NE(listView->itemForRow(30), nullptr);
    EXPECT_EQ(listView->itemForRow(24), nullptr);
    EXPECT_EQ(static_cast<Label*>(listView->itemForRow(30))->text(), "Row 30");
    EXPECT_EQ(listView->itemForRow(30)->mapToWindow({}).y, 30 * 20 - 510);

    listView->scrollToRow(999);
    window.grab(3);
    EXPECT_EQ(listView->scrollPosition().y, 20000 - 100);
}

// Taller rows keep the row at the top where it was, as far into it.
TEST_F(ScrollTest, ListViewKeepsTheTopRowWhenRowsChangeHeight)
{
    Window window;
    auto list = std::make_unique<ListView>();
    list->createItem = [] { return std::make_unique<Label>(); };
    list->bindItem = [](View*, size_t) {};
    list->setRowHeight(20);
    list->setSpacing(2);
    list->setMargins({0, 6, 0, 0});
    list->setRowCount(1000);
    ListView* listView = list.get();
    window.resize({200, 100});
    window.setContentView(std::move(list));
    window.show();
    window.grab(1);
    // Row 30, 5 into it.
    listView->setScrollPosition({0, 6 + 30 * 22 + 5});
    listView->setRowHeight(40);
    EXPECT_DOUBLE_EQ(listView->scrollPosition().y, 6 + (30 + 5.0 / 22) * 42);
    window.grab(2);
    EXPECT_NE(listView->itemForRow(30), nullptr);
    EXPECT_EQ(listView->itemForRow(29), nullptr);
}

namespace {

// A row that tracks hover and counts everything it hears of the pointer.
class HoverRow : public View
{
public:
    HoverRow() { setTracksHover(true); }

    bool acceptsPointer() const override { return true; }

    bool entered = false;
    int events = 0;

protected:
    void hoverChanged() override { ++events; }
    void hoverMove(const PointerEvent&) override { ++events; }
    void pointerEnter() override
    {
        entered = true;
        ++events;
    }
    void pointerLeave() override
    {
        entered = false;
        ++events;
    }
};

struct RowScene
{
    RowScene()
    {
        auto scroll = std::make_unique<ScrollView>();
        view = scroll.get();
        Box* content = view->setContent<Box>(SizeF{300, 4000});
        for (int row = 0; row < 200; ++row) {
            HoverRow* item = content->addChild<HoverRow>();
            item->setGeometry({0, row * 20.0, 300, 20});
            rows.push_back(item);
        }
        window.resize({300, 300});
        window.setContentView(std::move(scroll));
        window.show();
        window.grab(1);
        view->setScrollPosition({0, 500});
        window.grab(2);
    }

    HoverRow* rowUnder(PointF point) const
    {
        return rows[size_t(std::floor((point.y + view->scrollPosition().y) / 20))];
    }

    int events() const
    {
        int count = 0;
        for (const HoverRow* row : rows)
            count += row->events;
        return count;
    }

    void forgetEvents()
    {
        for (HoverRow* row : rows)
            row->events = 0;
    }

    Window window;
    ScrollView* view;
    std::vector<HoverRow*> rows;
};

} // namespace

// The row under a resting pointer stays hovered while a coast carries it
// away, and the pointer hovers the row it is over once the coast slows.
TEST_F(ScrollTest, FastCoastKeepsTheHoveredRow)
{
    RowScene scene;
    TestInput::movePointer(scene.window, {50, 50});
    HoverRow* hovered = scene.rowUnder({50, 50});
    EXPECT_TRUE(hovered->isHovered());
    EXPECT_TRUE(hovered->entered);

    swipe(scene.window, 16, 2000);
    scene.forgetEvents();
    const double released = scene.view->scrollPosition().y;
    for (int time = 2066; time <= 2300; time += 16) {
        frame(scene.window, time);
        EXPECT_TRUE(hovered->isHovered());
        EXPECT_TRUE(hovered->entered);
        EXPECT_EQ(scene.events(), 0);
    }
    EXPECT_GT(scene.view->scrollPosition().y, released + 60);

    // Slower than the threshold, with some way still to go.
    frame(scene.window, 2600);
    const double slow = scene.view->scrollPosition().y;
    HoverRow* under = scene.rowUnder({50, 50});
    EXPECT_NE(under, hovered);
    EXPECT_FALSE(hovered->isHovered());
    EXPECT_FALSE(hovered->entered);
    EXPECT_TRUE(under->isHovered());
    EXPECT_TRUE(under->entered);

    for (int time = 2616; time <= 4000; time += 16)
        frame(scene.window, time);
    EXPECT_GT(scene.view->scrollPosition().y, slow);
    under = scene.rowUnder({50, 50});
    EXPECT_TRUE(under->isHovered());
    EXPECT_TRUE(under->entered);
}

TEST_F(ScrollTest, PointerMotionDuringAFastCoastHoversAtOnce)
{
    RowScene scene;
    TestInput::movePointer(scene.window, {50, 50});
    swipe(scene.window, 16, 2000);
    frame(scene.window, 2066);
    frame(scene.window, 2082);

    TestInput::movePointer(scene.window, {50, 150});
    HoverRow* under = scene.rowUnder({50, 150});
    EXPECT_TRUE(under->isHovered());
    EXPECT_TRUE(under->entered);
    EXPECT_FALSE(scene.rowUnder({50, 50})->isHovered());
}

TEST_F(ScrollTest, WheelStepHoversInTheNextFrame)
{
    RowScene scene;
    TestInput::movePointer(scene.window, {50, 50});
    HoverRow* hovered = scene.rowUnder({50, 50});
    now = 1000;
    ScrollEvent event;
    event.value120Y = 120;
    event.deltaY = 10;
    TestInput::scroll(scene.window, {50, 50}, event);
    frame(scene.window, 1000);
    HoverRow* under = scene.rowUnder({50, 50});
    EXPECT_NE(under, hovered);
    EXPECT_FALSE(hovered->isHovered());
    EXPECT_TRUE(under->isHovered());
}

// Fingers that stop on the touchpad tick no more frames; the next frame
// hovers what the gesture brought under the pointer.
TEST_F(ScrollTest, PausedGestureHoversWithoutInput)
{
    RowScene scene;
    TestInput::movePointer(scene.window, {50, 50});
    for (uint32_t time = 1016; time <= 1048; time += 16) {
        touchpad(scene.window, {0, 10}, time);
        frame(scene.window, time);
    }
    HoverRow* hovered = nullptr;
    for (HoverRow* row : scene.rows) {
        if (row->isHovered())
            hovered = row;
    }
    ASSERT_NE(hovered, nullptr);
    EXPECT_NE(hovered, scene.rowUnder({50, 50}));

    for (int time = 1064; time <= 1160; time += 16)
        frame(scene.window, time);
    HoverRow* under = scene.rowUnder({50, 50});
    EXPECT_FALSE(hovered->isHovered());
    EXPECT_TRUE(under->isHovered());
    EXPECT_TRUE(under->entered);
}

// A recycled row view loses its hover before it shows another row.
TEST_F(ScrollTest, RecycledListRowIsNotHovered)
{
    Window window;
    auto list = std::make_unique<ListView>();
    std::map<View*, size_t> bound;
    list->createItem = [] { return std::make_unique<HoverRow>(); };
    list->bindItem = [&bound](View* item, size_t row) { bound[item] = row; };
    list->setRowHeight(20);
    list->setRowCount(1000);
    ListView* listView = list.get();
    window.resize({200, 100});
    window.setContentView(std::move(list));
    window.show();
    window.grab(1);

    TestInput::movePointer(window, {50, 10});
    auto* first = static_cast<HoverRow*>(listView->itemForRow(0));
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(first->isHovered());
    EXPECT_TRUE(first->entered);

    swipe(window, 30, 2000, {50, 10});
    int reused = 0;
    for (int time = 2066; time <= 2200; time += 16) {
        frame(window, time);
        if (first->isVisible() && bound[first] != 0) {
            ++reused;
            EXPECT_FALSE(first->isHovered());
            EXPECT_FALSE(first->entered);
        }
    }
    EXPECT_GT(listView->scrollPosition().y, 200);
    EXPECT_GT(reused, 0);
}

// A view that counts presses.
class PressBox : public Box
{
public:
    using Box::Box;
    bool acceptsPointer() const override { return true; }
    int presses = 0;

protected:
    void pointerPress(const PointerEvent&) override { ++presses; }
};

// The bar's strip takes presses unless the bars lie below the content; its
// padding moves the track, so a press above the track pages up no more.
TEST(ScrollViewTest, BarsBelowTheContentAndPadded)
{
    Window window;
    auto scroll = std::make_unique<ScrollView>();
    ScrollView* view = scroll.get();
    PressBox* content = view->setContent<PressBox>(SizeF{200, 1000});
    window.resize({200, 100});
    window.setContentView(std::move(scroll));
    window.show();
    window.grab();

    TestInput::click(window, {196, 50});
    EXPECT_EQ(content->presses, 0);
    view->setBarsBelowContent(true);
    TestInput::click(window, {196, 50});
    EXPECT_EQ(content->presses, 1);
    view->setBarsBelowContent(false);
    // The first click paged down.
    view->setScrollPosition({0, 0});

    // Dragging the handle from its top: the track starts 40 down.
    view->setVerticalBarPadding(40, 4);
    window.grab();
    TestInput::press(window, {196, 41});
    TestInput::movePointer(window, {196, 41 + (100 - 40 - 4 - 32) / 2.0});
    TestInput::release(window, {196, 41 + (100 - 40 - 4 - 32) / 2.0});
    EXPECT_NEAR(view->scrollPosition().y, 450, 1);
}

TEST(ScrollViewTest, CornerPaddingKeepsTheHandleConcentric)
{
    EXPECT_DOUBLE_EQ(ScrollBar::cornerPadding(0), ScrollBar::inset);
    EXPECT_DOUBLE_EQ(ScrollBar::cornerPadding(12), 9);
    EXPECT_DOUBLE_EQ(ScrollBar::cornerPadding(14), 11);
}

// Invisible at rest, it fades in over 16 pixels of scrolling: a blur of
// the content under the tint, here black stripes greyed under it.
TEST(ScrollViewTest, HeaderBackdropFadesInOverScrolledContent)
{
    class Stripes : public Box
    {
    public:
        using Box::Box;

    protected:
        void paint(Painter& painter) override
        {
            for (double y = 0; y < 1000; y += 8)
                painter.fillRect({0, y, 200, 4}, Color::rgb(0));
        }
    };

    Window window;
    auto root = std::make_unique<View>();
    auto scroll = std::make_unique<ScrollView>();
    ScrollView* view = scroll.get();
    view->setContent<Stripes>(SizeF{200, 1000});
    root->addChild(std::move(scroll));
    auto* backdrop = root->addChild<HeaderBackdrop>(view);
    window.resize({200, 100});
    window.setContentView(std::move(root));
    window.show();
    window.grab();
    view->setGeometry({0, 0, 200, 100});
    backdrop->setGeometry({0, 0, 200, 40});
    window.grab();

    const auto row = [&window](int y) {
        const Image image = window.grab();
        int darkest = 255;
        int lightest = 0;
        for (int x = 0; x < 200; ++x) {
            darkest = std::min(darkest, int(image.pixel(x, y).g));
            lightest = std::max(lightest, int(image.pixel(x, y).g));
        }
        return std::pair{darkest, lightest};
    };
    // At rest the stripes show through.
    EXPECT_EQ(row(1).first, 0);
    view->setScrollPosition({0, 16});
    // Blurred to an even grey, nothing as dark as a stripe.
    const auto [darkest, lightest] = row(10);
    EXPECT_GT(darkest, 60);
    EXPECT_LT(lightest - darkest, 8);
}
