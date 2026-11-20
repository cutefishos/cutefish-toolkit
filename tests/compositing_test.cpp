// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cmath>
#include <cxxabi.h>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <typeinfo>
#include <vector>

#include "test_views.h"
#include "ui/button.h"
#include "ui/check_box.h"
#include "ui/combo_box.h"
#include "ui/dialog_window.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/painter.h"
#include "ui/progress_bar.h"
#include "ui/scroll_view.h"
#include "ui/segmented_control.h"
#include "ui/slider.h"
#include "ui/switch.h"
#include "ui/test_input.h"
#include "ui/text_field.h"
#include "ui/text_layout.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_header.h"

using namespace cutefish::ui;

static constexpr uint64_t kFrameNs = 16666667;
// What a translucent pixel can gain or lose in 8-bit channels by being
// blended into a transparent texture before the window.
static constexpr int kTextureRounding = 3;

static std::string typeName(const View& view)
{
    int status = 0;
    char* name = abi::__cxa_demangle(typeid(view).name(), nullptr, nullptr, &status);
    std::string result = status == 0 ? name : typeid(view).name();
    free(name);
    return result;
}

static void forEachView(View* view, const std::function<void(View*)>& function)
{
    function(view);
    for (const auto& child : view->children())
        forEachView(child.get(), function);
}

static PointF centreOf(const View* view)
{
    return view->mapToWindow({view->geometry().width / 2, view->geometry().height / 2});
}

namespace {

// The gallery's controls, in a header and a scrolling page.
struct Controls
{
    ScrollView* scroll = nullptr;
    Column* page = nullptr;
    Row* buttons = nullptr;
    Switch* toggle = nullptr;
    CheckBox* checkBox = nullptr;
    RadioButton* radio = nullptr;
    Slider* slider = nullptr;
    SegmentedControl* segments = nullptr;
    TabBar* tabs = nullptr;
    TextField* field = nullptr;
    Button* button = nullptr;
};

} // namespace

static std::unique_ptr<View> buildControls(Controls& controls)
{
    static ButtonGroup group;
    auto root = std::make_unique<Column>();
    WindowHeader* header = root->addChild<WindowHeader>();
    header->setLayoutHints({.fillWidth = true});
    header->setContent<Label>("Controls");
    controls.scroll = root->addChild<ScrollView>();
    controls.scroll->setLayoutHints({.fillWidth = true, .fillHeight = true});
    Column* page = controls.scroll->setContent<Column>();
    controls.page = page;
    page->setMargins(Margins::all(24));
    page->setSpacing(14);

    controls.buttons = page->addChild<Row>();
    controls.buttons->setSpacing(10);
    controls.button = controls.buttons->addChild<Button>("Button");
    controls.buttons->addChild<Button>("Disabled")->setEnabled(false);
    controls.buttons->addChild<DialogButton>("Cancel");
    controls.buttons->addChild<DialogButton>("Confirm", true);
    Row* toggles = page->addChild<Row>();
    toggles->setSpacing(10);
    controls.toggle = toggles->addChild<Switch>("Switch");
    controls.checkBox = toggles->addChild<CheckBox>("Check box");
    controls.radio = toggles->addChild<RadioButton>("Radio");
    controls.radio->setGroup(&group);
    toggles->addChild<RadioButton>("Other")->setGroup(&group);
    controls.slider = page->addChild<Slider>();
    controls.slider->setValue(0.4);
    controls.slider->setLayoutHints({.fillWidth = true});
    page->addChild<ProgressBar>()->setValue(0.65);
    page->addChild<BusyIndicator>();
    Row* choices = page->addChild<Row>();
    choices->setSpacing(10);
    controls.segments = choices->addChild<SegmentedControl>(std::vector<std::string>{"Day", "Week", "Month"});
    choices->addChild<ComboBox>(std::vector<std::string>{"Automatic", "Light", "Dark"});
    controls.tabs = page->addChild<TabBar>(std::vector<std::string>{"General", "Display", "Sound"});
    controls.tabs->setLayoutHints({.fillWidth = true});
    controls.field = page->addChild<TextField>();
    controls.field->setPlaceholder("Search");
    controls.field->setLayoutHints({.fillWidth = true});
    Row* icons = page->addChild<Row>();
    for (const char* name : {"check", "close", "search", "folder"})
        icons->addChild<Icon>(name, 16);
    for (int i = 0; i < 12; ++i)
        page->addChild<Label>("Row " + std::to_string(i) + " of text that scrolls");
    return root;
}

namespace {

// Draws a few shapes that land on fractional positions, and counts paints.
class Shapes : public View
{
public:
    int paints = 0;
    Color color = Color::rgb(0x2E7DF7);

    SizeF sizeHint() const override { return {60, 40}; }

protected:
    void paint(Painter& painter) override
    {
        ++paints;
        painter.fillRoundedRect(localRect(), 8.5, color);
        painter.strokeRoundedRect(localRect().adjusted(4.3, 4.3, -4.3, -4.3), 4, Color::rgb(0xFFFFFF), 1);
        painter.drawShadow({10, 10, 20, 12}, 3, 2, Color::rgb(0).withAlpha(0.4));
    }
};

// Squares on a fixed grid cut by its bounds, so a change of size changes
// only what it uncovers, noting what each paint was asked for.
class FixedGrid : public View
{
public:
    int paints = 0;
    std::vector<RectF> painted;

protected:
    void paint(Painter& painter) override
    {
        ++paints;
        painted = painter.clipRects();
        painter.clip(localRect());
        painter.fillRect(localRect(), Color::rgb(0xF0F0F0));
        for (double y = 0; y < geometry().height; y += 20) {
            for (double x = 0; x < geometry().width; x += 20)
                painter.fillRect({x + 4, y + 4, 12, 12}, Color::rgb(0x2E7DF7));
        }
    }
};

} // namespace

static int mismatches(const Image& a, const Image& b, std::string* first = nullptr)
{
    if (a.width() != b.width() || a.height() != b.height())
        return -1;
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) == b.pixel(x, y))
                continue;
            if (!count && first)
                *first = std::to_string(x) + "," + std::to_string(y);
            ++count;
        }
    }
    return count;
}

// The largest channel difference between two images of one size.
static int maxDifference(const Image& a, const Image& b)
{
    int largest = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const Color p = a.pixel(x, y);
            const Color q = b.pixel(x, y);
            largest = std::max({largest, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b),
                                std::abs(p.a - q.a)});
        }
    }
    return largest;
}

TEST(CompositingTest, VisualRectsHoldEverythingPainted)
{
    for (const double scale : {1.0, 1.5}) {
        Window window;
        Controls controls;
        window.resize({760, 900});
        window.setContentView(buildControls(controls));
        window.setScale(scale);
        window.show();
        TestInput::setKeyboardFocus(window, true);
        window.grab();

        const auto check = [&](View* view, const char* state) {
            const RectF visual = view->visualRect();
            const Rect ink = TestInput::inkRect(*view, scale);
            if (ink.width == 0)
                return;
            const int left = int(std::floor(visual.x * scale)) - 1;
            const int top = int(std::floor(visual.y * scale)) - 1;
            const int right = int(std::ceil(visual.right() * scale)) + 1;
            const int bottom = int(std::ceil(visual.bottom() * scale)) + 1;
            EXPECT_TRUE(ink.x >= left && ink.y >= top && ink.x + ink.width <= right
                        && ink.y + ink.height <= bottom)
                << typeName(*view) << " " << state << " at " << scale << ": ink " << ink.x << ","
                << ink.y << " " << ink.width << "x" << ink.height << " outside " << left << "," << top
                << " " << right - left << "x" << bottom - top;
        };

        forEachView(window.contentView(), [&](View* view) { check(view, "at rest"); });
        forEachView(window.contentView(), [&](View* view) {
            if (!view->acceptsFocus() || !view->isEnabled())
                return;
            view->setFocus(FocusReason::Keyboard);
            window.grab();
            check(view, "focused");
        });
        forEachView(window.contentView(), [&](View* view) {
            if (!view->acceptsPointer() || !view->isEnabled() || view == window.contentView())
                return;
            TestInput::movePointer(window, centreOf(view));
            window.grab();
            check(view, "hovered");
            TestInput::press(window, centreOf(view));
            window.grab();
            check(view, "pressed");
            TestInput::release(window, {1, 899});
        });
    }
}

TEST(CompositingTest, IncrementalFramesMatchFullRepaints)
{
    const Theme original = Theme::current();
    for (const double scale : {1.0, 1.5}) {
        Window window;
        Controls controls;
        window.resize({560, 640});
        window.setContentView(buildControls(controls));
        window.setScale(scale);
        window.show();

        uint64_t now = 1000000000;
        int frames = 0;
        bool failed = false;
        std::string step = "start";
        const auto frame = [&](int count = 1) {
            for (int i = 0; i < count && !failed; ++i) {
                now += kFrameNs;
                const Image incremental = TestInput::presentFrame(window, now);
                const Image full = window.grab(now);
                std::string first;
                const int wrong = mismatches(incremental, full, &first);
                ++frames;
                if (wrong) {
                    failed = true;
                    incremental.savePng("/tmp/cutefish-toolkit-incremental.png");
                    full.savePng("/tmp/cutefish-toolkit-full.png");
                }
                EXPECT_EQ(wrong, 0) << step << ", frame " << frames << " at " << scale << ", first at " << first;
            }
        };

        frame(4);
        step = "hover";
        forEachView(window.contentView(), [&](View* view) {
            if (view->acceptsPointer() && view != window.contentView()) {
                step = "hover " + typeName(*view);
                TestInput::movePointer(window, centreOf(view));
                frame(2);
            }
        });
        step = "toggles";
        TestInput::click(window, centreOf(controls.toggle));
        frame(20);
        TestInput::click(window, centreOf(controls.checkBox));
        TestInput::click(window, centreOf(controls.radio));
        frame(10);
        step = "segments";
        TestInput::click(window, centreOf(controls.segments->children()[1].get()));
        TestInput::click(window, centreOf(controls.tabs->children()[2].get()));
        frame(20);
        step = "text";
        TestInput::click(window, centreOf(controls.field));
        TestInput::typeText(window, "hello");
        frame(3);
        TestInput::typeKey(window, key::Left, ShiftModifier);
        frame(2);
        step = "slider";
        const PointF start = centreOf(controls.slider);
        TestInput::press(window, start);
        TestInput::movePointer(window, {start.x + 60, start.y});
        frame(2);
        TestInput::release(window, {start.x + 60, start.y});
        frame(2);
        step = "scroll";
        ScrollEvent wheel;
        wheel.deltaY = 48;
        wheel.value120Y = 120;
        TestInput::scroll(window, centreOf(controls.scroll), wheel);
        frame(30);
        step = "transform";
        controls.buttons->setTransform({{30, 5}, 1});
        frame(2);
        controls.buttons->setTransform({{30.4, 7.7}, 0.9});
        frame(2);
        controls.buttons->setOpacity(0.5);
        frame(2);
        controls.buttons->setTransform({});
        controls.buttons->setOpacity(1);
        frame(2);
        if (failed)
            return;
        step = "layered";
        controls.page->setLayered(true);
        frame(2);
        TestInput::movePointer(window, centreOf(controls.button));
        frame(2);
        controls.page->setLayered(false);
        frame(2);
        step = "visibility";
        controls.buttons->setVisible(false);
        frame(2);
        controls.buttons->setVisible(true);
        controls.button->setEnabled(false);
        frame(2);
        step = "theme";
        Theme::setCurrent(Theme::dark());
        frame(3);
        Theme::setCurrent(original);
        frame(3);
        EXPECT_GT(frames, 100);
    }
}

TEST(CompositingTest, LayerAtRestPaintsTheSamePixels)
{
    for (const double scale : {1.0, 1.25, 1.5, 1.75}) {
        Window window;
        auto root = std::make_unique<View>();
        View* group = root->addChild<View>();
        group->setGeometry({13.3, 7.7, 200, 120});
        group->addChild<Shapes>()->setGeometry({5.5, 3.25, 60, 40});
        group->addChild<Shapes>()->setGeometry({70.2, 50.6, 61.3, 40.1});
        window.resize({240, 160});
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();

        const Image direct = window.grab();
        group->setLayered(true);
        const Image layered = window.grab();
        // Translucent pixels round once more through a transparent texture;
        // a pixel out of place would differ by far more.
        EXPECT_LE(maxDifference(direct, layered), kTextureRounding) << scale;
    }
}

// Fills its rect, counting its paints.
class Fill : public View
{
public:
    explicit Fill(Color color)
        : m_color(color)
    {
    }

    void paint(Painter& painter) override
    {
        ++paints;
        painter.fillRect(localRect(), m_color);
    }

    int paints = 0;

private:
    Color m_color;
};

static ViewGlass halfWhiteDisc()
{
    ViewGlass glass;
    glass.key = 1;
    glass.rect = {0, 0, 40, 40};
    glass.radius = 20;
    glass.tint[3] = 0.5;
    glass.sideEdge[3] = 1;
    return glass;
}

TEST(CompositingTest, GlassShadesWhatLiesUnderItsViewByDistanceToItsOutline)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {0, 0, 0, 255});
    View* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 40, 40});
    pane->setGlass({halfWhiteDisc()});
    window.resize({60, 60});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = window.grab();
    // White at half over black in the middle and up to the outline, whose
    // line lies outside it; the corners of the rect and outside untouched.
    EXPECT_NEAR(image.pixel(60, 60).r, 128, 2);
    EXPECT_NEAR(image.pixel(20, 60).r, 128, 2);
    EXPECT_NEAR(image.pixel(60, 20).r, 128, 2);
    EXPECT_EQ(image.pixel(22, 22).r, 0);
    EXPECT_EQ(image.pixel(5, 5).r, 0);
}

TEST(CompositingTest, GlassShowsWhatLiesFurtherInWithinItsBezelAndAddsItsLight)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
    Fill* black = root->addChild<Fill>(Color {0, 0, 0, 255});
    black->setGeometry({0, 0, 14, 40});
    Fill* grey = root->addChild<Fill>(Color {128, 128, 128, 255});
    grey->setGeometry({60, 0, 40, 40});
    View* bent = root->addChild<View>();
    bent->setGeometry({10, 10, 40, 20});
    ViewGlass bending;
    bending.key = 1;
    bending.rect = {0, 0, 40, 20};
    bending.bezel = 8;
    bending.bend = 8;
    bending.profile = 1;
    bent->setGlass({bending});
    View* lit = root->addChild<View>();
    lit->setGeometry({70, 10, 20, 20});
    ViewGlass lighting;
    lighting.key = 1;
    lighting.rect = {0, 0, 20, 20};
    lighting.light = {{0, 0.2}};
    lit->setGlass({lighting});
    window.resize({100, 40});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = window.grab();
    // Over the black strip just inside the pane's left edge it shows the
    // white further in; the middle shows what lies under it.
    EXPECT_GE(image.pixel(21, 40).r, 250);
    EXPECT_GE(image.pixel(25, 40).r, 250);
    EXPECT_GE(image.pixel(60, 40).r, 250);
    EXPECT_LE(image.pixel(17, 40).r, 2);
    // The light adds to the grey under it rather than veiling it.
    EXPECT_NEAR(image.pixel(160, 40).r, 179, 2);
}

TEST(CompositingTest, GlassBendsByItsSideBezelAtTheSidesAndRaisesItsColour)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
    Fill* black = root->addChild<Fill>(Color {0, 0, 0, 255});
    black->setGeometry({0, 0, 14, 40});
    Fill* grey = root->addChild<Fill>(Color {128, 128, 128, 255});
    grey->setGeometry({60, 0, 40, 40});
    View* bent = root->addChild<View>();
    bent->setGeometry({10, 10, 40, 20});
    ViewGlass bending;
    bending.key = 1;
    bending.rect = {0, 0, 40, 20};
    bending.bezel = 8;
    bending.sideBezel = 0;
    bending.bend = 8;
    bending.profile = 1;
    bent->setGlass({bending});
    View* raised = root->addChild<View>();
    raised->setGeometry({70, 10, 20, 20});
    ViewGlass raising;
    raising.key = 1;
    raising.rect = {0, 0, 20, 20};
    raising.exponent = 0.5;
    raised->setGlass({raising});
    window.resize({100, 40});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = window.grab();
    // No bezel at the side: the black under the pane's left edge shows as it is.
    EXPECT_LE(image.pixel(21, 40).r, 2);
    // Grey 128 raised to the power 0.5.
    EXPECT_NEAR(image.pixel(160, 40).r, 181, 2);
}

TEST(CompositingTest, GlassBendsMostInsideItsOutlineDispersesAtItsSidesAndLiftsItsShadowOffIt)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
    Fill* black = root->addChild<Fill>(Color {0, 0, 0, 255});
    black->setGeometry({0, 0, 14, 40});
    Fill* band = root->addChild<Fill>(Color {0, 0, 0, 255});
    band->setGeometry({60, 0, 100, 14});
    View* bent = root->addChild<View>();
    bent->setGeometry({10, 10, 40, 20});
    ViewGlass bending;
    bending.key = 1;
    bending.rect = {0, 0, 40, 20};
    bending.bezel = 8;
    bending.bend = 8;
    bending.bendPeak = 4;
    bending.profile = 1;
    bent->setGlass({bending});
    // Two panes over the band's lower edge, dispersing all round and along
    // the sides only.
    std::vector<View*> dispersing;
    for (double sides : {0.0, 1.0}) {
        View* pane = root->addChild<View>();
        pane->setGeometry({sides > 0 ? 110.0 : 62.0, 8, 40, 20});
        ViewGlass glass;
        glass.key = 1;
        glass.rect = {0, 0, 40, 20};
        glass.bezel = 8;
        glass.bend = 8;
        glass.profile = 1;
        glass.dispersion = 0.5;
        glass.dispersionSides = sides;
        pane->setGlass({glass});
    }
    View* shaded = root->addChild<View>();
    shaded->setGeometry({20, 50, 20, 10});
    ViewGlass shading;
    shading.key = 1;
    shading.rect = {0, 0, 20, 10};
    shading.shadow[3] = 1;
    shading.shadowBlur = 8;
    shading.shadowGap = 4;
    shaded->setGlass({shading});
    window.resize({160, 80});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = window.grab();
    // Right at the outline nothing is bent, so the black strip under it
    // shows; 4.5 points in the bend is nearly whole and shows the white.
    EXPECT_LE(image.pixel(21, 40).r, 30);
    EXPECT_GE(image.pixel(29, 40).r, 250);
    // A point inside the top edge over the band: red bent less than blue
    // where the dispersion goes all round, neither along the top where it
    // keeps to the sides.
    const Color all = image.pixel(2 * 82, 19);
    const Color sides = image.pixel(2 * 130, 19);
    EXPECT_GE(std::abs(int(all.b) - int(all.r)), 100);
    EXPECT_LE(std::abs(int(sides.b) - int(sides.r)), 4);
    // The shadow starts lighter at the outline than 4 points below it.
    EXPECT_GE(image.pixel(60, 121).r, image.pixel(60, 128).r + 30);
}

TEST(CompositingTest, GlassReadsAlongItsAxesAndSplitsItsSideLine)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
    Fill* band = root->addChild<Fill>(Color {0, 0, 0, 255});
    band->setGeometry({0, 0, 100, 13});
    View* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 60, 20});
    ViewGlass glass;
    glass.key = 1;
    glass.rect = {0, 0, 60, 20};
    glass.radius = 10;
    glass.bezel = 8;
    glass.bend = -6;
    glass.profile = 1;
    glass.bendAlongAxes = 1;
    glass.sideEdge[3] = 1;
    glass.edgeDevicePixels = 2;
    glass.edgeSplit = 2;
    pane->setGlass({glass});
    window.resize({100, 40});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = window.grab();
    // Reading straight up, a point 4 points in from the top shows the band
    // above as well in the cap as in the middle.
    EXPECT_LE(image.pixel(2 * 40, 2 * 14).r, 40);
    EXPECT_LE(image.pixel(2 * 14, 2 * 14).r, 40);
    // Just inside either end the side line's red reaches further in than
    // its blue; along the top it does not.
    const Color left = image.pixel(2 * 10 + 1, 2 * 20);
    const Color right = image.pixel(2 * 70 - 2, 2 * 20);
    const Color top = image.pixel(2 * 40, 2 * 10 + 1);
    EXPECT_LE(left.r + 60, left.b);
    EXPECT_LE(right.r + 60, right.b);
    EXPECT_NEAR(top.r, top.b, 2);
}

TEST(CompositingTest, GlassKeepsItsLineOnWholePixelsTakesItsBottomLineAndMultiplies)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
    Fill* grey = root->addChild<Fill>(Color {128, 128, 128, 255});
    grey->setGeometry({66, 0, 34, 40});
    View* snapped = root->addChild<View>();
    snapped->setGeometry({10.25, 10, 20, 20});
    ViewGlass snapping;
    snapping.key = 1;
    snapping.rect = {0, 0, 20, 20};
    snapping.sideEdge[3] = 1;
    snapping.edgeOnPixels = true;
    snapped->setGlass({snapping});
    View* lined = root->addChild<View>();
    lined->setGeometry({40, 10, 20, 20});
    ViewGlass lining;
    lining.key = 1;
    lining.rect = {0, 0, 20, 20};
    lining.edge[3] = 1;
    lining.bottomEdge[0] = 1;
    lining.bottomEdge[3] = 1;
    lined->setGlass({lining});
    View* multiplied = root->addChild<View>();
    multiplied->setGeometry({70, 10, 20, 20});
    ViewGlass multiplying;
    multiplying.key = 1;
    multiplying.rect = {0, 0, 20, 20};
    multiplying.multiply[1] = 0.5;
    multiplying.multiply[2] = 0;
    multiplying.multiply[3] = 1;
    multiplied->setGlass({multiplying});
    window.resize({100, 40});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = window.grab();
    // The left outline falls half way into pixel 20: the line covers that
    // pixel and leaves the one outside it alone.
    EXPECT_GE(image.pixel(19, 40).r, 253);
    EXPECT_LE(image.pixel(20, 40).r, 2);
    EXPECT_GE(image.pixel(21, 40).r, 253);
    // Black along the top, red along the bottom.
    EXPECT_LE(image.pixel(100, 19).r, 2);
    EXPECT_GE(image.pixel(100, 60).r, 253);
    EXPECT_LE(image.pixel(100, 60).g, 2);
    // Grey 128 multiplied by (1, 0.5, 0).
    EXPECT_NEAR(image.pixel(160, 40).r, 128, 2);
    EXPECT_NEAR(image.pixel(160, 40).g, 64, 2);
    EXPECT_LE(image.pixel(160, 40).b, 2);
}

TEST(CompositingTest, GlassBlurredAtHalfTheResolutionShowsAFineLineWiderAndShallower)
{
    for (const bool halved : {false, true}) {
        Window window;
        auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
        Fill* line = root->addChild<Fill>(Color {0, 0, 0, 255});
        line->setGeometry({40, 0, 1, 40});
        View* pane = root->addChild<View>();
        pane->setGeometry({20, 10, 40, 20});
        ViewGlass glass;
        glass.key = 1;
        glass.rect = {0, 0, 40, 20};
        glass.blurAtHalfResolution = halved;
        pane->setGlass({glass});
        window.resize({80, 40});
        window.setContentView(std::move(root));
        window.setScale(2);
        window.show();

        const Image image = window.grab();
        int darkest = 255;
        int greyed = 0;
        for (int x = 74; x < 88; ++x) {
            darkest = std::min<int>(darkest, image.pixel(x, 40).r);
            greyed += image.pixel(x, 40).r < 230;
        }
        if (!halved) {
            // Unblurred: two black pixels.
            EXPECT_LE(darkest, 2);
            EXPECT_EQ(greyed, 2);
        } else {
            EXPECT_GE(darkest, 40);
            EXPECT_LE(darkest, 140);
            EXPECT_GE(greyed, 3);
        }
    }
}

TEST(CompositingTest, GlassCurveGoesOnStraightPastBlackAndWhiteAndBlursLeaningToTheLight)
{
    for (const bool straight : {false, true}) {
        Window window;
        auto root = std::make_unique<Fill>(Color {128, 128, 128, 255});
        Fill* red = root->addChild<Fill>(Color {255, 0, 0, 255});
        red->setGeometry({40, 0, 40, 40});
        View* pane = root->addChild<View>();
        pane->setGeometry({0, 0, 80, 40});
        ViewGlass glass;
        glass.key = 1;
        glass.rect = {0, 0, 80, 40};
        glass.tint[0] = glass.tint[1] = glass.tint[2] = 0.029;
        glass.tint[3] = 0.5;
        glass.saturation = 1.9;
        glass.exponent = 0.49;
        if (straight) {
            glass.exponentSlopes[0] = 0.7;
            glass.exponentSlopes[1] = 1.53;
        }
        pane->setGlass({glass});
        window.resize({80, 40});
        window.setContentView(std::move(root));
        window.setScale(2);
        window.show();

        const Image image = window.grab();
        // Grey keeps the curve either way.
        EXPECT_NEAR(image.pixel(40, 40).g, 133, 3);
        // Red's other channels go below black after the saturation: flat,
        // they are black; straight, they lie below the black's 32.
        if (!straight) {
            EXPECT_LE(image.pixel(120, 40).g, 2);
        } else {
            EXPECT_GE(image.pixel(120, 40).r, 253);
            EXPECT_NEAR(image.pixel(120, 40).g, 15, 4);
            EXPECT_NEAR(image.pixel(120, 40).b, 15, 4);
        }
    }

    int darkest[2] = {};
    for (const bool leaning : {false, true}) {
        Window window;
        auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
        Fill* line = root->addChild<Fill>(Color {0, 0, 0, 255});
        line->setGeometry({40, 0, 2, 40});
        View* pane = root->addChild<View>();
        pane->setGeometry({20, 10, 40, 20});
        ViewGlass glass;
        glass.key = 1;
        glass.rect = {0, 0, 40, 20};
        glass.blur = 4;
        glass.blurPower = leaning ? 1.4 : 1;
        pane->setGlass({glass});
        window.resize({80, 40});
        window.setContentView(std::move(root));
        window.setScale(2);
        window.show();

        const Image image = window.grab();
        darkest[leaning] = 255;
        for (int x = 70; x < 94; ++x)
            darkest[leaning] = std::min<int>(darkest[leaning], image.pixel(x, 40).r);
    }
    EXPECT_LT(darkest[0], 230);
    EXPECT_GE(darkest[1], darkest[0] + 10);
}

TEST(CompositingTest, GlassMovesToNewValuesOnFramesWithoutRepaintingItsView)
{
    Window window;
    auto root = std::make_unique<View>();
    // In a texture, so only a paint of its own paints it.
    Fill* under = root->addChild<Fill>(Color {0, 0, 0, 255});
    under->setGeometry({0, 0, 60, 60});
    under->setLayered(true);
    under->setOpaqueLayer(true);
    auto* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 40, 40});
    pane->setGlass({halfWhiteDisc()});
    window.resize({60, 60});
    window.setContentView(std::move(root));
    window.setScale(1);
    window.show();

    uint64_t now = 1000000000;
    EXPECT_NEAR(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 128, 2);
    const int paints = under->paints;
    ViewGlass brighter = halfWhiteDisc();
    brighter.tint[3] = 1;
    brighter.duration = 0.1;
    brighter.curve[0] = 0;
    brighter.curve[1] = 0;
    brighter.curve[2] = 1;
    brighter.curve[3] = 1;
    pane->setGlass({brighter});
    // The first frame shows the move one refresh along (a sixth of 0.1 s),
    // two thirds of the way 50 ms later, then all of it.
    EXPECT_NEAR(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 149, 3);
    EXPECT_NEAR(TestInput::presentFrame(window, now += 50000000).pixel(30, 30).r, 212, 3);
    EXPECT_NEAR(TestInput::presentFrame(window, now += 50000000).pixel(30, 30).r, 255, 1);
    EXPECT_EQ(under->paints, paints);

    // Left out, it fades out over its own duration.
    pane->setGlass({});
    EXPECT_NEAR(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 212, 3);
    EXPECT_NEAR(TestInput::presentFrame(window, now += 50000000).pixel(30, 30).r, 85, 3);
    EXPECT_EQ(TestInput::presentFrame(window, now += 50000000).pixel(30, 30).r, 0);
    EXPECT_EQ(under->paints, paints);
}

TEST(CompositingTest, GlassFadingAColourOutPassesNoDarkerShade)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {255, 255, 255, 255});
    auto* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 40, 40});
    ViewGlass pressed = halfWhiteDisc();
    pressed.tint[3] = 0;
    pressed.sideEdge[3] = 0;
    pressed.over[0] = pressed.over[1] = pressed.over[2] = 1;
    pressed.over[3] = 0.85;
    pane->setGlass({pressed});
    window.resize({60, 60});
    window.setContentView(std::move(root));
    window.setScale(1);
    window.show();

    uint64_t now = 1000000000;
    TestInput::presentFrame(window, now += kFrameNs);
    // White laid over white, fading to no colour at all: white throughout.
    ViewGlass released = pressed;
    std::fill(released.over, released.over + 4, 0.0);
    released.duration = 0.1;
    pane->setGlass({released});
    for (int i = 0; i < 8; ++i)
        EXPECT_GE(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 254) << i;
}

TEST(CompositingTest, GlassLightsWhereThePointerIs)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {0, 0, 0, 255});
    auto* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 80, 40});
    ViewGlass glass;
    glass.rect = {0, 0, 80, 40};
    glass.radius = 20;
    glass.glow[3] = 1;
    glass.glowRadius = 15;
    pane->setGlass({glass});
    window.resize({100, 60});
    window.setContentView(std::move(root));
    window.setScale(1);
    window.show();

    uint64_t now = 1000000000;
    EXPECT_EQ(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 0);
    TestInput::movePointer(window, {30, 30});
    Image image = TestInput::presentFrame(window, now += kFrameNs);
    EXPECT_GE(image.pixel(30, 30).r, 240);
    EXPECT_EQ(image.pixel(70, 30).r, 0);
    TestInput::movePointer(window, {70, 30});
    image = TestInput::presentFrame(window, now += kFrameNs);
    EXPECT_EQ(image.pixel(30, 30).r, 0);
    EXPECT_GE(image.pixel(70, 30).r, 240);
}

TEST(CompositingTest, GlassLightLagsBehindThePointer)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {0, 0, 0, 255});
    auto* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 80, 40});
    ViewGlass glass;
    glass.rect = {0, 0, 80, 40};
    glass.radius = 20;
    glass.glow[3] = 1;
    glass.glowRadius = 15;
    glass.glowLag = 0.1;
    pane->setGlass({glass});
    window.resize({100, 60});
    window.setContentView(std::move(root));
    window.setScale(1);
    window.show();

    uint64_t now = 1000000000;
    TestInput::movePointer(window, {30, 30});
    EXPECT_GE(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 240);
    TestInput::movePointer(window, {70, 30});
    // A frame later the light has gone less than half the way.
    Image image = TestInput::presentFrame(window, now += kFrameNs);
    EXPECT_LT(image.pixel(70, 30).r, 40);
    for (int i = 0; i < 12; ++i)
        image = TestInput::presentFrame(window, now += kFrameNs);
    EXPECT_GE(image.pixel(70, 30).r, 240);
    EXPECT_EQ(image.pixel(30, 30).r, 0);
}

// A texture whose paint sets the glass of the pane it names.
class GlassPainter : public View
{
public:
    void paint(Painter&) override
    {
        ViewGlass glass = halfWhiteDisc();
        glass.tint[3] = alpha;
        pane->setGlass({glass});
    }

    View* pane = nullptr;
    double alpha = 0.5;
};

TEST(CompositingTest, GlassSetWhileTexturesPaintShowsInTheSameFrame)
{
    Window window;
    auto root = std::make_unique<Fill>(Color {0, 0, 0, 255});
    View* pane = root->addChild<View>();
    pane->setGeometry({10, 10, 40, 40});
    auto* painter = root->addChild<GlassPainter>();
    painter->setGeometry({0, 0, 5, 5});
    painter->setLayered(true);
    painter->pane = pane;
    window.resize({60, 60});
    window.setContentView(std::move(root));
    window.setScale(1);
    window.show();

    uint64_t now = 1000000000;
    EXPECT_NEAR(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 128, 2);
    painter->alpha = 1;
    painter->requestPaint();
    EXPECT_NEAR(TestInput::presentFrame(window, now += kFrameNs).pixel(30, 30).r, 255, 1);
}

TEST(CompositingTest, ComposedAtAnotherScaleGlassEdgesStayOneBufferPixel)
{
    Window window;
    auto root = std::make_unique<View>();
    Fill* white = root->addChild<Fill>(Color {255, 255, 255, 255});
    white->setGeometry({0, 0, 40, 20});
    white->setLayered(true);
    white->setOpaqueLayer(true);
    Fill* square = root->addChild<Fill>(Color {0, 0, 0, 255});
    square->setGeometry({28, 4, 8, 8});
    square->setLayered(true);
    View* pane = root->addChild<View>();
    pane->setGeometry({4, 4, 20, 12});
    ViewGlass glass;
    glass.rect = {0, 0, 20, 12};
    glass.sideEdge[3] = 1;
    pane->setGlass({glass});
    window.resize({40, 20});
    window.setContentView(std::move(root));
    window.setScale(2);
    window.show();

    const Image image = TestInput::composeFrame(window, 1.75);
    ASSERT_EQ(image.width(), 70);
    // The pane's side line covers the one buffer pixel outside its edge.
    EXPECT_EQ(image.pixel(5, 17).r, 255);
    EXPECT_LE(image.pixel(6, 17).r, 2);
    EXPECT_GE(image.pixel(7, 17).r, 253);
    // The textures are scaled into the buffer: the square spans 49 to 63.
    EXPECT_LE(image.pixel(50, 14).r, 2);
    EXPECT_LE(image.pixel(62, 14).r, 2);
    EXPECT_GE(image.pixel(47, 14).r, 253);
    EXPECT_GE(image.pixel(64, 14).r, 253);
}

TEST(CompositingTest, TransformsMoveTexturesWithoutRepainting)
{
    for (const double scale : {1.0, 1.5}) {
        Window window;
        auto root = std::make_unique<View>();
        Shapes* moved = root->addChild<Shapes>();
        moved->setGeometry({20, 20, 60, 40});
        window.resize({200, 100});
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();

        uint64_t now = 1000000000;
        TestInput::presentFrame(window, now += kFrameNs);
        moved->setTransform({{10, 0}, 1});
        TestInput::presentFrame(window, now += kFrameNs);
        const int paints = moved->paints;
        moved->setTransform({{40, 10}, 1});
        const Image translated = TestInput::presentFrame(window, now += kFrameNs);
        moved->setTransform({{40, 10}, 0.8});
        TestInput::presentFrame(window, now += kFrameNs);
        moved->setOpacity(0.5);
        TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_EQ(moved->paints, paints);

        // Whole device pixels: the texture lands where painting there would.
        moved->setOpacity(1);
        moved->setTransform({});
        moved->setGeometry({60, 30, 60, 40});
        EXPECT_LE(maxDifference(translated, window.grab()), kTextureRounding) << scale;
    }
}

// The horizontal centre of what is drawn in `image`, by coverage.
static double inkCentreX(const Image& image)
{
    double sum = 0;
    double weight = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double ink = 255 - image.pixel(x, y).r;
            sum += ink * (x + 0.5);
            weight += ink;
        }
    }
    return weight > 0 ? sum / weight : 0;
}

TEST(CompositingTest, AScaledTextureMovesByFractionsOfAPixel)
{
    for (const double scale : {1.0, 1.75}) {
        Window window;
        window.setBackgroundColor(Color::rgb(0xFFFFFF));
        auto root = std::make_unique<View>();
        Shapes* moved = root->addChild<Shapes>();
        moved->color = Color::rgb(0);
        moved->setGeometry({20, 20, 60, 40});
        window.resize({200, 100});
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();

        // A quarter of a device pixel a step: the ink moves every step, not in
        // whole pixels now and then, and a pixel over the four.
        uint64_t now = 1000000000;
        double first = 0;
        double previous = 0;
        for (int step = 0; step <= 4; ++step) {
            moved->setTransform({{step * 0.25 / scale, 0}, 0.8});
            const double centre = inkCentreX(TestInput::presentFrame(window, now += kFrameNs));
            if (step == 0)
                first = centre;
            else
                EXPECT_NEAR(0.25, centre - previous, 0.12) << scale << " " << step;
            previous = centre;
        }
        EXPECT_NEAR(1, previous - first, 0.05) << scale;

        // Unscaled, it still lands on whole device pixels.
        moved->setTransform({{0.4 / scale, 0}, 1});
        const Image nearly = TestInput::presentFrame(window, now += kFrameNs);
        moved->setTransform({});
        EXPECT_EQ(0, maxDifference(nearly, TestInput::presentFrame(window, now += kFrameNs))) << scale;
    }
}

TEST(CompositingTest, MovedLayersKeepTheirTextures)
{
    for (const double scale : {1.0, 1.5}) {
        Window window;
        auto root = std::make_unique<View>();
        View* group = root->addChild<View>();
        group->setGeometry({0, 0, 200, 100});
        Shapes* moved = group->addChild<Shapes>();
        moved->setGeometry({20, 20, 60, 40});
        moved->setLayered(true);
        window.resize({200, 100});
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();

        uint64_t now = 1000000000;
        TestInput::presentFrame(window, now += kFrameNs);
        const int paints = moved->paints;
        // Two device pixels at either scale, by the view and by its parent.
        moved->setGeometry({20 + 2 / scale, 20, 60, 40});
        TestInput::presentFrame(window, now += kFrameNs);
        group->setGeometry({0, 4 / scale, 200, 100});
        const Image kept = TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_EQ(moved->paints, paints) << scale;
        // The texture lands where painting there would.
        moved->setLayered(false);
        EXPECT_LE(maxDifference(kept, window.grab()), kTextureRounding) << scale;

        // Part of a device pixel paints the texture again.
        moved->setLayered(true);
        TestInput::presentFrame(window, now += kFrameNs);
        const int layeredPaints = moved->paints;
        moved->setGeometry({20.25, 20, 60, 40});
        TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_GT(moved->paints, layeredPaints) << scale;
    }
}

static bool paintsAt(const std::vector<RectF>& rects, PointF point)
{
    for (const RectF& rect : rects) {
        if (rect.contains(point))
            return true;
    }
    return false;
}

TEST(CompositingTest, ResizedLayersKeepTheirTextures)
{
    for (const double scale : {1.0, 1.5}) {
        Window window;
        auto root = std::make_unique<View>();
        FixedGrid* grid = root->addChild<FixedGrid>();
        grid->setGeometry({10, 10, 100, 60});
        grid->setLayered(true);
        grid->setOpaqueLayer(true);
        grid->setKeepsLayerOnResize(true);
        window.resize({400, 300});
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();

        uint64_t now = 1000000000;
        TestInput::presentFrame(window, now += kFrameNs);
        // Growing paints only what it uncovers, inside the texture and past
        // it, and so does shrinking, which uncovers nothing.
        for (const SizeF size : {SizeF {120, 70}, SizeF {300, 240}, SizeF {80, 50}}) {
            const int paints = grid->paints;
            const RectF before = grid->geometry();
            grid->setGeometry({10, 10, size.width, size.height});
            const Image kept = TestInput::presentFrame(window, now += kFrameNs);
            if (size.width > before.width) {
                EXPECT_GT(grid->paints, paints) << scale;
                EXPECT_FALSE(paintsAt(grid->painted, {before.width / 2, before.height / 2})) << scale;
                EXPECT_TRUE(paintsAt(grid->painted, {size.width - 1, size.height - 1})) << scale;
            } else {
                EXPECT_EQ(grid->paints, paints) << scale;
            }
            grid->setLayered(false);
            EXPECT_LE(maxDifference(kept, window.grab()), kTextureRounding) << scale << " " << size.width;
            grid->setLayered(true);
            TestInput::presentFrame(window, now += kFrameNs);
        }

        // Without it, a change of size paints it whole.
        grid->setKeepsLayerOnResize(false);
        grid->setGeometry({10, 10, 90, 60});
        TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_TRUE(paintsAt(grid->painted, {45, 30})) << scale;
    }
}

TEST(CompositingTest, ClippedAwayLayersPaintOnceInView)
{
    for (const double scale : {1.0, 1.5}) {
        Window window;
        auto root = std::make_unique<View>();
        View* clip = root->addChild<View>();
        clip->setGeometry({10, 10, 150, 80});
        clip->setClipsChildren(true);
        Shapes* shapes = clip->addChild<Shapes>();
        shapes->setGeometry({20, 120, 60, 40});
        shapes->setTransform({{0, 0}, 0.9});
        window.resize({200, 200});
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();

        uint64_t now = 1000000000;
        for (int i = 0; i < 3; ++i)
            TestInput::presentFrame(window, now += kFrameNs);
        shapes->setGeometry({20, 130.5, 60, 40});
        TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_EQ(shapes->paints, 0) << scale;

        // Brought in by its transform, then moved out and back in.
        shapes->setTransform({{3, -100}, 0.9});
        Image incremental = TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_GT(shapes->paints, 0) << scale;
        EXPECT_EQ(mismatches(incremental, window.grab(now)), 0) << scale;
        shapes->setTransform({{3, 0}, 0.9});
        incremental = TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_EQ(mismatches(incremental, window.grab(now)), 0) << scale;
        shapes->setGeometry({25, 20.5, 60, 40});
        shapes->setTransform({{3, 0}, 0.95});
        incremental = TestInput::presentFrame(window, now += kFrameNs);
        EXPECT_EQ(mismatches(incremental, window.grab(now)), 0) << scale;
    }
}

TEST(CompositingTest, DamageStaysWithTheChangedView)
{
    Window window;
    auto root = std::make_unique<View>();
    Shapes* left = root->addChild<Shapes>();
    left->setGeometry({10, 10, 60, 40});
    Shapes* right = root->addChild<Shapes>();
    right->setGeometry({200, 10, 60, 40});
    window.resize({300, 100});
    window.setContentView(std::move(root));
    window.show();

    uint64_t now = 1000000000;
    for (int i = 0; i < 4; ++i)
        TestInput::presentFrame(window, now += kFrameNs);
    const int rightPaints = right->paints;
    const int leftPaints = left->paints;
    left->color = Color::rgb(0xE5484D);
    left->requestPaint();
    TestInput::presentFrame(window, now += kFrameNs);
    EXPECT_EQ(left->paints, leftPaints + 1);
    EXPECT_EQ(right->paints, rightPaints);
}

namespace {

// Squares of 20 logical pixels in rows, one colour each.
class Board : public View
{
public:
    std::vector<Color> colors = std::vector<Color>(10 * 5, Color::rgb(0x2E7DF7));
    RectF lastClip;

protected:
    void paint(Painter& painter) override
    {
        lastClip = painter.clipBounds();
        for (int i = 0; i < int(colors.size()); ++i)
            painter.fillRect({double(i % 10) * 20, double(i / 10) * 20, 20, 20}, colors[size_t(i)]);
    }
};

} // namespace

TEST(CompositingTest, PartialPaintRepaintsOnlyTheRect)
{
    Window window;
    auto root = std::make_unique<View>();
    Board* board = root->addChild<Board>();
    board->setGeometry({10, 10, 200, 100});
    window.resize({220, 120});
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    window.setContentView(std::move(root));
    window.show();

    uint64_t now = 1000000000;
    for (int i = 0; i < 4; ++i)
        TestInput::presentFrame(window, now += kFrameNs);
    // Both squares change, but only the second is asked to paint again.
    board->colors[0] = Color::rgb(0x30A46C);
    board->colors[12] = Color::rgb(0xE5484D);
    board->requestPaint({40, 20, 20, 20});
    const Image incremental = TestInput::presentFrame(window, now += kFrameNs);
    const double scale = window.scale();
    const auto at = [scale](double x, double y) {
        return std::pair<int, int>(int(std::lround(x * scale)), int(std::lround(y * scale)));
    };
    const auto [inX, inY] = at(10 + 50, 10 + 30);
    const auto [outX, outY] = at(10 + 10, 10 + 10);
    EXPECT_EQ(incremental.pixel(inX, inY).r, 0xE5);
    EXPECT_EQ(incremental.pixel(outX, outY).r, 0x2E);
    // The clip, in the view's coordinates, takes in the rect: a view that
    // paints only inside its clip paints all that changed.
    EXPECT_LE(board->lastClip.x, 40);
    EXPECT_LE(board->lastClip.y, 20);
    EXPECT_GE(board->lastClip.x + board->lastClip.width, 60);
    EXPECT_GE(board->lastClip.y + board->lastClip.height, 40);
    const Image full = window.grab(now);
    EXPECT_EQ(full.pixel(outX, outY).r, 0x30);
    EXPECT_EQ(full.pixel(inX, inY).r, 0xE5);
    EXPECT_GE(board->lastClip.x + board->lastClip.width, 200);
    EXPECT_GE(board->lastClip.y + board->lastClip.height, 100);
}

namespace {

// Rows of squares and text under a vertical offset, as a scrolled document
// paints; it paints every pixel, so its layer can be opaque.
class Document : public View
{
public:
    double offset = 0;
    int highlighted = -1;
    Color squares = Color::rgb(0x2E7DF7);
    int paints = 0;

protected:
    void paint(Painter& painter) override
    {
        ++paints;
        painter.clip(localRect());
        painter.fillRect(localRect(), Color::rgb(0xFFFFFF));
        const double scale = painter.scale();
        for (int row = int(offset / 18); row * 18 - offset < geometry().height; ++row) {
            const double y = row * 18 - offset;
            const Color color = row == highlighted ? Color::rgb(0xE5484D) : squares;
            painter.fillRect({4, y + 3, 12, 12}, color);
            TextLayout line("Line " + std::to_string(row) + " of the scrolled document", {"Noto Sans", 10, false, false},
                            scale);
            line.setDefaultColor(Color::rgb(0x1C1C1E));
            line.beginLayout();
            line.createLine(geometry().width - 24, {22, y + 1});
            line.endLayout();
            line.draw(painter, {});
        }
    }
};

struct DocumentWindow
{
    Window window;
    Document* document = nullptr;

    DocumentWindow(double scale, double offset)
    {
        auto root = std::make_unique<View>();
        document = root->addChild<Document>();
        document->setGeometry({10, 20, 220, 150});
        document->setLayered(true);
        document->setOpaqueLayer(true);
        document->offset = offset;
        window.resize({240, 180});
        window.setBackgroundColor(Color::rgb(0xF6F6F7));
        window.setContentView(std::move(root));
        window.setScale(scale);
        window.show();
    }
};

} // namespace

TEST(CompositingTest, ScrolledLayersMatchAFreshPaint)
{
    for (const double scale : {1.0, 1.5, 2.0}) {
        DocumentWindow scrolled(scale, 40);
        uint64_t now = 1000000000;
        for (int i = 0; i < 3; ++i)
            TestInput::presentFrame(scrolled.window, now += kFrameNs);
        Document* document = scrolled.document;
        const RectF all = document->localRect();

        // Whole device pixels at every scale: only the uncovered rows paint.
        for (const double step : {2.0, -6.0, 30.0}) {
            document->offset += step;
            document->scrollPainted(all, 0, -step);
            const Image incremental = TestInput::presentFrame(scrolled.window, now += kFrameNs);
            DocumentWindow fresh(scale, document->offset);
            EXPECT_EQ(mismatches(incremental, fresh.window.grab()), 0) << scale << " step " << step;
        }

        // Squares recoloured without a paint stay as they were where the
        // texture moved, and take the colour only where it uncovered.
        document->squares = Color::rgb(0x30A46C);
        document->offset += 30;
        document->scrollPainted(all, 0, -30);
        Image moved = TestInput::presentFrame(scrolled.window, now += kFrameNs);
        const int top = int(std::lround(20 * scale));
        const int uncovered = int(std::lround((20 + all.height - 30) * scale));
        int greenAbove = 0;
        int greenBelow = 0;
        for (int y = top; y < int(std::lround((20 + all.height) * scale)); ++y) {
            for (int x = 0; x < moved.width(); ++x) {
                const Color c = moved.pixel(x, y);
                if (c.r == 0x30 && c.g == 0xA4 && c.b == 0x6C)
                    ++(y < uncovered ? greenAbove : greenBelow);
            }
        }
        EXPECT_EQ(greenAbove, 0) << scale;
        EXPECT_GT(greenBelow, 0) << scale;
        document->squares = Color::rgb(0x2E7DF7);
        document->requestPaint();
        TestInput::presentFrame(scrolled.window, now += kFrameNs);

        // A change waiting to be painted moves with the content.
        document->highlighted = int(document->offset / 18) + 3;
        document->requestPaint({0, document->highlighted * 18 - document->offset, all.width, 18});
        document->offset += 4;
        document->scrollPainted(all, 0, -4);
        Image incremental = TestInput::presentFrame(scrolled.window, now += kFrameNs);
        DocumentWindow fresh(scale, document->offset);
        fresh.document->highlighted = document->highlighted;
        EXPECT_EQ(mismatches(incremental, fresh.window.grab()), 0) << scale << " with waiting damage";

        // A move of part of a device pixel paints the rect again.
        const int paints = document->paints;
        document->offset += 1.0 / 3;
        document->scrollPainted(all, 0, -1.0 / 3);
        incremental = TestInput::presentFrame(scrolled.window, now += kFrameNs);
        EXPECT_EQ(document->paints, paints + 1);
        DocumentWindow fractional(scale, document->offset);
        fractional.document->highlighted = document->highlighted;
        EXPECT_EQ(mismatches(incremental, fractional.window.grab()), 0) << scale << " fractional";
    }
}

TEST(CompositingTest, ResizedLayersMatchAFreshPaint)
{
    for (const double scale : {1.0, 1.5}) {
        DocumentWindow resized(scale, 10);
        uint64_t now = 1000000000;
        TestInput::presentFrame(resized.window, now += kFrameNs);
        // Growing, shrinking and growing again, inside the texture's room and past it.
        for (const double width : {226.0, 214.0, 230.0, 160.0, 228.0}) {
            resized.document->setGeometry({10, 20, width, 150 - (230 - width) / 4});
            const Image incremental = TestInput::presentFrame(resized.window, now += kFrameNs);
            DocumentWindow fresh(scale, 10);
            fresh.document->setGeometry(resized.document->geometry());
            EXPECT_EQ(mismatches(incremental, fresh.window.grab()), 0) << scale << " width " << width;
        }
    }
}

TEST(CompositingTest, OpaqueLayersKeepSubpixelText)
{
    DocumentWindow layered(1.0, 0);
    DocumentWindow direct(1.0, 0);
    direct.document->setLayered(false);
    EXPECT_EQ(mismatches(layered.window.grab(), direct.window.grab()), 0);
}

TEST(CompositingTest, LayeredOpacityAppliesToTheGroup)
{
    Window window;
    auto root = std::make_unique<View>();
    View* group = root->addChild<View>();
    group->setGeometry({0, 0, 100, 100});
    auto* back = group->addChild<Shapes>();
    back->setGeometry({0, 0, 60, 60});
    back->color = Color::rgb(0xFF0000);
    auto* front = group->addChild<Shapes>();
    front->setGeometry({30, 30, 60, 60});
    front->color = Color::rgb(0x0000FF);
    window.resize({100, 100});
    window.setBackgroundColor(Color::rgb(0xFFFFFF));
    window.setContentView(std::move(root));
    window.show();

    group->setOpacity(0.5);
    // Where the two overlap, only the front one shows through the group,
    // while separately the back one shows through the front one.
    const Color separate = window.grab().pixel(32, 58);
    group->setLayered(true);
    const Color together = window.grab().pixel(32, 58);
    EXPECT_NEAR(separate.g, 64, 2);
    EXPECT_NEAR(separate.b, 191, 2);
    EXPECT_NEAR(together.g, 128, 2);
    EXPECT_GE(together.b, 254);
}

TEST(CompositingTest, InputFollowsTheTransform)
{
    Window window;
    auto root = std::make_unique<View>();
    Button* button = root->addChild<Button>("Moved");
    button->setGeometry({10, 10, 80, 32});
    int clicks = 0;
    button->onClicked = [&clicks] { ++clicks; };
    window.resize({300, 100});
    window.setContentView(std::move(root));
    window.show();
    window.grab();

    button->setTransform({{150, 0}, 1});
    window.grab();
    TestInput::click(window, {50, 26});
    EXPECT_EQ(clicks, 0);
    TestInput::click(window, {200, 26});
    EXPECT_EQ(clicks, 1);
    EXPECT_EQ(button->mapToWindow({0, 0}).x, 160);
}
