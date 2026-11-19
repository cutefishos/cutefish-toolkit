// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/test_driver.h"

#include <algorithm>
#include <ctime>

#include "base/event_loop.h"
#include "base/timer.h"
#include "ui/application.h"
#include "ui/label.h"
#include "ui/menu.h"
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

bool TestDriver::hasFocus(const std::string& name)
{
    const View* view = findOne(name);
    return view && view->hasFocus();
}

bool TestDriver::click(const std::string& name)
{
    layout();
    View* view = findOne(name);
    if (!view)
        return false;
    if (!isVisibleInWindow(view)) {
        fail("\"" + name + "\" is hidden");
        return false;
    }
    RectF visible = intersected(windowRect(view), {0, 0, m_window.size().width, m_window.size().height});
    for (const View* ancestor = view->parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->clipsChildren())
            visible = intersected(visible, windowRect(ancestor));
    }
    if (visible.width <= 0 || visible.height <= 0) {
        fail("\"" + name + "\" shows nowhere in the window");
        return false;
    }
    const PointF center{visible.x + visible.width / 2, visible.y + visible.height / 2};
    View* hit = m_window.viewAt(center, true);
    if (hit && !isWithin(hit, view) && !isWithin(view, hit)) {
        fail("\"" + name + "\" is covered by \"" + hit->name() + "\" at its middle");
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
