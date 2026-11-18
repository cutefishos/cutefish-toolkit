// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "ui/drag.h"
#include "ui/event.h"
#include "ui/geometry.h"
#include "ui/path.h"
#include "ui/window.h"

namespace cutefish::ui {

class EmbeddedGpu;
class EmbeddedHost;
class EmbeddedPlatformWindow;

// A window inside a process that owns the screen, such as the compositor,
// on Application::Platform::Embedded. It has no surface of its own: the host
// gives it a size, a scale and the keyboard, feeds it input, renders it when
// it composites and draws the pixels and the cursor itself. Nothing here
// blocks or starts a thread.
class EmbeddedWindow : public Window
{
public:
    // Pixels of the last render, kept by the window until the next render or
    // a change of size or scale.
    struct Frame
    {
        // RGBA8888, premultiplied, row by row. Null before the first
        // render.
        const uint8_t* pixels = nullptr;
        // round(logical size * scale) on each axis.
        int width = 0;
        int height = 0;
        size_t stride = 0;
        // Device pixels that changed since the render before; empty when
        // none did. The whole frame after a change of size or scale.
        std::vector<Rect> damage;
    };

    EmbeddedWindow();
    // A popup of the parent, as Window::createPopup() makes them on Embedded:
    // the host hears of it through EmbeddedHost::popupShown() and places it.
    EmbeddedWindow(Window* parent, const PopupPlacement& placement, bool grab);
    ~EmbeddedWindow() override;

    // The process's host, for popups, activation, text input, blur and
    // drags; null (the default) refuses or ignores them. Set it before
    // the first window.
    static void setHost(EmbeddedHost* host);
    static EmbeddedHost* host();

    // The logical size; fractional sizes are kept as they are. The window
    // shows from the first size on.
    void setSize(SizeF size);
    // The keyboard is the host's to give: keys reach the window either way,
    // but carets blink and text fields show focus only while it has it.
    void setKeyboardFocus(bool focused);
    // isActive(): focus rings and other active looks.
    void setActive(bool active);

    // Popups: the window the popup hangs off, null for others.
    EmbeddedWindow* popupParent() const;
    const PopupPlacement& popupPlacement() const;
    // It holds an xdg_popup grab, see EmbeddedHost::popupShown().
    bool isGrabbingPopup() const;
    // Where the host placed the popup, in the parent's logical coordinates;
    // a size other than size() resizes it, as a positioner allowed to
    // resize does.
    void setPopupGeometry(const RectF& geometry);
    const RectF& popupGeometry() const { return m_popupGeometry; }
    // xdg_popup.popup_done, or a toplevel's close button: the window's
    // onCloseRequested. A menu closes itself.
    void requestClose();
    // Dismisses every popup of the window, innermost first, as a broken
    // grab does.
    void closePopups();

    // The rounded rect the host blurs behind, in logical coordinates, and
    // its corner radius; an empty rect for none.
    RectF blurRect() const;
    double blurRadius() const;
    // The outline masking the blur instead of the rounded rect, in logical
    // coordinates, as cutefish_surface_blur_v1.set_shape; empty for none.
    const Path& blurShape() const;

    // Lays the views out now rather than at the next render, e.g. to read a
    // view's place before the first frame.
    void updateLayout();

    // True when a render would draw something new: a change, an animation
    // running, a pending layout.
    bool needsRender() const;
    // Called once whenever a render becomes wanted, until the host renders;
    // also after a render while animations run. It may come from inside any
    // call into the toolkit, the input calls included.
    std::function<void()> onRenderRequested;

    // Ticks animations at the frame time and lays out, which may resize the
    // window: a host sizes the next render's texture by pixelWidth() and
    // pixelHeight() after it. That render, at the same frame time, only
    // paints.
    void beginFrame(uint64_t frameTimeNs = 0);
    // Lays out, ticks animations at the frame time and repaints what changed,
    // into memory the window owns. The frame time is nanoseconds on
    // CLOCK_MONOTONIC (now when 0); animations step by the times given.
    const Frame& render(uint64_t frameTimeNs = 0);
    const Frame& frame() const { return m_frame; }

    // A GL_TEXTURE_2D of the host's with 8-bit RGBA texels, exactly
    // pixelWidth() by pixelHeight(), on the context the EmbeddedGpu was made on.
    struct GpuTexture
    {
        uint32_t id = 0;
        int width = 0;
        int height = 0;
        // It still holds what the last render into it drew, and nothing
        // rendered elsewhere since: only the damage is repainted.
        bool keepsLastFrame = false;
    };
    // As render(), with Skia on the GPU into the host's texture, its first
    // row the frame's top as an upload of Frame::pixels would put it. The
    // frame's pixels are null. Null when nothing was drawn: the context is
    // not current or was lost, or the texture's size is not the frame's;
    // render() on the CPU then. See EmbeddedGpu for the GL state contract.
    const Frame* render(EmbeddedGpu& gpu, const GpuTexture& texture, uint64_t frameTimeNs = 0);
    // round(logical size * scale): the frame's size in pixels.
    int pixelWidth() const;
    int pixelHeight() const;

    // Pointer input in the window's logical coordinates. Moves outside the
    // window still go to a view holding a press.
    void pointerEnter(PointF position);
    void pointerMove(PointF position);
    void pointerLeave();
    void pointerButton(MouseButton button, bool pressed, uint32_t modifiers = NoModifier);
    void scroll(const ScrollEvent& event);
    // A key as the host's xkb state gives it: the keysym of
    // xkb_state_key_get_one_sym, its xkb_state_key_get_utf8 text and the
    // modifiers in effect. Letters are taken in lower case, ISO_Left_Tab as
    // Tab, and control characters drop out of the text, as on Wayland. The
    // host repeats keys itself, with repeat set.
    void key(const KeyEvent& event, bool pressed);
    // What the input method sent, as text-input-v3's done gathers it, for
    // the focused text view.
    void textInput(const TextInputEvent& event);

    // Touch points in the window's logical coordinates. The first point
    // drives the pointer as synthesized mouse events: a press,
    // moves and a release with PointerEvent::device TouchScreen and no
    // hover left behind; other points are not taken. Cancel releases
    // without a click.
    void touchDown(int32_t id, PointF position);
    void touchMotion(int32_t id, PointF position);
    void touchUp(int32_t id);
    void touchCancel();
    // A touchpad gesture over the window, see View::gesture(). A hold's
    // begin also stops whatever coasts, as on Wayland.
    void gesture(const GestureEvent& event);

    // A drag over the window, in logical coordinates. The offer is the
    // host's and stays valid until leave or drop. Enter and motion return
    // the action the target would take (one DropAction or a dropActions()
    // answer; NoAction refuses); drop() returns the one it took.
    uint32_t dragEnter(PointF position, DropOffer* offer, uint32_t modifiers = NoModifier);
    uint32_t dragMotion(PointF position, uint32_t modifiers = NoModifier);
    void dragLeave();
    uint32_t drop();

    // What the pointer asks for over the view under it, or the view holding
    // a press.
    CursorShape cursorShape() const { return m_reportedShape; }
    std::function<void(CursorShape shape)> onCursorShapeChanged;

private:
    EmbeddedPlatformWindow* platform() const;
    void connectPlatform();
    // Damage and the next request, after either render.
    void finishFrame();
    // At beginFrame()'s frame time when it ran for this one.
    bool renderFrameAt(uint64_t frameTimeNs);

    Frame m_frame;
    // The frame time beginFrame() prepared, until a render draws it.
    uint64_t m_begunFrameNs = 0;
    CursorShape m_reportedShape = CursorShape::Default;
    RectF m_popupGeometry;
};

} // namespace cutefish::ui
