// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "base/timer.h"
#include "test_views.h"
#include "ui/application.h"
#include "ui/button.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/menu.h"
#include "ui/test_driver.h"
#include "ui/text_field.h"
#include "ui/window.h"

using namespace cutefish;
using namespace cutefish::ui;

namespace {

// Takes every press over it.
class Sheet : public test::Box
{
public:
    Sheet()
        : Box({10, 10})
    {
    }

    bool acceptsPointer() const override { return true; }
};

struct Fixture
{
    Fixture()
    {
        auto content = std::make_unique<Column>();
        content->setSpacing(10);
        ok = content->addChild<Button>("OK");
        ok->setName("form:ok");
        ok->onClicked = [this] { ++okClicks; };
        status = content->addChild<Label>("Ready");
        status->setName("form:status");
        field = content->addChild<TextField>();
        field->setName("form:name");
        Button* hidden = content->addChild<Button>("Hidden");
        hidden->setName("form:hidden");
        hidden->onClicked = [this] { ++hiddenClicks; };
        hidden->setVisible(false);
        View* folded = content->addChild<test::Box>(SizeF{50, 20});
        folded->setName("form:folded");
        folded->setVisible(false);
        folded->addChild<Button>("Inner")->setName("form:inner");
        content->addChild<Label>("One")->setName("form:twin");
        content->addChild<Label>("Two")->setName("form:twin");

        // A strip that shows only its first 50 points of what it holds.
        clip = content->addChild<test::Box>(SizeF{50, 20});
        clip->setClipsChildren(true);
        partly = clip->addChild<Button>("Partly");
        partly->setName("strip:partly");
        partly->onClicked = [this] { ++partlyClicks; };
        Button* outside = clip->addChild<Button>("Outside");
        outside->setName("strip:outside");

        window.resize({200, 400});
        window.setContentView(std::move(content));
        window.show();
        window.grab();
        partly->setGeometry({30, 0, 100, 20});
        outside->setGeometry({60, 0, 40, 20});
    }

    bool failed(const std::string& part) const
    {
        for (const std::string& failure : failures) {
            if (failure.find(part) != std::string::npos)
                return true;
        }
        return false;
    }

    std::vector<std::string> failures;
    Window window;
    TestDriver driver{window, [this](const std::string& message) { failures.push_back(message); }};
    Button* ok;
    Label* status;
    TextField* field;
    View* clip;
    Button* partly;
    int okClicks = 0;
    int hiddenClicks = 0;
    int partlyClicks = 0;
};

} // namespace

TEST(TestDriverTest, FindsViewsByName)
{
    Fixture fixture;
    EXPECT_EQ(fixture.driver.find("form:ok"), fixture.ok);
    EXPECT_EQ(fixture.driver.find<Label>("form:status"), fixture.status);
    EXPECT_EQ(fixture.driver.findAll<Label>("form:twin").size(), 2u);
    EXPECT_EQ(fixture.driver.findAll("form:missing").size(), 0u);
    EXPECT_TRUE(fixture.failures.empty());

    // Each miss names the view.
    EXPECT_EQ(fixture.driver.find("form:missing"), nullptr);
    EXPECT_TRUE(fixture.failed("no view named \"form:missing\""));
    EXPECT_EQ(fixture.driver.find("form:twin"), nullptr);
    EXPECT_TRUE(fixture.failed("2 views named \"form:twin\""));
    EXPECT_EQ(fixture.driver.find<Label>("form:ok"), nullptr);
    EXPECT_TRUE(fixture.failed("\"form:ok\" is not a view of the type asked for"));
}

TEST(TestDriverTest, FindsViewsOverTheContent)
{
    Fixture fixture;
    Sheet* sheet = fixture.window.overlay()->addChild<Sheet>();
    sheet->setName("overlay:sheet");
    EXPECT_EQ(fixture.driver.find("overlay:sheet"), sheet);

    // The sheet takes the press: the click does not happen.
    EXPECT_FALSE(fixture.driver.click("form:ok"));
    EXPECT_TRUE(fixture.failed("\"form:ok\" is covered by \"overlay:sheet\""));
    EXPECT_EQ(fixture.okClicks, 0);
}

TEST(TestDriverTest, Clicks)
{
    Fixture fixture;
    EXPECT_TRUE(fixture.driver.click("form:ok"));
    EXPECT_EQ(fixture.okClicks, 1);
    // Right after the first, as a user clicking again.
    EXPECT_TRUE(fixture.driver.click("form:ok"));
    EXPECT_EQ(fixture.okClicks, 2);
    EXPECT_TRUE(fixture.failures.empty());

    EXPECT_FALSE(fixture.driver.click("form:hidden"));
    EXPECT_TRUE(fixture.failed("\"form:hidden\" is hidden"));
    EXPECT_EQ(fixture.hiddenClicks, 0);
    EXPECT_FALSE(fixture.driver.click("form:inner"));
    EXPECT_TRUE(fixture.failed("\"form:inner\" is hidden"));
    EXPECT_FALSE(fixture.driver.click("form:missing"));
    EXPECT_TRUE(fixture.failed("no view named \"form:missing\""));
}

TEST(TestDriverTest, ClicksWhatShowsOfAView)
{
    Fixture fixture;
    EXPECT_TRUE(fixture.driver.click("strip:partly"));
    EXPECT_EQ(fixture.partlyClicks, 1);
    EXPECT_TRUE(fixture.failures.empty());

    EXPECT_FALSE(fixture.driver.click("strip:outside"));
    EXPECT_TRUE(fixture.failed("\"strip:outside\" shows nowhere in the window"));
}

TEST(TestDriverTest, ShowsAndFrames)
{
    Fixture fixture;
    EXPECT_TRUE(fixture.driver.shows("form:ok"));
    EXPECT_FALSE(fixture.driver.shows("form:hidden"));
    EXPECT_FALSE(fixture.driver.shows("form:inner"));
    EXPECT_FALSE(fixture.driver.shows("form:missing"));
    EXPECT_TRUE(fixture.failures.empty());

    const RectF ok = fixture.driver.frame("form:ok");
    const PointF topLeft = fixture.ok->mapToWindow({});
    EXPECT_EQ(ok, (RectF{topLeft.x, topLeft.y, fixture.ok->geometry().width, fixture.ok->geometry().height}));
    EXPECT_GT(ok.width, 0);
    EXPECT_GE(fixture.driver.frame("form:status").y, ok.bottom());
    const RectF partly = fixture.driver.frame("strip:partly");
    EXPECT_EQ(partly.x - fixture.clip->mapToWindow({}).x, 30);
    EXPECT_EQ(partly.width, 100);

    EXPECT_EQ(fixture.driver.frame("form:missing"), RectF{});
    EXPECT_TRUE(fixture.failed("no view named \"form:missing\""));
}

TEST(TestDriverTest, ReadsAndTypesText)
{
    Fixture fixture;
    EXPECT_EQ(fixture.driver.text("form:status"), "Ready");
    EXPECT_EQ(fixture.driver.text("form:ok"), "OK");
    EXPECT_EQ(fixture.driver.text("form:name"), "");
    EXPECT_TRUE(fixture.failures.empty());

    fixture.driver.setKeyboardFocus(true);
    EXPECT_FALSE(fixture.driver.hasFocus("form:name"));
    EXPECT_TRUE(fixture.driver.focus("form:name"));
    EXPECT_TRUE(fixture.driver.hasFocus("form:name"));
    fixture.driver.typeText("abc");
    EXPECT_EQ(fixture.driver.text("form:name"), "abc");
    fixture.driver.typeKey(key::BackSpace);
    EXPECT_EQ(fixture.driver.text("form:name"), "ab");
    KeyEvent d;
    d.key = 'd';
    d.text = "d";
    fixture.driver.pressKey(d);
    fixture.driver.releaseKey(d);
    EXPECT_EQ(fixture.driver.text("form:name"), "abd");
    EXPECT_TRUE(fixture.failures.empty());

    EXPECT_FALSE(fixture.driver.focus("form:status"));
    EXPECT_TRUE(fixture.failed("\"form:status\" does not take the focus"));
    EXPECT_EQ(fixture.driver.text("strip:missing"), "");
    EXPECT_TRUE(fixture.failed("no view named \"strip:missing\""));
    fixture.clip->setName("strip");
    EXPECT_EQ(fixture.driver.text("strip"), "");
    EXPECT_TRUE(fixture.failed("\"strip\" holds 2 views with text, not one"));
}

TEST(TestDriverTest, ChoosesMenuItems)
{
    Fixture fixture;
    int copies = 0;
    int opened = 0;
    bool canPaste = false;
    Menu menu;
    Menu* edit = menu.addSubmenu("Edit");
    edit->addItem("Copy", [&] { ++copies; });
    const size_t paste = edit->addItem("Paste", [] {});
    const size_t cut = edit->addItem("Cut", [] {});
    edit->setItemVisible(cut, false);
    edit->onAboutToShow = [&] {
        ++opened;
        edit->setItemEnabled(paste, canPaste);
    };
    fixture.window.setMainMenu(&menu);

    EXPECT_TRUE(fixture.driver.chooseMenuItem("Edit", "Copy"));
    EXPECT_EQ(copies, 1);
    EXPECT_EQ(opened, 1);
    EXPECT_TRUE(fixture.failures.empty());

    EXPECT_FALSE(fixture.driver.chooseMenuItem("Edit", "Paste"));
    EXPECT_TRUE(fixture.failed("\"Edit\" > \"Paste\" is disabled"));
    canPaste = true;
    EXPECT_TRUE(fixture.driver.chooseMenuItem("Edit", "Paste"));
    EXPECT_FALSE(fixture.driver.chooseMenuItem("Edit", "Cut"));
    EXPECT_TRUE(fixture.failed("no item \"Cut\" in the menu \"Edit\""));
    EXPECT_FALSE(fixture.driver.chooseMenuItem("View", "Zoom"));
    EXPECT_TRUE(fixture.failed("no menu \"View\" in the main menu"));
    fixture.window.setMainMenu(nullptr);
}

TEST(TestDriverTest, Activates)
{
    Fixture fixture;
    fixture.driver.setActive(true);
    EXPECT_TRUE(fixture.window.states().activated);
    EXPECT_TRUE(TestDriver::waitFor([&] { return fixture.window.isActive(); }));
    fixture.driver.setActive(false);
    EXPECT_FALSE(fixture.window.states().activated);
    EXPECT_TRUE(TestDriver::waitFor([&] { return !fixture.window.isActive(); }));
}

TEST(TestDriverTest, WaitsForTheLoop)
{
    bool fired = false;
    base::Timer timer(&Application::instance()->loop());
    timer.setSingleShot(true);
    timer.onTimeout = [&] { fired = true; };
    timer.start(std::chrono::milliseconds(20));
    EXPECT_TRUE(TestDriver::waitFor([&] { return fired; }));
    EXPECT_FALSE(TestDriver::waitFor([] { return false; }, std::chrono::milliseconds(30)));
}
