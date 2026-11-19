// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/event.h"
#include "ui/geometry.h"

namespace cutefish::ui {

class View;
class Window;

// Works a shown headless window as its user would, by the names of its views
// (View::name(), "<area>:<thing>"): clicks where a view shows, types, and
// reads what the views show, through TestInput. Every action lays the window
// out before and after, as frames do between a user's inputs.
//
// A call that needs a view or a menu item and cannot find or reach it hands
// a message naming it to the failure handler and returns a neutral value:
// null, false, an empty string or rect. Questions about state (shows(),
// findAll(), waitFor()) only answer.
class TestDriver
{
public:
    using FailureHandler = std::function<void(const std::string& message)>;

    TestDriver(Window& window, FailureHandler onFailure);

    Window& window() const { return m_window; }

    // As a frame does, the animations ticked, without painting.
    void layout();

    // The one view of the name in the window's content and overlay.
    template<typename T = View>
    T* find(const std::string& name)
    {
        View* view = findOne(name);
        T* typed = dynamic_cast<T*>(view);
        if (view && !typed)
            fail("\"" + name + "\" is not a view of the type asked for");
        return typed;
    }
    // Every view of the name, hidden ones included, in tree order.
    template<typename T = View>
    std::vector<T*> findAll(const std::string& name) const
    {
        std::vector<T*> typed;
        for (View* view : named(name)) {
            if (T* match = dynamic_cast<T*>(view))
                typed.push_back(match);
        }
        return typed;
    }

    // The view is there, visible, and so is every view above it.
    bool shows(const std::string& name) const;
    // In window coordinates.
    RectF frame(const std::string& name);
    // A label's, text field's, text area's or check box's text; for another
    // view, the text of the one such view inside it, as a button's label.
    std::string text(const std::string& name);
    bool hasFocus(const std::string& name);
    // How many views of the name show, as shows() asks of one.
    size_t countShown(const std::string& name) const;
    // The texts of the shown labels, text fields, text areas and check boxes
    // inside the view, or its own, in tree order; none while it is hidden.
    std::vector<std::string> texts(const std::string& name);
    // A shown label, text field, text area or check box of the window reads
    // the text.
    bool showsText(const std::string& text) const;

    // A click at the middle of what shows of the one view of the name that
    // shows, hidden views of the name left out, which must take the pointer
    // there itself or through a view inside or around it.
    bool click(const std::string& name, MouseButton button = MouseButton::Left);
    bool focus(const std::string& name);
    // A wheel or touchpad scroll over the middle of what shows of the view,
    // reached as for a click.
    bool scroll(const std::string& name, const ScrollEvent& event);
    // Another client's drag of the data, by MIME type, over the middle of
    // what shows of the view, reached as for a click: it rests there until
    // the loop has answered what the window read of the data on its entry,
    // then is let go. The dropped data is read on the next pass of the loop;
    // something under the pointer must take the drop.
    bool drop(const std::string& name, const std::vector<std::pair<std::string, std::string>>& data);
    // A click on the middle of a segment of the segmented control, the view
    // named or the one inside it, reached as for a click.
    bool chooseSegment(const std::string& name, size_t index);
    // A press on the slider's track at one fraction of the handle's travel,
    // a move to another and the release there; the slider is the view named
    // or the one inside it, reached as for a click.
    bool dragSlider(const std::string& name, double from, double to);
    // A click on the row with the text of the menu or drop-down open over
    // the window, which must show it; a disabled row does nothing.
    bool choosePopupItem(const std::string& item);
    // The newest shown window kept over this one, such as a dialog; null
    // while there is none. A driver of its own works it.
    Window* dialog() const;
    // A left click at the middle of the one shown label of the window that
    // reads the text, for a button whose view has no name, such as an
    // alert's, reached as for a click.
    bool clickText(const std::string& text);

    // The window's keyboard focus, and the compositor activating it.
    void setKeyboardFocus(bool focused);
    void setActive(bool active);
    void pressKey(const KeyEvent& event);
    void releaseKey(const KeyEvent& event);
    // A press and a release.
    void typeKey(uint32_t key, uint32_t modifiers = NoModifier, const std::string& text = {});
    // Each character as a key that produces it.
    void typeText(const std::string& text);

    // As the menu bar chooses an item of a menu of the window's main menu,
    // both by their text: onAboutToShow runs first, and the item must be
    // shown and enabled.
    bool chooseMenuItem(const std::string& menu, const std::string& item);

    // Runs the thread's loop until the condition holds or the time is up.
    static bool waitFor(const std::function<bool()>& condition,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

private:
    View* findOne(const std::string& name);
    std::vector<View*> named(const std::string& name) const;
    // The one shown view of the name and where a click on it lands, or null
    // after a failure.
    View* reach(const std::string& name, PointF& point);
    void fail(const std::string& message) const;

    Window& m_window;
    FailureHandler m_onFailure;
};

} // namespace cutefish::ui
