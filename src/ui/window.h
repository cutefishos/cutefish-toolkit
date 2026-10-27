// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/color_mixer.h"
#include "ui/event.h"
#include "ui/geometry.h"
#include "ui/image.h"
#include "ui/view.h"

class SkCanvas;
struct SkIRect;
class SkRegion;

namespace cutefish::ui {

class DropOffer;
struct DragSource;
struct FrameDamage;
class FrameClient;
class Menu;
class Path;
class PlatformWindow;
class StatusItem;
class StatusItemClient;
class TestInput;
class TrustedConnection;
class View;

// The values follow xdg_positioner's anchors and gravities.
enum class PopupAnchor {
    Center = 0,
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    BottomLeft,
    TopRight,
    BottomRight,
};

// Where a popup goes, relative to its parent window. The compositor moves
// it as allowed when it would not fit on the screen.
struct PopupPlacement
{
    // In the parent window's coordinates.
    RectF anchorRect;
    // The point of the rect the popup hangs from, and the way it grows from there.
    PopupAnchor anchor = PopupAnchor::BottomLeft;
    PopupAnchor gravity = PopupAnchor::BottomRight;
    PointF offset;
    bool flipX = true;
    bool flipY = true;
    bool slide = true;
    bool resize = false;
};

// Where a toplevel opens: next to `rect`, in the compositor's global
// logical coordinates, placed as a popup with this anchor and gravity is.
struct PlacementAnchor
{
    RectF rect;
    PopupAnchor anchor = PopupAnchor::Bottom;
    PopupAnchor gravity = PopupAnchor::Bottom;

    bool operator==(const PlacementAnchor&) const = default;
};

// What the compositor says about a toplevel.
struct WindowStates
{
    bool activated = false;
    bool maximized = false;
    bool fullScreen = false;
    // Snapped to a side; the edges that touch others are square.
    bool tiled = false;

    bool operator==(const WindowStates&) const = default;
};

// cutefish_surface_blur_v1's glass material, every parameter as the
// protocol names it; the defaults are the compositor's. Lengths are logical
// pixels, angles degrees.
struct GlassMaterial
{
    double blurSigma = 1.2;
    double bevel = 14;
    double depth = 16;
    double ior = 1.7;
    double profile = 2.5;
    double displacement = 36;
    double chroma = 0.05;
    double lightAngle = 50;
    double specularIntensity = 0;
    double shininess = 42;
    double sheen = 0;
    double ambientOcclusion = 0;
    double ambientOcclusionRadius = 4;
    double saturation = 1;
    // Straight alpha, each component 0 to 1.
    double tintRed = 1;
    double tintGreen = 1;
    double tintBlue = 1;
    double tintAlpha = 0.12;
    // An edge drawn by the material instead of the window outline; 0 for none.
    double rim = 0;

    bool operator==(const GlassMaterial&) const = default;
};

struct FrameStatistics
{
    // CLOCK_MONOTONIC.
    uint64_t firstSwapNs = 0;
    uint64_t firstPresentedNs = 0;
    uint64_t rendered = 0;
    uint64_t presented = 0;
    uint64_t discarded = 0;
};

// A top-level window drawing its own frame, or a popup of one. It renders on
// the UI thread, once per frame callback, and a configure is answered by the
// next commit with a buffer of the new size.
class Window
{
public:
    Window();
    // A toplevel on the session's trusted connection, for the desktop.
    explicit Window(TrustedConnection& connection);
    // A popup of the parent. With a grab it takes the keyboard, and a press
    // anywhere else closes it. Its size is the one given to resize() before
    // show().
    Window(Window* parent, const PopupPlacement& placement, bool grab = true);
    // A popup for the application's platform: an EmbeddedWindow on Embedded,
    // which the host places and draws, a Window elsewhere.
    static std::unique_ptr<Window> createPopup(Window* parent, const PopupPlacement& placement, bool grab = true);
    virtual ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void setTitle(const std::string& title);
    void setMinimumSize(SizeF size);
    void setMaximumSize(SizeF size);
    // The size the window asks for when the compositor leaves it the choice.
    void resize(SizeF size);
    // A popup: placed again by `placement` at its size now, shown or not, as
    // xdg_popup.reposition. Resize first to move and resize it at once.
    void setPopupPlacement(const PopupPlacement& placement);
    // Keeps the window exactly as tall as its content at its width, measured
    // at the window's scale, since text is taller at some scales.
    void setFitsContentHeight(bool fits);
    // A colour with alpha makes the window translucent. Unset is the theme's
    // background; a recipe naming theme colours follows the theme.
    void setBackgroundColor(std::optional<ColorRecipe> color);
    // A tool tip over a rect of the view, in its coordinates, until unpinned,
    // such as one showing a control's state; hover tool tips wait meanwhile.
    void pinToolTip(View* view, const RectF& rect, std::string text);
    void unpinToolTip(View* view);
    // At scale 1 with full or medium hinting: advances and line metrics on
    // whole pixels (the default on Wayland and headless, as the ported
    // applications were matched). Off, text lays out unrounded at every
    // scale: the default for EmbeddedWindows (the shell, the lock screen,
    // the switcher), measured against the shell's references.
    void setHintedTextLayout(bool hinted);
    bool hintedTextLayout() const { return m_hintedTextLayout; }
    TextLayoutMode textLayoutMode() const { return {m_scale, m_hintedTextLayout}; }
    // Subpixel text (fontconfig's rgba) over translucent parts of the window
    // blended per channel, coloured fringes and all (the default), or
    // greyscale there.
    void setTranslucentSubpixelText(bool subpixel);
    // The rounded body the compositor blurs behind, outlines and shadows.
    void setBackgroundBlur(const RectF& rect, double radius);
    // The outline that masks the blur instead of the rounded rect, such as
    // a bubble's, as cutefish_surface_blur_v1.set_shape; empty for none.
    // Several subpaths blur several areas; the blur rect must cover them.
    void setBackgroundBlurShape(const Path& path);
    // Glass instead of the plain blur over the blur rect, or the plain blur
    // again without one. Wayland only; applies with the next frame.
    void setBackgroundGlass(const std::optional<GlassMaterial>& material);
    // Whether the compositor draws the blur and glass asked for above.
    bool backgroundGlassAvailable() const;
    // A size the window picks itself keeps its centre where it was, instead
    // of its top left corner, as cutefish_window_hints_v1's center anchor.
    void setResizeAroundCenter(bool aroundCenter);
    // The compositor reopens the window where the last window of this
    // application with the same id was closed. Before show().
    void setRestoreId(const std::string& id);
    // Opens next to a point or rectangle on the screen that came from the
    // compositor, such as a status notifier item's activation point, each
    // time it is shown; the compositor keeps it inside the work area. The
    // user moves it freely afterwards. Before show(). Unset by default.
    void setPlacementAnchor(const std::optional<PlacementAnchor>& anchor);
    // The outline a window draws its body in, inside a transparent window,
    // such as a bubble with an arrow: the compositor then neither clips it to
    // a rounded rectangle nor outlines it, and casts its shadow from this
    // outline. Window coordinates; an empty path is the window's rectangle,
    // the default. At once.
    void setBodyShape(const Path& path);
    // Whether the compositor casts the window's shadow (the default); off
    // for a window that draws its own shadow, or wants none, in a
    // transparent surface. At once.
    void setCastsShadow(bool castsShadow);
    // The whole window, frame and shadow included, at this opacity from 0
    // to 1 (the default 1). At 0 it is unseen but still takes the pointer.
    // At once.
    void setOpacity(double opacity);
    // Puts the window's top left at a point in the compositor's global
    // logical coordinates (Application::screens()): before show() it opens
    // there, afterwards it moves there; the compositor keeps it reachable.
    // False where the compositor places windows itself, as it does for
    // ordinary clients.
    bool setScreenPosition(PointF position);
    // Where the window's top left is on the screen, once it is shown, where
    // the compositor tells; unset elsewhere.
    const std::optional<PointF>& screenPosition() const { return m_screenPosition; }
    // After screenPosition() changed, whoever moved the window.
    std::function<void()> onScreenPositionChanged;
    // Whether the user resizes the window by its edges (the default); off,
    // the compositor offers no resize edges or cursors there.
    void setUserResizable(bool resizable);
    // Kept out of the dock and the window switcher, as tool windows are.
    void setSkipTaskbar(bool skip);
    // The main menu the status bar shows while this window is active, in
    // place of the application's (Application::setMainMenu); null goes back
    // to the application's. Not owned; it stays while the window shows it.
    void setMainMenu(Menu* menu);
    // The window's own main menu, else the application's; null for a popup.
    Menu* mainMenu() const;
    // Above ordinary windows.
    void setAlwaysOnTop(bool onTop);
    // The header's colour mode for the window buttons, whatever the theme,
    // unset follows the theme.
    void setHeaderDarkMode(std::optional<bool> dark);
    bool headerDarkMode() const;
    bool isPopup() const { return m_parent != nullptr; }
    Window* parentWindow() const { return m_parent; }
    Window* transientParent() const { return m_transientParent; }

    void setContentView(std::unique_ptr<View> view);
    View* contentView() const { return m_contentView.get(); }
    // Views over the content: each child covers the window and places its own
    // views, painted above the content, taking the pointer where one of them
    // accepts it and leaving it to the content elsewhere.
    View* overlay();

    void show();
    // Off screen at once, though the window lives on until destroyed: a
    // popup closing from inside its own input handling. show() maps it
    // again. Embedded only; on Wayland the surface goes with the window,
    // and a popup only gives the keys back to its parent.
    void hide();
    bool isHidden() const { return m_hidden; }
    void startMove();
    void setMaximized(bool maximized);
    void setFullScreen(bool fullScreen);
    void minimize();
    // Hides every window of the application, as the status bar's Hide does.
    void hideApplication();
    // Kept above the parent window and opened over it; a modal one blocks it.
    void setTransientParent(Window* parent, bool modal);
    // The same for another process's window, by the handle it exported
    // ("wayland:<handle>", as portals pass it), through xdg-foreign; empty
    // clears it. Callable before show().
    void setTransientForForeign(const std::string& handle, bool modal);
    // On a trusted connection: the desktop role on the output (its wl_output
    // name), normal focus; before show(). False without the shell interface.
    bool setDesktopRole(const std::string& outputName);
    // Moves a desktop window to another output.
    void setDesktopOutput(const std::string& outputName);
    // Gives a desktop window the keyboard, as the shell's activate.
    void activateDesktop();
    // A desktop window's work area, in its coordinates: the output less
    // the panels and reserved edges. Empty until the compositor tells.
    const RectF& workArea() const { return m_workArea; }
    std::function<void()> onWorkAreaChanged;
    // The largest size the compositor lets the window take (the work
    // area's size, as xdg_toplevel.configure_bounds gives it), else the
    // screen's. Normal windows hear no work area position on Wayland, so
    // there is no available geometry beyond this size.
    SizeF availableSize() const;

    // Opens at the size the user last left a window of this id at, and
    // maximized when it was left maximized. Kept in
    // ~/.config/cutefish/<name>.conf, the application id without "cutefish-"
    // being the name; windows of one id share it. Before show().
    void setRestoresSize(std::string windowId);
    const WindowStates& states() const { return m_states; }
    // The user is dragging an edge of the window.
    bool isInteractivelyResized() const { return m_interactiveResize; }

    double scale() const { return m_scale; }
    bool hasKeyboardFocus() const { return m_keyboardFocus; }
    // Headless and embedded windows only; on screen the compositor sets the scale.
    void setScale(double scale);
    // The logical size of the screen the window is on, the whole output
    // (no work area); empty when unknown.
    SizeF screenSize() const;
    // Headless and embedded windows: the output's logical size; popups take
    // their parent's while they have none.
    void setScreenSize(SizeF size) { m_screenSize = size; }
    SizeF size() const { return m_size; }
    // The window, or one of its transient family (its transient parents,
    // dialogs and popups), has the compositor's focus. A window stays active
    // while its dialog is focused.
    bool isActive() const;
    // Asks the compositor to focus the window and raise it, through
    // xdg-activation, with the last input.
    void activate();
    // As activate(), with a token another process was given for it, such as
    // Application::takeActivationToken() of a second instance; activate()
    // for an empty one.
    void activate(const std::string& token);
    // The window's identifier for another process's dialog to be parented
    // to, as portals take it: "wayland:<handle>" through xdg-foreign, or
    // empty when the window cannot be exported. Shown windows only.
    void exportHandle(std::function<void(const std::string& handle)> done);
    // An xdg-activation token for an application this one launches, so it
    // opens focused (XDG_ACTIVATION_TOKEN); empty when the compositor gives
    // none. From the window's last input.
    void requestActivationToken(std::function<void(const std::string& token)> done);

    // Every view lays out again, keeping its measurements: for state that
    // views read as they lay out, such as whether the window is active.
    void requestLayout();
    void requestPaint();

    View* focusView() const { return m_focusView; }
    // Runs when no view takes the key. The key is lower case, as in KeyEvent.
    int addShortcut(uint32_t modifiers, uint32_t key, std::function<void()> action);
    void removeShortcut(int id);

    // Renders the content as of the given frame time (CLOCK_MONOTONIC, now
    // when 0), off screen.
    Image grab(uint64_t frameTimeNs = 0);
    // Every view with its logical and device geometry, one per line.
    std::string layoutDump() const;

    const FrameStatistics& frameStatistics() const { return m_statistics; }

    // After the compositor changed the states.
    std::function<void()> onStatesChanged;
    // When a drag of the window's edge starts and when it ends.
    std::function<void()> onInteractiveResizeChanged;
    // After isActive() changed, once the compositor's focus settled.
    std::function<void()> onActiveChanged;
    // Every press in the window, in its logical coordinates, before a view
    // gets it; for closing what the pointer pressed outside of.
    std::function<void(PointF position, MouseButton button)> onPointerPressed;
    // The close button of a toplevel, or a popup the compositor dismissed.
    // Unset, a toplevel quits the application and a popup does nothing.
    std::function<void()> onCloseRequested;

private:
    friend class Application;
    // Repaints and notifies when isActive() differs from what was notified.
    void updateActive();
    Color backgroundColor() const;

    friend class EmbeddedWindow;
    friend class FrameClient;
    friend class HeaderBackdrop;
    friend class PlatformWindow;
    friend class StatusItem;
    friend class Subsurface;

    // A status item's window, made by StatusItem.
    explicit Window(StatusItemClient* statusItem);
    friend class TestInput;
    friend class View;

    struct Shortcut
    {
        int id;
        uint32_t modifiers;
        uint32_t key;
        std::function<void()> action;
    };

    void handleConfigure(int width, int height, const WindowStates& states);
    void handleConfigureBounds(int width, int height);
    void handleWorkArea(const RectF& area);
    void handleScreenPosition(PointF position);
    // A shown modal dialog of this window blocks its pointer.
    bool blockedByModal() const;
    void handleInteractiveResize(bool active);
    void handleScaleChanged(double scale);
    void handleCloseRequested();
    void handleFrameDone();
    void handlePresented(uint64_t timestampNs, uint32_t refreshNs);
    void handleDiscarded();
    void handlePointerEnter(double x, double y);
    void handlePointerLeave();
    void handlePointerMotion(double x, double y);
    void handlePointerButton(MouseButton button, bool pressed, uint32_t modifiers);
    void handlePointerScroll(const ScrollEvent& event);
    void handleKeyboardFocus(bool focused);
    void handleKey(const KeyEvent& event, bool pressed);
    // Runs the shortcut for the key, if there is one.
    bool runShortcut(const KeyEvent& event);
    // A key a text field keeps from the window's shortcuts.
    static bool isEditingSequence(const KeyEvent& event);
    void handleHold();
    void handleTextInput(const TextInputEvent& event);
    void handleGesture(const GestureEvent& event);
    void handleTouchDown(int32_t id, double x, double y);
    void handleTouchMotion(int32_t id, double x, double y);
    void handleTouchUp(int32_t id);
    void handleTouchCancel();
    void handleDragEnter(double x, double y, DropOffer* offer, uint32_t modifiers);
    uint32_t handleDragMotion(double x, double y, uint32_t modifiers);
    void handleDragLeave();
    uint32_t handleDrop();
    bool startDrag(DragSource source);
    void handleThemeChanged();
    void updateTextInput();

    void viewRemoved(View* view);
    void viewDestroyed(View* view);

    void scheduleRender();
    void postRender();
    void render();
    void prepareFrame(uint64_t frameTimeNs);
    // Lays the views out when something asked for it; false when nothing did.
    bool layoutIfNeeded();
    // Lays everything out again and reports the views whose geometry the
    // pass before left different, for TestInput::setVerifiesLayout.
    void verifyLayout();
    static void setVerifiesLayout(bool verifies);
    static bool verifiesLayout();
    static int layoutMismatches();
    // Everything inside the region, or everything without one.
    void paintContent(SkCanvas* canvas, const SkRegion* region);
    void updateHover();
    void updateHoverChain();
    void updateCursor();
    void scheduleToolTip(View* view);
    void showToolTip(View* view, const RectF& rect, const std::string& text);
    void hideToolTip();
    void setFocusView(View* view, FocusReason reason);
    void focusNext(bool backwards);
    PointerEvent pointerEvent(const View* view) const;
    // The press is a drag from a view in the header that moves the window.
    bool pressMovesWindow() const;

    void scheduleFrame();
    // A layout pass for the views marked as needing one, which may be none.
    void scheduleLayout();
    // `prepared`: prepareFrame() already ran at this frame time.
    bool renderFrame(uint64_t frameTimeNs, bool prepared = false);
    std::vector<Rect> paintFrame(SkCanvas* canvas, int bufferAge);
    // Device pixels the view and its children cover, untransformed below the
    // view itself; `painted` asks how they were last painted, not will be.
    SkIRect subtreeRect(const View* view, PointF origin, bool painted) const;
    // As subtreeRect(), through the view's own transform when layered.
    SkIRect placedRect(const View* view, PointF origin, bool painted) const;
    SkIRect mapThroughTransform(const View* view, const SkIRect& rect, PointF position) const;
    // Adds the rect, in the coordinates around the view, to every layer and
    // the window above it.
    void addDamage(const View* view, SkIRect rect, bool painted);
    void damageNow(const View* view);
    void damageRect(const View* view, const RectF& rect);
    // The rect, in the layered view's coordinates, in its texture.
    void damageLayer(View* view, const RectF& rect);
    // Queues a move of the layered view's texture content and moves its
    // damage along; false when the texture cannot be moved.
    bool scrollLayer(View* view, const RectF& rect, double dx, double dy);
    void collectDamage(View* view, PointF origin);
    // Theme and scale changes: the window and every texture.
    void damageAll();
    // The whole window repaints, and any view may paint differently.
    void damageWhole();
    // How often damage from the subtree reached the rect, untransformed in
    // window device pixels, or the whole window repainted, since the watcher
    // first asked; the rect is the one later damage is checked against.
    uint64_t watchDamage(const View* watcher, const View* subtree, const SkIRect& rect);
    void unwatchDamage(const View* watcher);
    // How text draws into this window's canvas, before it is painted.
    void applyTextTarget() const;
    // `visible`: where the clips above the view leave it room, in window
    // device pixels, or null below a layer; textures wholly outside it wait
    // until they come into view.
    void updateLayers(View* view, PointF origin, SkCanvas* target, const SkIRect* visible);
    void addFrameClient(FrameClient* client);
    void removeFrameClient(FrameClient* client);
    bool hasTickingClients() const;
    void interruptFrameClients(int interruption);
    uint64_t frameTime(uint64_t now);
    void fitContentHeight();
    void restoreSize();
    void saveSize();
    void saveMaximized();

    std::unique_ptr<PlatformWindow> m_platform;
    // The window's own main menu; null for the application's.
    Menu* m_mainMenu = nullptr;
    SizeF m_screenSize;
    std::optional<PointF> m_screenPosition;
    std::unique_ptr<FrameDamage> m_frameDamage;
    Window* m_parent = nullptr;
    // Popups keep no keyboard focus of their own: the root window's keys go
    // to the innermost grabbing popup, as the compositor leaves them.
    std::vector<Window*> m_grabbingPopups;
    // Every popup of this window, grabbing or not, oldest first.
    std::vector<Window*> m_popups;
    std::unique_ptr<View> m_contentView;
    std::unique_ptr<View> m_overlay;
    // The overlay's views first, then the content's.
    View* viewAt(PointF point, bool input) const;
    std::vector<FrameClient*> m_frameClients;
    std::shared_ptr<Window*> m_self;

    SizeF m_size = {640, 480};
    SizeF m_minimumSize;
    SizeF m_maximumSize = {16777215, 16777215};
    std::string m_restoreId;
    // The work area, which a restored size stays inside.
    SizeF m_bounds;
    bool m_boundsApplied = false;
    // The size before the window went maximized, full screen or tiled, for a
    // configure that leaves the size to the window on the way back.
    SizeF m_normalSize;
    bool m_interactiveResize = false;
    bool m_resized = false;
    int m_saveTimer = 0;
    double m_scale = 1.0;
    bool m_fitsContentHeight = false;
    SizeF m_fittedSize;
    std::optional<ColorRecipe> m_backgroundColor;
    WindowStates m_states;
    Window* m_transientParent = nullptr;
    // isActive() as last notified.
    bool m_active = false;
    bool m_configured = false;
    bool m_layoutNeeded = true;
    bool m_needsFrame = false;
    bool m_framePending = false;
    bool m_renderPosted = false;
    bool m_rendering = false;
    FrameStatistics m_statistics;
    uint64_t m_frameTimeNs = 0;
    uint64_t m_refreshNs = 16666667;

    PointF m_pointer;
    // Layout passes re-hover only a pointer still over the window.
    bool m_pointerInside = false;
    View* m_hovered = nullptr;
    // The views tracking hover that the pointer is within, deepest first.
    std::vector<View*> m_hoverChain;
    View* m_pressed = nullptr;
    // A touchpad gesture stays with the view that took its first movement.
    View* m_scrollTarget = nullptr;
    // The view that took the current pinch, swipe or hold.
    View* m_gestureTarget = nullptr;
    // What pointer events say they come from: TouchScreen while a touch
    // point drives the pointer.
    PointerDevice m_pointerDevice = PointerDevice::TouchPad;
    // The touch point that drives the pointer, the first one down; -1 for
    // none.
    int32_t m_touchId = -1;
    // The drag over the window, its target and the action the target took.
    DropOffer* m_dropOffer = nullptr;
    View* m_dropTarget = nullptr;
    uint32_t m_dropAction = 0;
    PointF m_dropPosition;
    uint32_t m_dropModifiers = 0;
    MouseButton m_pressedButton = MouseButton::Left;
    uint32_t m_modifiers = NoModifier;
    uint64_t m_lastPressNs = 0;
    PointF m_lastPressPosition;
    MouseButton m_lastPressButton = MouseButton::Left;
    int m_clickCount = 0;
    // Unset after the pointer enters, so the next update sends it.
    int m_cursorShape = -1;

    // Painted over the content, inside the window.
    std::unique_ptr<View> m_toolTip;
    View* m_toolTipOwner = nullptr;
    // A press puts the tip away until the pointer moves to another view.
    View* m_toolTipDismissed = nullptr;
    int m_toolTipTimer = 0;
    bool m_toolTipPinned = false;

    View* m_focusView = nullptr;
    FocusReason m_focusReason = FocusReason::Other;
    bool m_keyboardFocus = false;
    bool m_shown = false;
    bool m_hidden = false;
    bool m_translucentSubpixel = true;
    bool m_hintedTextLayout = true;
    std::optional<bool> m_headerDarkMode;
    RectF m_workArea;
    bool m_modal = false;
    std::vector<Shortcut> m_shortcuts;
    int m_nextShortcutId = 1;
};

} // namespace cutefish::ui
