// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "base/event_loop.h"
#include "ui/about_dialog.h"
#include "ui/application.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/test_input.h"
#include "ui/theme.h"
#include "ui/window.h"
#include "ui/window_header.h"

using namespace cutefish::ui;

namespace {

void collect(View* view, const std::function<void(View*)>& visit)
{
    visit(view);
    for (const auto& child : view->children())
        collect(child.get(), visit);
}

template<typename T>
std::vector<T*> viewsOf(Window* window)
{
    std::vector<T*> views;
    collect(window->contentView(), [&views](View* view) {
        if (auto* typed = dynamic_cast<T*>(view))
            views.push_back(typed);
    });
    return views;
}

struct Fixture
{
    explicit Fixture(double scale = 1)
        : dialog(&parent)
    {
        parent.setScale(scale);
        parent.resize({800, 600});
        parent.setContentView(std::make_unique<View>());
        parent.show();
        dialog.setName("Calendar");
        dialog.setDescription("Keeps track of your events.");
        dialog.setIconSource("office-calendar");
        dialog.setLink("https://github.com/cutefishos/calendar");
        dialog.onLinkActivated = [this](const std::string& link) { links.push_back(link); };
        dialog.onClosed = [this] { ++closed; };
        dialog.show();
        dialog.window()->setScale(scale);
        dialog.window()->grab();
    }

    // The content's child after the header.
    View* icon() const
    {
        const auto& children = dialog.window()->contentView()->children();
        return children.size() > 1 ? children[1].get() : nullptr;
    }

    Window parent;
    AboutDialog dialog;
    std::vector<std::string> links;
    int closed = 0;
};

} // namespace

// 280 wide and taller than that: the icon, the name, the description and
// the link, centred, stacked from the top under the header.
TEST(AboutDialogTest, LaysOutTall)
{
    Fixture f;
    Window* window = f.dialog.window();
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->transientParent(), &f.parent);
    EXPECT_EQ(window->size().width, 280);
    EXPECT_GT(window->size().height, window->size().width);

    const std::vector<WindowHeader*> headers = viewsOf<WindowHeader>(window);
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers[0]->geometry(), (RectF{0, 0, 280, 40}));

    View* icon = f.icon();
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->geometry(), (RectF{84, 80, 112, 112}));

    const auto& children = window->contentView()->children();
    ASSERT_EQ(children.size(), 5u);
    auto* name = dynamic_cast<Label*>(children[2].get());
    auto* description = dynamic_cast<Label*>(children[3].get());
    auto* link = dynamic_cast<Label*>(children[4].get());
    ASSERT_TRUE(name && description && link);
    EXPECT_EQ(name->text(), "Calendar");
    EXPECT_EQ(name->geometry().y, 192 + 20);
    EXPECT_EQ(description->geometry().y, name->geometry().bottom() + 8);
    EXPECT_EQ(description->geometry().width, 200);
    for (Label* label : {name, description, link})
        EXPECT_NEAR(label->geometry().x + label->geometry().width / 2, 140, 0.5) << label->text();
    EXPECT_EQ(link->geometry().y, description->geometry().bottom() + 20);
    EXPECT_EQ(window->size().height, std::ceil(link->geometry().bottom() + 40));
}

TEST(AboutDialogTest, LongDescriptionsWrap)
{
    Window parent;
    parent.resize({800, 600});
    parent.setContentView(std::make_unique<View>());
    parent.show();
    AboutDialog dialog(&parent);
    dialog.setName("Terminal");
    dialog.setDescription("Runs a shell and the programs started in it, and a few more words to wrap.");
    dialog.show();
    dialog.window()->grab();
    const auto& children = dialog.window()->contentView()->children();
    ASSERT_EQ(children.size(), 5u);
    auto* name = dynamic_cast<Label*>(children[2].get());
    auto* description = dynamic_cast<Label*>(children[3].get());
    ASSERT_TRUE(name && description);
    EXPECT_GT(description->geometry().height, 2 * 12);
    EXPECT_EQ(children[4]->geometry().y, description->geometry().bottom() + 20);
    dialog.close();
    Application::instance()->loop().dispatch();
}

TEST(AboutDialogTest, OpensTheLinkAndCloses)
{
    Fixture f;
    Window* window = f.dialog.window();
    Label* link = viewsOf<Label>(window).back();
    const RectF rect = link->geometry();
    TestInput::click(*window, link->mapToWindow({rect.width / 2, rect.height / 2}));
    EXPECT_EQ(f.links, std::vector<std::string>{"https://github.com/cutefishos/calendar"});

    // Shown again: the same window, raised.
    f.dialog.show();
    EXPECT_EQ(f.dialog.window(), window);

    window->onCloseRequested();
    EXPECT_FALSE(f.dialog.isShown());
    EXPECT_EQ(f.closed, 1);
    Application::instance()->loop().dispatch();
}

TEST(AboutDialogTest, EscapeCloses)
{
    Fixture f;
    TestInput::typeKey(*f.dialog.window(), key::Escape);
    EXPECT_FALSE(f.dialog.isShown());
    EXPECT_EQ(f.closed, 1);
    Application::instance()->loop().dispatch();
}

TEST(AboutDialogTest, IconKeepsItsSizeAtEveryScale)
{
    for (const double scale : {1.5, 1.75, 2.0}) {
        Fixture f(scale);
        ASSERT_NE(f.icon(), nullptr);
        EXPECT_EQ(f.icon()->geometry().width, 112) << scale;
        EXPECT_EQ(f.icon()->geometry().height, 112) << scale;
    }
}
