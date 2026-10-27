// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ui/event.h"
#include "ui/path.h"
#include "ui/window.h"

struct wl_callback;
struct wl_output;
struct wl_surface;
struct wp_fractional_scale_v1;
struct wp_presentation_feedback;
struct wp_viewport;
struct xdg_dialog_v1;
struct xdg_popup;
struct xdg_surface;
struct xdg_toplevel;
struct zxdg_imported_v2;
struct cutefish_shell_surface_v1;
struct cutefish_status_item_v1;

namespace cutefish::ui {
class DropOffer;
}

namespace cutefish::ui::wayland {

class Connection;
struct ShellSurfaceListeners;

// An xdg_surface, as a toplevel or a popup, with a viewport and a fractional
// scale. Its size is in logical pixels; the buffer is whatever the renderer
// attaches.
class ShellSurface
{
public:
    class Delegate
    {
    public:
        virtual ~Delegate() = default;

        // A configure sequence ended. A zero size leaves the choice to the client.
        virtual void configure(int width, int height, const WindowStates& states) = 0;
        virtual void scaleChanged(double scale) = 0;
        // The close button of a toplevel, or a popup dismissed by the compositor.
        virtual void closeRequested() = 0;
        virtual void frameDone() = 0;
        virtual void presented(uint64_t timestampNs, uint32_t refreshNs) = 0;
        virtual void discarded() = 0;

        virtual void pointerEnter(double x, double y) = 0;
        virtual void pointerLeave() = 0;
        virtual void pointerMotion(double x, double y) = 0;
        virtual void pointerButton(MouseButton button, bool pressed, uint32_t modifiers) = 0;
        virtual void pointerScroll(const ScrollEvent& event) = 0;
        // Fingers came to rest on the touchpad.
        virtual void pointerHold() = 0;

        // Touch points in surface-local logical pixels.
        virtual void touchDown(int32_t, double, double) {}
        virtual void touchMotion(int32_t, double, double) {}
        virtual void touchUp(int32_t) {}
        virtual void touchCancel() {}

        virtual void keyboardEnter() = 0;
        virtual void keyboardLeave() = 0;
        virtual void key(const KeyEvent& event, bool pressed) = 0;
        virtual void textInput(const TextInputEvent& event) = 0;

        // The largest size the window should take, the work area's; before
        // the configure it belongs to.
        virtual void configureBounds(int, int) {}
        // The user started or ended resizing the window by an edge.
        virtual void interactiveResize(bool) {}
        // Where a toplevel's top left is on the screen, in the compositor's
        // global logical coordinates; only where it may place itself.
        virtual void screenPosition(PointF) {}

        // A drag over the surface; enter and motion return the action the
        // window would take, drop the one it took.
        virtual uint32_t dragEnter(double, double, DropOffer*, uint32_t) { return 0; }
        virtual uint32_t dragMotion(double, double, uint32_t) { return 0; }
        virtual void dragLeave() {}
        virtual uint32_t drop() { return 0; }
        // A desktop surface's work area, surface-local logical pixels.
        virtual void workArea(const RectF&) {}

        // A status item's: the widest it may be, ahead of the configure that
        // carries the height; the bar's look; its highlight; its place on
        // the screen, empty while it has none.
        virtual void statusItemLimits(int) {}
        virtual void statusItemAppearance(bool, uint32_t) {}
        virtual void statusItemHighlight(bool) {}
        virtual void statusItemPlaced(const RectF&) {}
    };

    struct StatusItemRole {};

    // A toplevel.
    ShellSurface(Connection& connection, Delegate* delegate);
    // A status item in the bar, which the connection must offer.
    ShellSurface(Connection& connection, Delegate* delegate, StatusItemRole);
    // A popup of the parent, placed relative to it; with a grab, it takes the
    // keyboard and goes when the pointer presses elsewhere.
    ShellSurface(Connection& connection, Delegate* delegate, ShellSurface* parent,
                 const PopupPlacement& placement, int width, int height, bool grab);
    ~ShellSurface();

    ShellSurface(const ShellSurface&) = delete;
    ShellSurface& operator=(const ShellSurface&) = delete;

    Delegate* delegate() const { return m_delegate; }
    wl_surface* surface() const { return m_surface; }
    // Null for a popup.
    xdg_toplevel* toplevel() const { return m_xdgToplevel; }
    bool isStatusItem() const { return m_statusItem != nullptr; }
    Connection& connection() const { return m_connection; }

    void setTitle(const std::string& title);
    void setAppId(const std::string& appId);
    void setMinimumSize(int width, int height);
    void setMaximumSize(int width, int height);
    void startMove();
    void setMaximized(bool maximized);
    void setFullScreen(bool fullScreen);
    void minimize();
    // A dialog of the parent: kept above it, modal when asked.
    void setParent(ShellSurface* parent, bool modal);
    // A dialog of another client's toplevel, from its exported handle
    // (with or without the "wayland:" prefix); empty clears it.
    void setForeignParent(const std::string& handle, bool modal);
    // The size a window mapped maximized returns to; before map().
    void setRestoreSize(int width, int height);

    // The logical size of the output the surface is on, its parent's while it
    // is on none, or the first output's.
    SizeF outputSize() const;

    // The initial commit, without a buffer; the compositor answers with a configure.
    void map();
    // A popup: placed again, as xdg_popup.reposition; the compositor answers
    // with a configure. Ignored by a compositor without it.
    void reposition(const PopupPlacement& placement, int width, int height);

    // The rounded body the compositor blurs behind, outlines and shadows; an
    // empty rect turns it off. Applies with the next commit.
    void setBlur(const RectF& rect, double radius);
    // The outline masking the blur; an empty path goes back to the rect.
    void setBlurShape(const Path& path);
    // Glass over the blur rect, or the plain blur without a material.
    void setGlass(const std::optional<GlassMaterial>& material);
    void setResizeAroundCenter(bool aroundCenter);
    void setSkipTaskbar(bool skip);
    void setAlwaysOnTop(bool onTop);
    void hideApplication();
    void setRestoreId(const std::string& id);
    void setPlacementAnchor(const std::optional<PlacementAnchor>& anchor);
    // A toplevel's resize edges, which the compositor owns; on by default.
    void setServerResize(bool enabled);
    // See cutefish_window_hints_v1.set_body_shape; empty unsets it.
    void setBodyShape(const Path& path);
    // See cutefish_window_hints_v1.set_shadow.
    void setCastsShadow(bool castsShadow);
    // See cutefish_window_hints_v1.set_opacity.
    void setOpacity(double opacity);
    // A toplevel's top left at a point in the compositor's global logical
    // coordinates, the next time it maps or at once when it is mapped; false
    // when the compositor does not let this client place its windows.
    bool setScreenPosition(PointF position);
    // A status item's identifier and highlight request; at once.
    void setStatusItemIdentifier(const std::string& identifier);
    void setStatusItemHighlight(bool highlighted);
    // cutefish_shell_v1 on a trusted connection: the desktop role on the
    // output, normal focus; before map().
    bool setDesktop(const std::string& outputName);
    void setShellOutput(const std::string& outputName);
    void activateShellSurface();

    // Everything that has to land in the same commit as the next buffer:
    // the configure ack, the viewport, the opaque region, the next frame
    // callback and presentation feedback.
    void prepareCommit(int width, int height, bool opaque);

private:
    friend struct ShellSurfaceListeners;

    void createSurface();
    void createXdgSurface();

    Connection& m_connection;
    Delegate* m_delegate;
    wl_surface* m_surface = nullptr;
    wp_viewport* m_viewport = nullptr;
    wp_fractional_scale_v1* m_fractionalScale = nullptr;
    xdg_surface* m_xdgSurface = nullptr;
    xdg_toplevel* m_xdgToplevel = nullptr;
    xdg_popup* m_xdgPopup = nullptr;
    xdg_dialog_v1* m_dialog = nullptr;
    zxdg_imported_v2* m_imported = nullptr;
    cutefish_shell_surface_v1* m_shellSurface = nullptr;
    cutefish_status_item_v1* m_statusItem = nullptr;
    wl_callback* m_frameCallback = nullptr;
    ShellSurface* m_parent = nullptr;
    // The outputs the surface is on, the first entered first.
    std::vector<wl_output*> m_outputs;
    // The request of the same name hides the type in C++.
    std::unordered_set<struct wp_presentation_feedback*> m_feedbacks;

    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    WindowStates m_pendingStates;
    uint32_t m_configureSerial = 0;
    bool m_configurePending = false;
    int m_committedWidth = 0;
    int m_committedHeight = 0;
    bool m_committedOpaque = false;
};

} // namespace cutefish::ui::wayland
