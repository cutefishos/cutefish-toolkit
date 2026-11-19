// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/test_driver.h"

#include <algorithm>
#include <ctime>

#include "base/event_loop.h"
#include "base/timer.h"
#include "ui/application.h"
#include "ui/drag.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/segmented_control.h"
#include "ui/slider.h"
#include "ui/test_input.h"
#include "ui/text_area.h"
#include "ui/text_field.h"
#include "ui/toggle_control.h"
#include "ui/view.h"
#include "ui/window.h"

namespace cutefish::ui {

static uint64_t monotonicNow()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
}

static void collect(View* view, const std::string& name, std::vector<View*>& found)
{
    if (view->name() == name)
        found.push_back(view);
    for (const auto& child : view->children())
        collect(child.get(), name, found);
}

static RectF windowRect(const View* view)
{
    const PointF topLeft = view->mapToWindow({});
    return {topLeft.x, topLeft.y, view->geometry().width, view->geometry().height};
}

static RectF intersected(const RectF& a, const RectF& b)
{
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.right(), b.right());
    const double bottom = std::min(a.bottom(), b.bottom());
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

static const std::string* ownText(const View* view)
{
    if (const auto* label = dynamic_cast<const Label*>(view))
        return &label->text();
    if (const auto* field = dynamic_cast<const TextField*>(view))
        return &field->text();
    if (const auto* area = dynamic_cast<const TextArea*>(view))
        return &area->text();
    if (const auto* toggle = dynamic_cast<const ToggleControl*>(view))
        return &toggle->text();
    return nullptr;
}

static void collectTexts(const View* view, std::vector<const std::string*>& texts)
{
    for (const auto& child : view->children()) {
        if (const std::string* text = ownText(child.get()))
            texts.push_back(text);
        else
            collectTexts(child.get(), texts);
    }
}

static bool isVisibleInWindow(const View* view)
{
    for (; view; view = view->parent()) {
        if (!view->isVisible())
            return false;
    }
    return true;
}

static bool isWithin(const View* view, const View* ancestor)
{
    for (; view; view = view->parent()) {
        if (view == ancestor)
            return true;
    }
    return false;
}

static void collectShownTexts(const View* view, std::vector<std::string>& texts)
{
    for (const auto& child : view->children()) {
        if (!child->isVisible())
            continue;
        if (const std::string* text = ownText(child.get()))
            texts.push_back(*text);
        else
            collectShownTexts(child.get(), texts);
    }
}

static bool holdsShownText(const View* view, const std::string& text)
{
    if (!view->isVisible())
        return false;
    if (const std::string* own = ownText(view))
        return *own == text;
    for (const auto& child : view->children()) {
        if (holdsShownText(child.get(), text))
            return true;
    }
    return false;
}

// The view itself when it is a T, else the one T inside it.
template<typename T>
static T* oneInside(View* view)
{
    if (auto* typed = dynamic_cast<T*>(view))
        return typed;
    T* found = nullptr;
    for (const auto& child : view->children()) {
        if (T* inner = oneInside<T>(child.get())) {
            if (found)
                return nullptr;
            found = inner;
        }
    }
    return found;
}

// What shows of the view in its window, its clipping ancestors cut off.
static RectF shownRect(const View* view)
{
    const Window* window = view->window();
    RectF visible = windowRect(view);
    if (window)
        visible = intersected(visible, {0, 0, window->size().width, window->size().height});
    for (const View* ancestor = view->parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->clipsChildren())
            visible = intersected(visible, windowRect(ancestor));
    }
    return visible;
}

static Label* labelWithText(View* view, const std::string& text)
{
    if (!view->isVisible())
        return nullptr;
    if (auto* label = dynamic_cast<Label*>(view); label && label->text() == text)
        return label;
    for (const auto& child : view->children()) {
        if (Label* found = labelWithText(child.get(), text))
            return found;
    }
    return nullptr;
}

TestDriver::TestDriver(Window& window, FailureHandler onFailure)
    : m_window(window)
    , m_onFailure(std::move(onFailure))
{
}

void TestDriver::layout()
{
    m_window.prepareFrame(monotonicNow());
}

View* TestDriver::findOne(const std::string& name)
{
    const std::vector<View*> found = named(name);
    if (found.empty()) {
        fail("no view named \"" + name + "\"");
        return nullptr;
    }
    if (found.size() > 1) {
        fail(std::to_string(found.size()) + " views named \"" + name + "\"");
        return nullptr;
    }
    return found.front();
}

std::vector<View*> TestDriver::named(const std::string& name) const
{
    std::vector<View*> found;
    if (View* content = m_window.contentView())
        collect(content, name, found);
    if (m_window.m_overlay)
        collect(m_window.m_overlay.get(), name, found);
    return found;
}

void TestDriver::fail(const std::string& message) const
{
    if (m_onFailure)
        m_onFailure(message);
}

bool TestDriver::shows(const std::string& name) const
{
    const std::vector<View*> found = named(name);
    return found.size() == 1 && isVisibleInWindow(found.front());
}

RectF TestDriver::frame(const std::string& name)
{
    layout();
    const View* view = findOne(name);
    return view ? windowRect(view) : RectF{};
}

std::string TestDriver::text(const std::string& name)
{
    const View* view = findOne(name);
    if (!view)
        return {};
    if (const std::string* text = ownText(view))
        return *text;
    std::vector<const std::string*> texts;
    collectTexts(view, texts);
    if (texts.size() != 1) {
        fail("\"" + name + "\" holds " + std::to_string(texts.size()) + " views with text, not one");
        return {};
    }
    return *texts.front();
}

size_t TestDriver::countShown(const std::string& name) const
{
    size_t count = 0;
    for (const View* view : named(name)) {
        if (isVisibleInWindow(view))
            ++count;
    }
    return count;
}

std::vector<std::string> TestDriver::texts(const std::string& name)
{
    const View* view = findOne(name);
    if (!view || !isVisibleInWindow(view))
        return {};
    if (const std::string* text = ownText(view))
        return {*text};
    std::vector<std::string> texts;
    collectShownTexts(view, texts);
    return texts;
}

bool TestDriver::showsText(const std::string& text) const
{
    if (View* content = m_window.contentView(); content && holdsShownText(content, text))
        return true;
    return m_window.m_overlay && holdsShownText(m_window.m_overlay.get(), text);
}

bool TestDriver::hasFocus(const std::string& name)
{
    const View* view = findOne(name);
    return view && view->hasFocus();
}

View* TestDriver::reach(const std::string& name, PointF& point)
{
    layout();
    const std::vector<View*> found = named(name);
    if (found.empty()) {
        fail("no view named \"" + name + "\"");
        return nullptr;
    }
    std::vector<View*> shown;
    for (View* view : found) {
        if (isVisibleInWindow(view))
            shown.push_back(view);
    }
    if (shown.empty()) {
        fail("\"" + name + "\" is hidden");
        return nullptr;
    }
    if (shown.size() > 1) {
        fail(std::to_string(shown.size()) + " shown views named \"" + name + "\"");
        return nullptr;
    }
    View* view = shown.front();
    const RectF visible = shownRect(view);
    if (visible.width <= 0 || visible.height <= 0) {
        fail("\"" + name + "\" shows nowhere in the window");
        return nullptr;
    }
    const PointF center{visible.x + visible.width / 2, visible.y + visible.height / 2};
    View* hit = m_window.viewAt(center, true);
    if (hit && !isWithin(hit, view) && !isWithin(view, hit)) {
        fail("\"" + name + "\" is covered by \"" + hit->name() + "\" at its middle");
        return nullptr;
    }
    point = center;
    return view;
}

bool TestDriver::click(const std::string& name, MouseButton button)
{
    PointF point;
    if (!reach(name, point))
        return false;
    TestInput::click(m_window, point, button);
    layout();
    return true;
}

bool TestDriver::scroll(const std::string& name, const ScrollEvent& event)
{
    PointF point;
    if (!reach(name, point))
        return false;
    TestInput::scroll(m_window, point, event);
    layout();
    return true;
}

namespace {

// Hands each read its data later on the loop, as a pipe from the source does.
class DataOffer final : public DropOffer
{
public:
    explicit DataOffer(const std::vector<std::pair<std::string, std::string>>& data)
        : m_data(data)
    {
        for (const auto& [mimeType, bytes] : m_data)
            m_mimeTypes.push_back(mimeType);
    }

    const std::vector<std::string>& mimeTypes() const override { return m_mimeTypes; }
    uint32_t sourceActions() const override { return CopyAction | MoveAction; }

    void read(const std::string& mimeType, std::function<void(std::string data)> done) override
    {
        std::string bytes;
        for (const auto& [type, data] : m_data) {
            if (type == mimeType)
                bytes = data;
        }
        Application::instance()->loop().post([done = std::move(done), bytes = std::move(bytes)] { done(bytes); });
    }

private:
    std::vector<std::pair<std::string, std::string>> m_data;
    std::vector<std::string> m_mimeTypes;
};

} // namespace

bool TestDriver::drop(const std::string& name, const std::vector<std::pair<std::string, std::string>>& data)
{
    PointF point;
    if (!reach(name, point))
        return false;
    DataOffer offer(data);
    TestInput::dragEnter(m_window, point, &offer);
    base::EventLoop& loop = Application::instance()->loop();
    loop.post([&loop] { loop.quit(); });
    loop.run();
    TestInput::dragMove(m_window, point);
    const uint32_t action = TestInput::drop(m_window);
    layout();
    if (action == NoAction) {
        fail("nothing at the middle of \"" + name + "\" takes the drop");
        return false;
    }
    return true;
}

bool TestDriver::chooseSegment(const std::string& name, size_t index)
{
    PointF point;
    View* view = reach(name, point);
    if (!view)
        return false;
    auto* control = oneInside<SegmentedControl>(view);
    if (!control) {
        fail("\"" + name + "\" holds no one segmented control");
        return false;
    }
    if (index >= control->count()) {
        fail("\"" + name + "\" has no segment " + std::to_string(index));
        return false;
    }
    // The segments share what the padding leaves, as SegmentedControl lays them out.
    const RectF rect = windowRect(control);
    const double padding = control->padding();
    const double width = (rect.width - 2 * padding) / double(control->count());
    TestInput::click(m_window, {rect.x + padding + width * (double(index) + 0.5), rect.y + rect.height / 2});
    layout();
    return true;
}

bool TestDriver::dragSlider(const std::string& name, double from, double to)
{
    PointF point;
    View* view = reach(name, point);
    if (!view)
        return false;
    auto* slider = oneInside<Slider>(view);
    if (!slider) {
        fail("\"" + name + "\" holds no one slider");
        return false;
    }
    // The handle's centre travels the track.
    const RectF handle = slider->handleRect();
    const RectF rect = windowRect(slider);
    const double travel = std::max(0.0, rect.width - handle.width);
    const auto at = [&](double fraction) {
        return PointF{rect.x + handle.width / 2 + travel * fraction, rect.y + handle.y + handle.height / 2};
    };
    TestInput::press(m_window, at(from));
    layout();
    TestInput::movePointer(m_window, at(to));
    layout();
    TestInput::release(m_window, at(to));
    layout();
    return true;
}

bool TestDriver::choosePopupItem(const std::string& item)
{
    layout();
    Window* popup = nullptr;
    const std::vector<Window*>& windows = Application::instance()->windows();
    for (auto it = windows.rbegin(); it != windows.rend() && !popup; ++it) {
        if ((*it)->isPopup() && (*it)->parentWindow() == &m_window && !(*it)->isHidden())
            popup = *it;
    }
    if (!popup || !popup->contentView()) {
        fail("no menu open over the window for \"" + item + "\"");
        return false;
    }
    popup->prepareFrame(monotonicNow());
    Label* label = labelWithText(popup->contentView(), item);
    if (!label) {
        fail("no row \"" + item + "\" in the open menu");
        return false;
    }
    const RectF visible = shownRect(label);
    if (visible.width <= 0 || visible.height <= 0) {
        fail("the row \"" + item + "\" shows nowhere in the open menu");
        return false;
    }
    // The menu may go with the click.
    TestInput::click(*popup, {visible.x + visible.width / 2, visible.y + visible.height / 2});
    layout();
    return true;
}

Window* TestDriver::dialog() const
{
    const std::vector<Window*>& windows = Application::instance()->windows();
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        if (*it != &m_window && (*it)->transientParent() == &m_window && !(*it)->isHidden())
            return *it;
    }
    return nullptr;
}

static void collectLabels(View* view, const std::string& text, std::vector<Label*>& found)
{
    if (!view->isVisible())
        return;
    if (auto* label = dynamic_cast<Label*>(view); label && label->text() == text)
        found.push_back(label);
    for (const auto& child : view->children())
        collectLabels(child.get(), text, found);
}

bool TestDriver::clickText(const std::string& text)
{
    layout();
    std::vector<Label*> found;
    if (View* content = m_window.contentView())
        collectLabels(content, text, found);
    if (m_window.m_overlay)
        collectLabels(m_window.m_overlay.get(), text, found);
    if (found.size() != 1) {
        fail(std::to_string(found.size()) + " labels show \"" + text + "\", not one");
        return false;
    }
    const RectF visible = shownRect(found.front());
    if (visible.width <= 0 || visible.height <= 0) {
        fail("\"" + text + "\" shows nowhere in the window");
        return false;
    }
    const PointF center{visible.x + visible.width / 2, visible.y + visible.height / 2};
    View* hit = m_window.viewAt(center, true);
    if (hit && !isWithin(hit, found.front()) && !isWithin(found.front(), hit)) {
        fail("\"" + text + "\" is covered by \"" + hit->name() + "\" at its middle");
        return false;
    }
    TestInput::click(m_window, center);
    layout();
    return true;
}

bool TestDriver::focus(const std::string& name)
{
    layout();
    View* view = findOne(name);
    if (!view)
        return false;
    if (!view->acceptsFocus() || !isVisibleInWindow(view)) {
        fail("\"" + name + "\" does not take the focus");
        return false;
    }
    view->setFocus();
    layout();
    return true;
}

void TestDriver::setKeyboardFocus(bool focused)
{
    layout();
    TestInput::setKeyboardFocus(m_window, focused);
    layout();
}

void TestDriver::setActive(bool active)
{
    layout();
    WindowStates states = m_window.states();
    states.activated = active;
    TestInput::configure(m_window, int(m_window.size().width), int(m_window.size().height), states);
    layout();
}

void TestDriver::pressKey(const KeyEvent& event)
{
    layout();
    TestInput::pressKey(m_window, event);
    layout();
}

void TestDriver::releaseKey(const KeyEvent& event)
{
    layout();
    TestInput::releaseKey(m_window, event);
    layout();
}

void TestDriver::typeKey(uint32_t key, uint32_t modifiers, const std::string& text)
{
    layout();
    TestInput::typeKey(m_window, key, modifiers, text);
    layout();
}

void TestDriver::typeText(const std::string& text)
{
    layout();
    TestInput::typeText(m_window, text);
    layout();
}

bool TestDriver::chooseMenuItem(const std::string& menu, const std::string& item)
{
    layout();
    Menu* mainMenu = m_window.mainMenu();
    if (!mainMenu) {
        fail("no main menu for \"" + menu + "\"");
        return false;
    }
    Menu* submenu = nullptr;
    for (const Menu::Item& entry : mainMenu->m_items) {
        if (entry.submenu && entry.visible && entry.text == menu)
            submenu = entry.submenu.get();
    }
    if (!submenu) {
        fail("no menu \"" + menu + "\" in the main menu");
        return false;
    }
    if (submenu->onAboutToShow) {
        const std::function<void()> handler = submenu->onAboutToShow;
        handler();
    }
    const Menu::Item* chosen = nullptr;
    for (const Menu::Item& entry : submenu->m_items) {
        if (!entry.separator && entry.text == item)
            chosen = &entry;
    }
    if (!chosen || !chosen->visible) {
        fail("no item \"" + item + "\" in the menu \"" + menu + "\"");
        return false;
    }
    if (!chosen->enabled || !chosen->action) {
        fail("\"" + menu + "\" > \"" + item + "\" is disabled");
        return false;
    }
    // The action may change or destroy the menu.
    const std::function<void()> action = chosen->action;
    action();
    layout();
    return true;
}

bool TestDriver::waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    if (condition())
        return true;
    base::EventLoop& loop = Application::instance()->loop();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    base::Timer timer(&loop);
    timer.onTimeout = [&] {
        if (condition() || std::chrono::steady_clock::now() >= deadline)
            loop.quit();
    };
    timer.start(std::chrono::milliseconds(5));
    loop.run();
    return condition();
}

} // namespace cutefish::ui
