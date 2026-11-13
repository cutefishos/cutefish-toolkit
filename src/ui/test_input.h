// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ui/event.h"
#include "ui/geometry.h"
#include "ui/image.h"
#include "ui/window.h"

namespace cutefish::ui {

class DropOffer;
class View;
class Window;
struct WindowStates;

// Drives a window as the window system would, so tests can work views
// without a compositor. Positions are in window coordinates.
class TestInput
{
public:
    static void movePointer(Window& window, PointF position);
    static void leavePointer(Window& window);
    static void press(Window& window, PointF position, MouseButton button = MouseButton::Left,
                      uint32_t modifiers = NoModifier);
    static void release(Window& window, PointF position, MouseButton button = MouseButton::Left);
    static void click(Window& window, PointF position, MouseButton button = MouseButton::Left);
    static void scroll(Window& window, PointF position, const ScrollEvent& event);
    // Fingers coming to rest on the touchpad.
    static void hold(Window& window);
    // A touchpad swipe, pinch or hold event, where the pointer is.
    static void gesture(Window& window, const GestureEvent& event);
    // A configure from the compositor; a zero size leaves the choice to the window.
    static void configure(Window& window, int width, int height, const WindowStates& states);
    static void configureBounds(Window& window, int width, int height);
    // The user dragging an edge to the size.
    static void resizeInteractively(Window& window, SizeF size);

    static void setKeyboardFocus(Window& window, bool focused);
    static void pressKey(Window& window, const KeyEvent& event);
    static void releaseKey(Window& window, const KeyEvent& event);
    // A press and a release.
    static void typeKey(Window& window, uint32_t key, uint32_t modifiers = NoModifier,
                        const std::string& text = {});
    // Each character as a key that produces it.
    static void typeText(Window& window, const std::string& text);
    // As an input method sends it to the focused view.
    static void inputMethod(Window& window, const TextInputEvent& event);

    // Renders a frame as on screen into a chain of three buffers, repainting
    // only what changed since each buffer's last frame; returns the buffer.
    static Image presentFrame(Window& window, uint64_t frameTimeNs = 0);
    // The device pixels the view's own paint inks at the scale, without its
    // children, relative to its top left.
    static Rect inkRect(View& view, double scale);
    // The tool tip the window shows, or null.
    static View* toolTip(Window& window);
    // How often the window asked the window system to move it.
    static int moveRequests(Window& window);
    // The views watching damage in the window, as a header backdrop does.
    static size_t damageWatches(Window& window);
    // Headless windows report glass as available, or not (the default).
    static void setGlassAvailable(bool available);
    // After every layout pass, every window lays out again in full and
    // reports on standard error each view the pass left elsewhere, as a view
    // that changed what it measures without asking for layout does.
    static void setVerifiesLayout(bool verifies);
    static bool verifiesLayout();
    // The views reported so far.
    static int layoutMismatches();
    // The glass the window asked for, or none.
    static std::optional<GlassMaterial> backgroundGlass(Window& window);

    // A drag from another client entering, moving over, leaving or dropped
    // on the window; each returns the action the window takes.
    static uint32_t dragEnter(Window& window, PointF position, DropOffer* offer, uint32_t modifiers = NoModifier);
    static uint32_t dragMove(Window& window, PointF position, uint32_t modifiers = NoModifier);
    static void dragLeave(Window& window);
    static uint32_t drop(Window& window);
};

} // namespace cutefish::ui
