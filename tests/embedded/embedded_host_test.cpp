// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <optional>
#include <poll.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/blurred_copy.h"
#include "ui/bubble_shape.h"
#include "ui/button.h"
#include "ui/combo_box.h"
#include "ui/drag.h"
#include "ui/embedded_host.h"
#include "ui/embedded_window.h"
#include "ui/icon.h"
#include "ui/icon_image.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/progress_bar.h"
#include "ui/styled_label.h"
#include "ui/text_field.h"
#include "ui/view.h"

using namespace cutefish::ui;
using namespace std::chrono_literals;

namespace {

// Records what the windows ask of the host.
class Host final : public EmbeddedHost
{
public:
    Host() { EmbeddedWindow::setHost(this); }
    ~Host() override { EmbeddedWindow::setHost(nullptr); }

    void popupShown(EmbeddedWindow* popup) override
    {
        shown.push_back(popup);
        open.push_back(popup);
    }
    void popupRepositioned(EmbeddedWindow* popup) override { repositioned.push_back(popup); }
    void popupHidden(EmbeddedWindow* popup) override
    {
        hidden.push_back(popup);
        std::erase(open, popup);
    }
    void activationRequested(EmbeddedWindow* window) override { activated.push_back(window); }
    void textInputChanged(EmbeddedWindow*, const TextInputState* state, bool fromInputMethod) override
    {
        textInput.push_back(state ? std::optional<TextInputState>(*state) : std::nullopt);
        causes.push_back(fromInputMethod);
    }
    void blurChanged(EmbeddedWindow* window) override { blurred.push_back(window); }
    bool startDrag(EmbeddedWindow*, DragSource source) override
    {
        drag = std::move(source);
        return acceptDrags;
    }

    std::vector<EmbeddedWindow*> shown;
    std::vector<EmbeddedWindow*> open;
    std::vector<EmbeddedWindow*> repositioned;
    std::vector<EmbeddedWindow*> hidden;
    std::vector<EmbeddedWindow*> activated;
    std::vector<std::optional<TextInputState>> textInput;
    std::vector<bool> causes;
    std::vector<EmbeddedWindow*> blurred;
    std::optional<DragSource> drag;
    bool acceptDrags = true;
};

class Free : public View
{
};

// A view that takes the pointer and records what reaches it.
class Probe : public View
{
public:
    bool acceptsPointer() const override { return true; }

    std::vector<std::string> events;
    std::vector<PointerDevice> devices;
    std::vector<GestureEvent::Type> gestures;
    // Drops it takes, by MIME type.
    std::string dropType;
    uint32_t dropAction = CopyAction;
    std::optional<std::string> dropped;
    bool dragStarts = false;
    std::optional<DragSource> source;

protected:
    void pointerPress(const PointerEvent& event) override
    {
        events.push_back("press");
        devices.push_back(event.device);
        if (dragStarts && source) {
            const bool started = startDrag(std::move(*source));
            source.reset();
            events.push_back(started ? "drag" : "nodrag");
        }
    }
    void pointerMove(const PointerEvent&) override { events.push_back("move"); }
    void pointerRelease(const PointerEvent& event) override
    {
        events.push_back(localRect().contains(event.position) ? "release" : "release-outside");
    }
    bool gesture(const GestureEvent& event) override
    {
        gestures.push_back(event.type);
        return true;
    }
    bool canDrop(const DropOffer& offer) const override { return !dropType.empty() && offer.hasMimeType(dropType); }
    void dragEnter(const DragEvent&) override { events.push_back("enter"); }
    uint32_t dragUpdate(const DragEvent&) override
    {
        events.push_back("update");
        return dropAction;
    }
    void dragLeave() override { events.push_back("leave"); }
    uint32_t drop(const DragEvent& event) override
    {
        events.push_back("drop");
        event.offer->read(dropType, [this](std::string data) { dropped = std::move(data); });
        return dropAction;
    }
};

// As a host's wl_data_offer: data from memory, handed over later.
class Offer final : public DropOffer
{
public:
    Offer(std::vector<std::string> types, std::string data)
        : m_types(std::move(types))
        , m_data(std::move(data))
    {
    }

    const std::vector<std::string>& mimeTypes() const override { return m_types; }
    uint32_t sourceActions() const override { return CopyAction | MoveAction; }
    void read(const std::string&, std::function<void(std::string)> done) override
    {
        Application::instance()->loop().post([done, data = m_data] { done(data); });
    }

private:
    std::vector<std::string> m_types;
    std::string m_data;
};

} // namespace

static void runLoop(std::chrono::milliseconds duration)
{
    cutefish::base::EventLoop& loop = Application::instance()->loop();
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        pollfd entry{loop.fd(), POLLIN, 0};
        if (poll(&entry, 1, 5) == 1)
            loop.dispatch();
    }
}

static void press(EmbeddedWindow& window, PointF position, MouseButton button = MouseButton::Left)
{
    window.pointerMove(position);
    window.pointerButton(button, true);
    window.pointerButton(button, false);
}

static void type(EmbeddedWindow& window, uint32_t keysym, std::string text = {})
{
    KeyEvent event;
    event.key = keysym;
    event.text = std::move(text);
    window.key(event, true);
    window.key(event, false);
}

// The middle of a menu's row: five pixels of margin and rows of 26.
static PointF menuRow(int row)
{
    return {40, 5 + row * 26 + 13.0};
}

TEST(EmbeddedHostTest, MenuOpensAsAHostPlacedPopup)
{
    Host host;
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Probe* probe = content->addChild<Probe>();
    probe->setGeometry({0, 0, 200, 100});
    window.setContentView(std::move(content));
    window.setScale(1.5);
    window.setScreenSize({1280, 800});
    window.setSize({200, 100});
    window.setKeyboardFocus(true);
    window.render();

    int chosen = -1;
    Menu menu;
    menu.addItem("Open", [&chosen] { chosen = 0; });
    menu.addItem("Close", [&chosen] { chosen = 1; });
    menu.popup(&window, {30, 40});

    ASSERT_EQ(host.shown.size(), 1u);
    EmbeddedWindow* popup = host.shown[0];
    EXPECT_EQ(popup->popupParent(), &window);
    EXPECT_TRUE(popup->isGrabbingPopup());
    EXPECT_EQ(popup->popupPlacement().anchorRect, (RectF{30, 40, 1, 1}));
    EXPECT_EQ(popup->popupPlacement().anchor, PopupAnchor::TopLeft);
    EXPECT_EQ(popup->popupPlacement().gravity, PopupAnchor::BottomRight);
    EXPECT_EQ(popup->scale(), 1.5);
    EXPECT_GT(popup->size().width, 140);
    EXPECT_EQ(popup->size().height, 5 + 26 + 26 + 5);
    // The menu's body is blurred behind, at the window radius.
    EXPECT_EQ(popup->blurRect(), (RectF{0, 0, popup->size().width, popup->size().height}));
    EXPECT_GT(popup->blurRadius(), 0);

    // Sized and wanting a frame from the start.
    EXPECT_TRUE(popup->needsRender());
    popup->setPopupGeometry({30, 40, popup->size().width, popup->size().height});
    const EmbeddedWindow::Frame& frame = popup->render();
    ASSERT_NE(frame.pixels, nullptr);
    EXPECT_EQ(frame.width, int(std::lround(popup->size().width * 1.5)));

    // Keys reach the menu through the root window.
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Return, "\r");
    EXPECT_EQ(chosen, 1);
    ASSERT_EQ(host.hidden.size(), 1u);
    EXPECT_EQ(host.hidden[0], popup);
    EXPECT_FALSE(menu.isOpen());
    // Keys are the window's own again.
    EXPECT_TRUE(probe->events.empty());
    runLoop(10ms);

    // A click on a row, in the popup's coordinates.
    menu.popup(&window, {30, 40});
    ASSERT_EQ(host.shown.size(), 2u);
    popup = host.shown[1];
    popup->render();
    popup->pointerEnter(menuRow(0));
    popup->pointerButton(MouseButton::Left, true);
    popup->pointerButton(MouseButton::Left, false);
    EXPECT_EQ(chosen, 0);
    EXPECT_EQ(host.hidden.size(), 2u);
    runLoop(10ms);

    // A press elsewhere breaks the grab: the host dismisses the chain.
    menu.popup(&window, {30, 40});
    ASSERT_EQ(host.shown.size(), 3u);
    window.closePopups();
    EXPECT_FALSE(menu.isOpen());
    EXPECT_TRUE(host.open.empty());
    runLoop(10ms);
}

TEST(EmbeddedHostTest, SubmenusOpenBesideTheirRows)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setScreenSize({1280, 800});
    window.setSize({300, 200});
    window.setKeyboardFocus(true);
    window.render();

    std::string chosen;
    Menu menu;
    menu.addItem("First", [&chosen] { chosen = "first"; });
    Menu* more = menu.addSubmenu("More");
    more->addItem("Deep", [&chosen] { chosen = "deep"; });
    more->addItem("Deeper", [&chosen] { chosen = "deeper"; });
    menu.addItem("Last", [&chosen] { chosen = "last"; });
    menu.popup(&window, {10, 10});
    ASSERT_EQ(host.shown.size(), 1u);
    EmbeddedWindow* root = host.shown[0];
    root->render();

    // Hover opens it, against the menu's frame, two pixels over it, its
    // first row level with the row; mirrored when it does not fit.
    root->pointerEnter(menuRow(1));
    ASSERT_EQ(host.shown.size(), 2u);
    EmbeddedWindow* sub = host.shown[1];
    EXPECT_EQ(sub->popupParent(), root);
    EXPECT_FALSE(sub->isGrabbingPopup());
    const PopupPlacement& placement = sub->popupPlacement();
    EXPECT_EQ(placement.anchor, PopupAnchor::TopRight);
    EXPECT_EQ(placement.gravity, PopupAnchor::BottomRight);
    EXPECT_TRUE(placement.flipX);
    EXPECT_FALSE(placement.flipY);
    EXPECT_TRUE(placement.slide);
    EXPECT_DOUBLE_EQ(placement.anchorRect.x, 2);
    EXPECT_DOUBLE_EQ(placement.anchorRect.right(), root->size().width - 2);
    EXPECT_DOUBLE_EQ(placement.anchorRect.y, 26);
    sub->render();

    // Another row takes it down.
    root->pointerMove(menuRow(0));
    EXPECT_EQ(host.hidden.size(), 1u);
    EXPECT_EQ(host.hidden.back(), sub);
    runLoop(10ms);

    // Back over the row, then into the submenu: it stays.
    root->pointerMove(menuRow(1));
    ASSERT_EQ(host.shown.size(), 3u);
    sub = host.shown[2];
    sub->render();
    root->pointerLeave();
    sub->pointerEnter(menuRow(0));
    runLoop(80ms);
    EXPECT_TRUE(more->isOpen());

    // Leaving it for nowhere closes it after a moment.
    sub->pointerLeave();
    runLoop(80ms);
    EXPECT_FALSE(more->isOpen());
    EXPECT_TRUE(menu.isOpen());

    // Keys: Right opens it with its first row lit, Left closes it, Return
    // in it chooses and closes the chain.
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Right);
    ASSERT_TRUE(more->isOpen());
    type(window, XKB_KEY_Left);
    EXPECT_FALSE(more->isOpen());
    EXPECT_TRUE(menu.isOpen());
    type(window, XKB_KEY_Right);
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Return, "\r");
    EXPECT_EQ(chosen, "deeper");
    EXPECT_FALSE(more->isOpen());
    EXPECT_FALSE(menu.isOpen());
    EXPECT_TRUE(host.open.empty());
    runLoop(10ms);

    // Escape in a submenu closes only it.
    menu.popup(&window, {10, 10});
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Return, "\r");
    ASSERT_TRUE(more->isOpen());
    type(window, XKB_KEY_Escape);
    EXPECT_FALSE(more->isOpen());
    EXPECT_TRUE(menu.isOpen());
    type(window, XKB_KEY_Escape);
    EXPECT_FALSE(menu.isOpen());
    runLoop(10ms);
}

TEST(EmbeddedHostTest, AboutToShowFillsAnEmptySubmenu)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setSize({300, 200});
    window.render();

    Menu menu;
    Menu* lazy = menu.addSubmenu("Lazy");
    Menu* empty = menu.addSubmenu("Empty");
    (void)empty;
    lazy->onAboutToShow = [lazy] {
        lazy->clear();
        lazy->addItem("Filled", [] {});
    };
    menu.popup(&window, {0, 0});
    EmbeddedWindow* root = host.shown.at(0);
    root->render();
    root->pointerEnter(menuRow(1));
    EXPECT_EQ(host.shown.size(), 1u);
    root->pointerMove(menuRow(0));
    ASSERT_EQ(host.shown.size(), 2u);
    EXPECT_TRUE(lazy->isOpen());
    menu.close();
    EXPECT_TRUE(host.open.empty());
    runLoop(10ms);
}

TEST(EmbeddedHostTest, ComboBoxAndTextFieldMenus)
{
    Host host;
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    ComboBox* box = content->addChild<ComboBox>(std::vector<std::string>{"One", "Two", "Three"});
    box->setGeometry({10, 10, 160, 36});
    box->setCurrentIndex(0);
    TextField* field = content->addChild<TextField>("text");
    field->setGeometry({10, 60, 160, 36});
    window.setContentView(std::move(content));
    window.setScreenSize({1280, 800});
    window.setSize({200, 120});
    window.render();

    int activated = -1;
    box->onActivated = [&activated](int index) { activated = index; };
    press(window, {50, 28});
    ASSERT_EQ(host.shown.size(), 1u);
    EmbeddedWindow* dropDown = host.shown[0];
    EXPECT_EQ(dropDown->popupParent(), &window);
    // Under the box, gap pixels below it, flipped above when it does not fit.
    EXPECT_EQ(dropDown->popupPlacement().anchor, PopupAnchor::BottomLeft);
    EXPECT_GE(dropDown->size().width, 160);
    dropDown->render();
    dropDown->pointerEnter(menuRow(2));
    dropDown->pointerButton(MouseButton::Left, true);
    dropDown->pointerButton(MouseButton::Left, false);
    EXPECT_EQ(activated, 2);
    EXPECT_TRUE(host.open.empty());
    runLoop(10ms);

    // The field's context menu, at the pointer.
    press(window, {40, 78}, MouseButton::Right);
    ASSERT_EQ(host.shown.size(), 2u);
    EXPECT_EQ(host.shown[1]->popupPlacement().anchorRect.x, 40);
    window.closePopups();
    EXPECT_TRUE(host.open.empty());
    runLoop(10ms);
}

TEST(EmbeddedHostTest, PopupsGoWithTheirWindow)
{
    Host host;
    auto window = std::make_unique<EmbeddedWindow>();
    window->setContentView(std::make_unique<Free>());
    window->setSize({200, 100});
    window->render();
    Menu menu;
    menu.addItem("One", [] {});
    menu.popup(window.get(), {0, 0});
    ASSERT_EQ(host.open.size(), 1u);
    menu.close();
    runLoop(10ms);
    menu.popup(window.get(), {0, 0});
    ASSERT_EQ(host.open.size(), 1u);
    // A popup whose owner does nothing on close still goes off screen.
    PopupPlacement placement;
    std::unique_ptr<Window> bare = Window::createPopup(window.get(), placement, false);
    bare->resize({50, 20});
    bare->setContentView(std::make_unique<Free>());
    bare->show();
    ASSERT_EQ(host.open.size(), 2u);
    bare->resize({60, 20});
    ASSERT_EQ(host.repositioned.size(), 1u);
    // Moved, it is placed again where the new placement says.
    placement.anchorRect = {30, 40, 1, 1};
    bare->setPopupPlacement(placement);
    ASSERT_EQ(host.repositioned.size(), 2u);
    EXPECT_EQ(host.repositioned[1], host.open[1]);
    EXPECT_EQ(host.open[1]->popupPlacement().anchorRect, (RectF{30, 40, 1, 1}));
    window->closePopups();
    EXPECT_TRUE(host.open.empty());
    EXPECT_TRUE(bare->isHidden());
    // Moved while hidden, the host hears of it only when it maps again, at
    // its size and the last placement.
    placement.anchorRect = {50, 10, 1, 1};
    bare->setPopupPlacement(placement);
    EXPECT_EQ(host.repositioned.size(), 2u);
    bare->show();
    EXPECT_FALSE(bare->isHidden());
    ASSERT_EQ(host.open.size(), 1u);
    EXPECT_EQ(host.open[0]->size(), (SizeF{60, 20}));
    EXPECT_EQ(host.open[0]->popupPlacement().anchorRect, (RectF{50, 10, 1, 1}));
    bare->hide();
    EXPECT_TRUE(host.open.empty());
    bare.reset();

    menu.popup(window.get(), {0, 0});
    ASSERT_EQ(host.open.size(), 1u);
    window.reset();
    EXPECT_TRUE(host.open.empty());
    runLoop(10ms);
}

TEST(EmbeddedHostTest, ActivationAndBlurReachTheHost)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setSize({200, 100});
    window.activate();
    ASSERT_EQ(host.activated.size(), 1u);
    EXPECT_EQ(host.activated[0], &window);
    window.setBackgroundBlur({0, 0, 200, 100}, 12);
    window.setBackgroundBlur({0, 0, 200, 100}, 12);
    ASSERT_EQ(host.blurred.size(), 1u);
    EXPECT_EQ(window.blurRect(), (RectF{0, 0, 200, 100}));
    EXPECT_EQ(window.blurRadius(), 12);
}

TEST(EmbeddedHostTest, TextInputDrivesTheInputMethod)
{
    Host host;
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    TextField* field = content->addChild<TextField>();
    field->setGeometry({10, 10, 200, 35});
    TextField* other = content->addChild<TextField>();
    other->setGeometry({10, 50, 200, 35});
    window.setContentView(std::move(content));
    window.setSize({220, 100});
    window.render();

    // Nothing while the window lacks the keyboard.
    field->setFocus();
    EXPECT_TRUE(host.textInput.empty());
    window.setKeyboardFocus(true);
    ASSERT_EQ(host.textInput.size(), 1u);
    ASSERT_TRUE(host.textInput[0]);
    EXPECT_EQ(host.textInput[0]->surroundingText, "");
    EXPECT_EQ(host.textInput[0]->purpose, TextPurpose::Normal);
    EXPECT_GT(host.textInput[0]->cursorRectangle.height, 0);
    EXPECT_FALSE(host.causes[0]);

    // Typed keys change the state; the cause is not the input method.
    type(window, XKB_KEY_a, "a");
    ASSERT_TRUE(host.textInput.back());
    EXPECT_EQ(host.textInput.back()->surroundingText, "a");
    EXPECT_EQ(host.textInput.back()->cursor, 1u);
    EXPECT_FALSE(host.causes.back());

    // A preedit shows without changing the text.
    TextInputEvent preedit;
    preedit.preedit = "ni";
    preedit.preeditCursorBegin = 2;
    preedit.preeditCursorEnd = 2;
    window.textInput(preedit);
    EXPECT_EQ(field->text(), "a");

    // A commit replaces it, after the surrounding text it deletes.
    TextInputEvent commit;
    commit.deleteBefore = 1;
    commit.commit = "\xe4\xbd\xa0";
    window.textInput(commit);
    EXPECT_EQ(field->text(), "\xe4\xbd\xa0");
    ASSERT_TRUE(host.textInput.back());
    EXPECT_EQ(host.textInput.back()->surroundingText, "\xe4\xbd\xa0");
    EXPECT_EQ(host.textInput.back()->cursor, 3u);
    EXPECT_TRUE(host.causes.back());

    // Unchanged states are not sent again.
    const size_t sent = host.textInput.size();
    field->updateTextInput();
    EXPECT_EQ(host.textInput.size(), sent);

    // A password field takes no input method, as on Wayland; no keyboard
    // disables it too.
    other->setEchoMode(TextField::EchoMode::Password);
    other->setFocus();
    EXPECT_FALSE(host.textInput.back());
    field->setFocus();
    ASSERT_TRUE(host.textInput.back());
    window.setKeyboardFocus(false);
    EXPECT_FALSE(host.textInput.back());
}

TEST(EmbeddedHostTest, DropsGoToTheViewThatTakesThem)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Probe* left = content->addChild<Probe>();
    left->setGeometry({0, 0, 100, 100});
    left->dropType = "text/uri-list";
    Probe* right = content->addChild<Probe>();
    right->setGeometry({100, 0, 100, 100});
    right->dropType = "text/plain";
    right->dropAction = MoveAction;
    window.setContentView(std::move(content));
    window.setSize({200, 100});
    window.render();

    Offer offer({"text/uri-list", "text/plain"}, "file:///tmp/a");
    EXPECT_EQ(window.dragEnter({50, 50}, &offer), uint32_t(CopyAction));
    EXPECT_EQ(window.dragMotion({60, 50}), uint32_t(CopyAction));
    EXPECT_EQ(window.dragMotion({150, 50}), uint32_t(MoveAction));
    EXPECT_EQ(left->events, (std::vector<std::string>{"enter", "update", "update", "leave"}));
    EXPECT_EQ(window.drop(), uint32_t(MoveAction));
    EXPECT_EQ(right->events, (std::vector<std::string>{"enter", "update", "drop"}));
    runLoop(10ms);
    EXPECT_EQ(right->dropped, "file:///tmp/a");

    // Nothing takes an offer of other types; a refusing target hears a leave.
    Offer image({"image/png"}, "");
    EXPECT_EQ(window.dragEnter({50, 50}, &image), uint32_t(NoAction));
    window.dragLeave();
    left->events.clear();
    left->dropAction = NoAction;
    EXPECT_EQ(window.dragEnter({50, 50}, &offer), uint32_t(NoAction));
    EXPECT_EQ(window.drop(), uint32_t(NoAction));
    EXPECT_EQ(left->events, (std::vector<std::string>{"enter", "update", "leave"}));
}

TEST(EmbeddedHostTest, DragsStartThroughTheHost)
{
    Host host;
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Probe* probe = content->addChild<Probe>();
    probe->setGeometry({0, 0, 100, 100});
    window.setContentView(std::move(content));
    window.setSize({100, 100});
    window.render();

    DropAction finished = AskAction;
    DragSource source;
    source.mimeTypes = {"application/x-cutefish-dock-item"};
    source.data = [](const std::string& type) { return "item:" + type; };
    source.actions = MoveAction;
    source.icon = Bitmap(48, 48, Bitmap::Format::Argb32Premultiplied);
    source.iconScale = 1.5;
    source.hotSpot = {16, 16};
    source.onFinished = [&finished](DropAction action) { finished = action; };
    probe->source = std::move(source);
    probe->dragStarts = true;

    window.pointerEnter({50, 50});
    window.pointerButton(MouseButton::Left, true);
    // The drag has the pointer: no release reaches the view.
    window.pointerButton(MouseButton::Left, false);
    EXPECT_EQ(probe->events, (std::vector<std::string>{"press", "drag"}));
    ASSERT_TRUE(host.drag);
    EXPECT_EQ(host.drag->mimeTypes, std::vector<std::string>{"application/x-cutefish-dock-item"});
    EXPECT_EQ(host.drag->data("application/x-cutefish-dock-item"), "item:application/x-cutefish-dock-item");
    EXPECT_EQ(host.drag->icon.width(), 48);
    host.drag->onFinished(MoveAction);
    EXPECT_EQ(finished, MoveAction);

    // A refused drag leaves the press as it was.
    host.acceptDrags = false;
    probe->events.clear();
    probe->source = DragSource{};
    window.pointerButton(MouseButton::Left, true);
    window.pointerButton(MouseButton::Left, false);
    EXPECT_EQ(probe->events, (std::vector<std::string>{"press", "nodrag", "release"}));
}

TEST(EmbeddedHostTest, TouchDrivesThePointer)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Button* button = content->addChild<Button>("Go");
    button->setGeometry({10, 10, 80, 30});
    Probe* probe = content->addChild<Probe>();
    probe->setGeometry({100, 0, 100, 100});
    window.setContentView(std::move(content));
    window.setSize({200, 100});
    window.render();

    int clicks = 0;
    button->onClicked = [&clicks] { ++clicks; };
    window.touchDown(3, {50, 25});
    // Other fingers are not taken.
    window.touchDown(4, {150, 50});
    window.touchUp(4);
    window.touchUp(3);
    EXPECT_EQ(clicks, 1);
    EXPECT_TRUE(probe->events.empty());

    // A cancelled touch clicks nothing.
    window.touchDown(5, {50, 25});
    window.touchCancel();
    EXPECT_EQ(clicks, 1);

    window.touchDown(6, {150, 50});
    window.touchMotion(6, {160, 50});
    window.touchUp(6);
    EXPECT_EQ(probe->events, (std::vector<std::string>{"press", "move", "release"}));
    EXPECT_EQ(probe->devices, std::vector<PointerDevice>{PointerDevice::TouchScreen});
    window.touchDown(7, {150, 50});
    window.touchCancel();
    EXPECT_EQ(probe->events.back(), "release-outside");

    // A mouse afterwards is the usual pointer again.
    window.pointerEnter({150, 50});
    window.pointerButton(MouseButton::Left, true);
    window.pointerButton(MouseButton::Left, false);
    EXPECT_EQ(probe->devices.back(), PointerDevice::TouchPad);
}

TEST(EmbeddedHostTest, GesturesStayWithTheViewThatTookThem)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Probe* probe = content->addChild<Probe>();
    probe->setGeometry({0, 0, 100, 100});
    window.setContentView(std::move(content));
    window.setSize({200, 100});
    window.render();

    using Type = GestureEvent::Type;
    const auto send = [&window](Type type) {
        GestureEvent event;
        event.type = type;
        event.fingers = 3;
        window.gesture(event);
    };
    window.pointerEnter({50, 50});
    send(Type::SwipeBegin);
    window.pointerMove({150, 50});
    send(Type::SwipeUpdate);
    send(Type::SwipeEnd);
    // Begun outside the view: nothing.
    send(Type::PinchBegin);
    send(Type::PinchUpdate);
    send(Type::PinchEnd);
    window.pointerMove({50, 50});
    send(Type::HoldBegin);
    send(Type::HoldEnd);
    EXPECT_EQ(probe->gestures, (std::vector<Type>{Type::SwipeBegin, Type::SwipeUpdate, Type::SwipeEnd,
                                                  Type::HoldBegin, Type::HoldEnd}));
}

TEST(EmbeddedHostTest, BubbleMenusPointAtTheirTarget)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setScreenSize({1280, 800});
    window.setSize({1280, 100});
    window.render();

    Menu menu;
    menu.addItem("A window title far too long to fit into any reasonable menu at all, elided", [] {});
    menu.setItemChecked(0, true);
    menu.setItemMaximumWidth(0, 320);
    menu.addSeparator();
    menu.addItem("Quit", [] {});
    menu.setArrow(BubbleShape::Edge::Bottom, {640, 90});
    menu.setContinuousCorners(true);
    menu.setAvailableGeometry(RectF{0, 0, 1280, 1000});

    // MenuSurface's implicit size: rows, margins and the arrow's length.
    const SizeF size = menu.implicitSize(&window);
    EXPECT_EQ(size.width, 320 + 2 * 5);
    EXPECT_EQ(size.height, 26 + 12 + 26 + 2 * 5 + 6);
    EXPECT_GT(menu.minimumEdgeLength(), 40);

    menu.popupAt(&window, {580, 10});
    ASSERT_EQ(host.shown.size(), 1u);
    EmbeddedWindow* popup = host.shown[0];
    EXPECT_EQ(popup->popupPlacement().anchorRect, (RectF{580, 10, 1, 1}));
    EXPECT_EQ(popup->size(), size);
    // The glass is the body, masked by the bubble.
    EXPECT_EQ(popup->blurRect(), (RectF{0, 0, size.width, size.height - 6}));
    EXPECT_FALSE(popup->blurShape().isEmpty());
    EXPECT_TRUE(popup->blurShape().contains({60, size.height - 3}));
    EXPECT_FALSE(popup->blurShape().contains({size.width - 20, size.height - 3}));

    // The host's glass is the body: the menu paints none, only its rows.
    const EmbeddedWindow::Frame& frame = popup->render();
    ASSERT_NE(frame.pixels, nullptr);
    const auto alpha = [&frame](double x, double y) {
        return frame.pixels[size_t(y) * frame.stride + size_t(x) * 4 + 3];
    };
    EXPECT_EQ(alpha(60, size.height - 3), 0);
    EXPECT_EQ(alpha(4, size.height / 2), 0);
    int text = 0;
    for (int x = 0; x < frame.width; ++x)
        text += alpha(x, (size.height - 6 - 5 - 15) * popup->scale()) > 0;
    EXPECT_GT(text, 0);
    menu.close();
    runLoop(10ms);

    // Kept six pixels inside the available geometry.
    menu.popupAt(&window, {1270, 10});
    ASSERT_EQ(host.shown.size(), 2u);
    EXPECT_EQ(host.shown[1]->popupPlacement().anchorRect.x, 1280 - 1 - 6 - size.width);
    menu.close();
    runLoop(10ms);
}

TEST(EmbeddedHostTest, ThemeIconsTintAsSilhouettes)
{
    if (themeIconPath("folder", 32).empty())
        GTEST_SKIP() << "no folder icon in the theme";
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    ThemeIcon* icon = content->addChild<ThemeIcon>("folder", 32);
    icon->setGeometry({0, 0, 32, 32});
    window.setContentView(std::move(content));
    window.setSize({32, 32});
    icon->setColor(Color{0, 0, 0, 255});
    const EmbeddedWindow::Frame& frame = window.render();
    int covered = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const uint8_t* pixel = frame.pixels + size_t(y) * frame.stride + size_t(x) * 4;
            if (pixel[3] > 0)
                ++covered;
            EXPECT_EQ(pixel[0] + pixel[1] + pixel[2], 0);
        }
    }
    EXPECT_GT(covered, 100);
    // Unset: in its own colours again.
    icon->setColor(std::nullopt);
    const EmbeddedWindow::Frame& plain = window.render();
    int coloured = 0;
    for (size_t i = 0; i < size_t(plain.height) * plain.stride; i += 4)
        coloured += plain.pixels[i] + plain.pixels[i + 1] + plain.pixels[i + 2] > 0;
    EXPECT_GT(coloured, 100);
}

// A miss in the theme's icon-theme.cache must leave the cache usable: the
// cache's order puts the application's scalable drawing first.
TEST(EmbeddedHostTest, ThemeIconMissesKeepTheIconCache)
{
    if (themeIconPath("utilities-terminal", 24, 2).find("/apps@2x/scalable/") == std::string::npos)
        GTEST_SKIP() << "no Cutefish theme with its cache";
    EXPECT_EQ(themeIconPath("org.example.NoSuchIcon", 24, 2), "");
    themeIconPath("ai.storyteller.photocraft", 24, 2);
    // Not the cached answer: another size.
    EXPECT_NE(themeIconPath("utilities-terminal", 23, 2).find("/apps@2x/scalable/"), std::string::npos);
    EXPECT_NE(themeIconPath("utilities-terminal", 24, 1.5).find("/apps@2x/scalable/"), std::string::npos);
}

TEST(EmbeddedHostTest, ThemeIconsTakeAnAbsoluteFile)
{
    const std::string path = themeIconPath("utilities-terminal", 48, 2);
    if (path.empty())
        GTEST_SKIP() << "no utilities-terminal in the theme";
    EXPECT_EQ(themeIconPath(path, 48, 2), path);
    EXPECT_EQ(themeIconPath(path + ".missing", 48, 2), "");
    const Bitmap bitmap = themeIconBitmap(path, 48, 2);
    ASSERT_FALSE(bitmap.isNull());
    EXPECT_EQ(bitmap.width(), 96);
}

TEST(EmbeddedHostTest, PopupAtAsksAboutToShowOnce)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setScreenSize({1280, 800});
    window.setSize({300, 200});
    window.render();
    Menu menu;
    int asked = 0;
    menu.onAboutToShow = [&menu, &asked] {
        ++asked;
        menu.clear();
        menu.addItem("Filled", [] {});
    };
    menu.popupAt(&window, {10, 10});
    EXPECT_EQ(asked, 1);
    ASSERT_EQ(host.open.size(), 1u);
    // The row it added counts in the size.
    EXPECT_EQ(host.open[0]->size().height, 26 + 2 * 5);
    menu.close();
    runLoop(10ms);
}

TEST(EmbeddedHostTest, OpenMenusTakeChangedRows)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setScreenSize({1280, 800});
    window.setSize({300, 200});
    window.setKeyboardFocus(true);
    window.render();
    std::string chosen;
    Menu menu;
    menu.addItem("One", [&chosen] { chosen = "one"; });
    Menu* more = menu.addSubmenu("More");
    more->addItem("Deep", [] {});
    menu.popup(&window, {10, 10});
    EmbeddedWindow* popup = host.shown.at(0);
    popup->render();
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Right);
    ASSERT_TRUE(more->isOpen());

    // Nothing changed: nothing happens.
    menu.itemsChanged();
    runLoop(10ms);
    EXPECT_TRUE(host.repositioned.empty());

    menu.addItem("Two", [&chosen] { chosen = "two"; });
    menu.setItemText(0, "First");
    menu.itemsChanged();
    runLoop(10ms);
    ASSERT_EQ(host.repositioned.size(), 1u);
    EXPECT_EQ(popup->size().height, 3 * 26 + 2 * 5);
    // The submenu's row is still there: kept, and it has the keys.
    EXPECT_TRUE(more->isOpen());
    type(window, XKB_KEY_Left);
    EXPECT_FALSE(more->isOpen());
    type(window, XKB_KEY_Down);
    type(window, XKB_KEY_Return, "\r");
    EXPECT_EQ(chosen, "two");
    runLoop(10ms);
}

TEST(EmbeddedHostTest, BusyIndicatorTurnsWhileShown)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    BusyIndicator* busy = content->addChild<BusyIndicator>();
    busy->setGeometry({0, 0, 18, 18});
    window.setContentView(std::move(content));
    window.setScale(1.5);
    window.setSize({18, 18});
    const EmbeddedWindow::Frame& first = window.render(1000000000);
    ASSERT_NE(first.pixels, nullptr);
    const std::vector<uint8_t> before(first.pixels, first.pixels + first.stride * size_t(first.height));
    EXPECT_TRUE(window.needsRender());
    window.render(1000000000 + 250000000);
    const EmbeddedWindow::Frame& later = window.render(1000000000 + 250000000 + 16000000);
    // Anchored at its first tick: 16 ms of a 1000 ms turn.
    EXPECT_NEAR(busy->angle(), 360 * 0.016, 1);
    window.render(1000000000 + 500000000);
    EXPECT_NEAR(busy->angle(), 360 * 0.25, 1);
    EXPECT_NE(std::vector<uint8_t>(later.pixels, later.pixels + later.stride * size_t(later.height)), before);
    // Stopped, it asks for no frames.
    busy->setRunning(false);
    window.render(2000000000);
    EXPECT_FALSE(window.needsRender());
}

TEST(EmbeddedHostTest, StyledSpansUnderlineAndLink)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Label* label = content->addChild<Label>();
    label->setFontSize(14);
    label->setSpans({{"Read ", 0, std::nullopt},
                     {"the docs", 0, Color::rgb(0x3385ff), false, false, "https://example.org"},
                     {" now", 0, std::nullopt, true}});
    label->setGeometry({0, 0, 300, 40});
    window.setContentView(std::move(content));
    window.setSize({300, 40});
    const EmbeddedWindow::Frame& frame = window.render();
    window.updateLayout();
    const std::optional<Label::LineGeometry> line = label->lineGeometry(0);
    ASSERT_TRUE(line);
    EXPECT_EQ(label->lineCount(), 1u);
    EXPECT_GT(line->width, 40);
    // The link is under its own words only.
    EXPECT_EQ(label->linkAt({2, line->top + line->height / 2}), "");
    std::string found;
    double linkX = -1;
    for (double x = 0; x < line->width && found.empty(); x += 1) {
        found = label->linkAt({x, line->top + line->height / 2});
        linkX = x;
    }
    EXPECT_EQ(found, "https://example.org");
    EXPECT_EQ(label->linkAt({linkX, line->top + line->height + 5}), "");
    // An underline in the link's colour below the baseline, where the face
    // puts it.
    int blue = 0;
    for (int y = int(line->baseline) + 1; y < frame.height; ++y) {
        for (int x = int(linkX); x < int(linkX) + 20; ++x) {
            const uint8_t* pixel = frame.pixels + size_t(y) * frame.stride + size_t(x) * 4;
            blue += pixel[2] > 200 && pixel[0] < 120;
        }
    }
    EXPECT_GT(blue, 10);

    // Wrapped: two lines, each with its natural width.
    label->setSpans({});
    label->setText("A rather long application name");
    label->setWrap(true);
    label->setGeometry({0, 0, 120, 40});
    window.render();
    EXPECT_EQ(label->lineCount(), 2u);
    ASSERT_TRUE(label->lineGeometry(1));
    EXPECT_GT(label->lineGeometry(1)->top, label->lineGeometry(0)->top);
    EXPECT_LE(label->lineGeometry(0)->width, 120);
    EXPECT_FALSE(label->lineGeometry(2));
}

TEST(EmbeddedHostTest, BlurredCopyShowsASoftShadow)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    BlurredCopy* shadow = content->addChild<BlurredCopy>(6, 0.5);
    shadow->setGeometry({20, 20, 40, 32});
    Free* inner = shadow->setContent<Free>();
    class Block : public View
    {
    protected:
        void paint(Painter& painter) override { painter.fillRect(localRect(), Color{0, 0, 0, 255}); }
    };
    inner->addChild<Block>()->setGeometry({16, 16, 8, 8});
    shadow->setOpacity(0.25);
    window.setContentView(std::move(content));
    window.setSize({100, 80});
    const EmbeddedWindow::Frame& frame = window.render();
    const auto alpha = [&frame](int x, int y) { return frame.pixels[size_t(y) * frame.stride + size_t(x) * 4 + 3]; };
    // Soft: weaker in the middle than the block, reaching past it.
    EXPECT_GT(alpha(40, 40), 0);
    EXPECT_LT(alpha(40, 40), 64);
    EXPECT_GT(alpha(46, 40), 0);
    EXPECT_EQ(alpha(5, 5), 0);
}

// A shadow's text lays out as the window's text does, and follows the
// text: changed, it is the shadow a label of that text gets.
TEST(EmbeddedHostTest, BlurredCopyTextFollowsTheWindow)
{
    struct Shadowed
    {
        explicit Shadowed(const std::string& text)
        {
            auto content = std::make_unique<Free>();
            visible = content->addChild<Label>(text);
            visible->setGeometry({10, 4, 200, 20});
            shadow = content->addChild<BlurredCopy>(6, 0.5);
            shadow->setGeometry({10, 4, 200, 20});
            Free* inner = shadow->setContent<Free>();
            copy = inner->addChild<Label>(text);
            copy->setColor(Color::rgb(0x000000));
            copy->setGeometry({0, 0, 200, 20});
            window.setContentView(std::move(content));
            window.setScale(1.5);
            window.setSize({220, 28});
        }

        EmbeddedWindow window;
        Label* visible = nullptr;
        BlurredCopy* shadow = nullptr;
        Label* copy = nullptr;
    };
    Shadowed changed("Fri Oct 16 9:43 PM");
    changed.window.render();
    EXPECT_EQ(changed.copy->sizeHint().width, changed.visible->sizeHint().width);

    changed.visible->setText("Fri Oct 16 9:48 PM");
    changed.copy->setText("Fri Oct 16 9:48 PM");
    changed.shadow->contentChanged();
    const EmbeddedWindow::Frame& after = changed.window.render();
    const std::vector<uint8_t> afterPixels(after.pixels, after.pixels + size_t(after.height) * after.stride);

    Shadowed fresh("Fri Oct 16 9:48 PM");
    const EmbeddedWindow::Frame& made = fresh.window.render();
    ASSERT_EQ(made.height, after.height);
    ASSERT_EQ(made.stride, after.stride);
    EXPECT_TRUE(std::equal(afterPixels.begin(), afterPixels.end(), made.pixels));
}

TEST(EmbeddedHostTest, ThemeIconsPreloadOnThePool)
{
    if (themeIconPath("utilities-terminal", 105, 1.5).empty())
        GTEST_SKIP() << "no utilities-terminal in the theme";
    bool done = false;
    preloadThemeIcons({"utilities-terminal", "folder", "no-such-icon-at-all"}, 105, 1.5, [&done] { done = true; });
    for (int i = 0; i < 400 && !done; ++i)
        runLoop(5ms);
    EXPECT_TRUE(done);
    const Bitmap bitmap = themeIconBitmap("utilities-terminal", 105, 1.5);
    ASSERT_FALSE(bitmap.isNull());
    EXPECT_EQ(bitmap.width(), 158);
    EXPECT_EQ(bitmap.format(), Bitmap::Format::Rgba8888Premultiplied);
    EXPECT_GT(bitmap.alpha(79, 79), 0);
    EXPECT_TRUE(themeIconBitmap("no-such-icon-at-all", 105, 1.5).isNull());
}

TEST(EmbeddedHostTest, LabelWrapsAnywhereWithFixedLineHeight)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Label* label = content->addChild<Label>("--ready-fd 10 --shell-control-socket");
    label->setWrap(true);
    label->setGeometry({0, 0, 90, 200});
    window.setContentView(std::move(content));
    window.setSize({100, 200});
    window.render();
    const size_t words = label->lineCount();
    label->setWrapAnywhere(true);
    window.render();
    const size_t anywhere = label->lineCount();
    EXPECT_LE(anywhere, words);
    // Every full line fills the width, mid-word.
    ASSERT_TRUE(label->lineGeometry(0));
    EXPECT_GT(label->lineGeometry(0)->width, 80);

    label->setFixedLineHeight(20);
    window.render();
    ASSERT_TRUE(label->lineGeometry(1));
    EXPECT_EQ(label->lineGeometry(1)->top - label->lineGeometry(0)->top, 20);
    EXPECT_EQ(label->lineGeometry(0)->height, 20);
    EXPECT_EQ(label->heightForWidth(90), 20.0 * double(anywhere));
}

TEST(EmbeddedHostTest, TextFieldsCarryInputHints)
{
    Host host;
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    TextField* field = content->addChild<TextField>();
    field->setGeometry({0, 0, 200, 35});
    window.setContentView(std::move(content));
    window.setSize({200, 40});
    window.setKeyboardFocus(true);
    field->setFocus();
    field->setInputPurpose(TextPurpose::Digits);
    field->setInputHints(SensitiveDataHint);
    ASSERT_TRUE(host.textInput.back());
    EXPECT_EQ(host.textInput.back()->purpose, TextPurpose::Digits);
    EXPECT_EQ(host.textInput.back()->hints, uint32_t(SensitiveDataHint));
}

// fontconfig's rgba=rgb, as Cutefish's own fontconfig file sets it on the
// test machine: text over a translucent window gets the per-channel
// subpixel blend, coloured fringes and the average coverage as alpha.
TEST(EmbeddedHostTest, SubpixelTextOverTranslucentWindows)
{    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Label* label = content->addChild<Label>("Open Unpin");
    label->setColor(Color::rgb(0xffffff));
    label->setGeometry({0, 0, 120, 24});
    window.setContentView(std::move(content));
    window.setScale(1.5);
    window.setSize({120, 24});
    const EmbeddedWindow::Frame& frame = window.render();
    int fringes = 0;
    int glyph = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const uint8_t* p = frame.pixels + size_t(y) * frame.stride + size_t(x) * 4;
            if (p[3] == 0)
                continue;
            ++glyph;
            if (std::abs(int(p[0]) - int(p[2])) > 40)
                ++fringes;
            // The alpha is the average coverage, white text's rgb the coverage.
            EXPECT_NEAR(p[3], (int(p[0]) + int(p[1]) + int(p[2])) / 3.0, 2) << x << "," << y;
        }
    }
    EXPECT_GT(glyph, 100);
    EXPECT_GT(fringes, 20);
}

// Text with a style (a shadow) and text marked NoSubpixelAntialias stay
// greyscale, as on the status bar.
TEST(EmbeddedHostTest, StyledAndMarkedTextStaysGreyscale)
{
    const auto fringes = [](View* label) {
        EmbeddedWindow window;
        auto content = std::make_unique<Free>();
        content->addChild(std::unique_ptr<View>(label));
        label->setGeometry({0, 0, 120, 24});
        window.setContentView(std::move(content));
        window.setScale(1.5);
        window.setSize({120, 24});
        const EmbeddedWindow::Frame& frame = window.render();
        int count = 0;
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const uint8_t* p = frame.pixels + size_t(y) * frame.stride + size_t(x) * 4;
                count += p[3] > 0 && std::abs(int(p[0]) - int(p[2])) > 40;
            }
        }
        return count;
    };
    auto* plain = new Label("Open Unpin");
    plain->setColor(Color::rgb(0xffffff));
    EXPECT_GT(fringes(plain), 20);
    auto* marked = new Label("Open Unpin");
    marked->setColor(Color::rgb(0xffffff));
    marked->setSubpixelAntialiasing(false);
    EXPECT_EQ(fringes(marked), 0);
    auto* styled = new StyledLabel("Open Unpin");
    styled->setColor(Color::rgb(0xffffff));
    styled->setStyle(StyledLabel::Style::Raised, Color{0, 0, 0, 77});
    EXPECT_EQ(fringes(styled), 0);
}

// Absolute letter spacing: every glyph's advance, the last too.
TEST(EmbeddedHostTest, LetterSpacingWidensEveryGlyph)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Label* label = content->addChild<Label>("123456");
    label->setFontSize(28);
    label->setGeometry({0, 0, 300, 40});
    window.setContentView(std::move(content));
    window.setSize({300, 40});
    window.render();
    const double plain = label->sizeHint().width;
    label->setLetterSpacing(4);
    window.render();
    EXPECT_NEAR(label->sizeHint().width, plain + 6 * 4, 1e-6);
    ASSERT_TRUE(label->lineGeometry(0));
    EXPECT_NEAR(label->lineGeometry(0)->width - plain, 24, 1);

    // The last glyph is drawn whole, not clipped at the unspaced width.
    label->setColor(Color::rgb(0x000000));
    label->setGeometry({0, 0, 300, 40});
    const EmbeddedWindow::Frame& frame = window.render();
    int right = -1;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (frame.pixels[size_t(y) * frame.stride + size_t(x) * 4 + 3] > 0)
                right = std::max(right, x);
        }
    }
    // The last glyph starts 20 px further than unspaced; clipped at the
    // unspaced width its ink would stop there.
    EXPECT_GT(right, int(plain) + 4 * 4);
}

// Embedded windows lay text out unrounded at scale 1; a window can ask for
// whole pixels.
TEST(EmbeddedHostTest, HintedTextLayoutPerWindow)
{
    EmbeddedWindow window;
    auto content = std::make_unique<Free>();
    Label* label = content->addChild<Label>("tester");
    label->setFontSize(13);
    label->setGeometry({0, 0, 200, 20});
    window.setContentView(std::move(content));
    window.setSize({200, 20});
    window.render();
    EXPECT_FALSE(window.hintedTextLayout());
    EXPECT_NE(label->sizeHint().width, std::round(label->sizeHint().width));
    window.setHintedTextLayout(true);
    window.render();
    const double hinted = label->sizeHint().width;
    EXPECT_EQ(hinted, std::round(hinted));
}

// Popups draw text as their window.
TEST(EmbeddedHostTest, PopupsTakeTheirWindowsTextLayout)
{
    Host host;
    EmbeddedWindow window;
    window.setContentView(std::make_unique<Free>());
    window.setSize({300, 200});
    window.render();
    Menu menu;
    menu.addItem("One", [] {});
    menu.popup(&window, {10, 10});
    ASSERT_EQ(host.shown.size(), 1u);
    EXPECT_FALSE(host.shown[0]->hintedTextLayout());
    menu.close();
    runLoop(10ms);
}
