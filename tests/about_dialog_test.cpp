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
#include "ui/cell_layout.h"
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

    // The column's first child, the icon.
    View* icon() const
    {
        const std::vector<ColumnLayout*> columns = viewsOf<ColumnLayout>(dialog.window());
        return columns.empty() ? nullptr : columns.front()->children().front().get();
    }

    Window parent;
    AboutDialog dialog;
    std::vector<std::string> links;
    int closed = 0;
};

} // namespace

// 300 wide; the column of icon, gap, name, description and link under a
// 40 high header, another 40 under it; everything centred.
TEST(AboutDialogTest, LaysOutAsFishUI)
{
    // The reference was measured in Noto Sans, whatever the machine's
    // default face.
    const Theme original = Theme::current();
    Theme theme = original;
    theme.fontFamily = "Noto Sans";
    Theme::setCurrent(theme);

    Fixture f;
    Window* window = f.dialog.window();
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->transientParent(), &f.parent);
    const std::vector<Label*> labels = viewsOf<Label>(window);
    ASSERT_EQ(labels.size(), 3u);
    ASSERT_NE(f.icon(), nullptr);
    EXPECT_EQ(f.icon()->geometry(), (RectF{118, 0, 64, 64}));
    EXPECT_EQ(f.icon()->mapToWindow({}).y, 40);

    // Spacing 5 and the 12 high gap between the icon and the name.
    double y = 64 + 5 + 12 + 5;
    for (Label* label : labels) {
        const RectF geometry = label->geometry();
        EXPECT_EQ(geometry.y, y) << label->text();
        EXPECT_LE(std::abs(geometry.x + geometry.width / 2 - 150), 0.5) << label->text();
        y += geometry.height + 5;
    }
    EXPECT_EQ(labels[0]->text(), "Calendar");
    // Qt 6.8 and FishUI at scale 1 on the laptop: the column 161 high, the
    // labels at 109/75/43 from the left, 86/117/139 down, 26/17/17 high.
    EXPECT_EQ(labels[0]->geometry(), (RectF{109, 86, 82, 26}));
    EXPECT_EQ(labels[1]->geometry(), (RectF{75, 117, 150, 17}));
    EXPECT_EQ(labels[2]->geometry(), (RectF{43, 139, 215, 17}));
    EXPECT_EQ(window->size().height, 241);
    // The last label's bottom, the filler's spacing, and the bottom header height.
    EXPECT_EQ(window->size().width, 300);
    EXPECT_EQ(window->size().height, std::ceil(40 + y + 40));

    const std::vector<WindowHeader*> headers = viewsOf<WindowHeader>(window);
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers[0]->geometry(), (RectF{0, 0, 300, 40}));

    Theme::setCurrent(original);
}

TEST(AboutDialogTest, OpensTheLinkAndCloses)
{
    Fixture f;
    Window* window = f.dialog.window();
    Label* link = viewsOf<Label>(window).back();
    const PointF centre = link->mapToWindow({link->geometry().width / 2, link->geometry().height / 2});
    TestInput::click(*window, centre);
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

// FishUI's Image takes the provider's pixmap at the window's scale: the
// 64 icon is 128 logical at 150%, and the dialog grows with it.
TEST(AboutDialogTest, IconTakesTheProvidersSize)
{
    Fixture f(1.5);
    ASSERT_NE(f.icon(), nullptr);
    EXPECT_EQ(f.icon()->geometry(), (RectF{86, 0, 128, 128}));
    const std::vector<Label*> labels = viewsOf<Label>(f.dialog.window());
    ASSERT_FALSE(labels.empty());
    EXPECT_EQ(labels[0]->geometry().y, 128 + 5 + 12 + 5);
}
