// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/event.h"
#include "ui/geometry.h"

class SkRegion;

namespace cutefish::ui {

class Bitmap;
class DropOffer;
struct DragEvent;
struct DragSource;
struct Layer;
class Painter;
class Window;

enum class Alignment {
    // The layout's own: start in a column, centre in a row.
    Default,
    Start,
    Center,
    End,
};

// How a layout places a view. Lengths are logical pixels.
struct LayoutHints
{
    bool fillWidth = false;
    bool fillHeight = false;
    // The view's share of the space left over, against its filling siblings.
    double stretch = 1;
    // Across the layout's direction.
    Alignment alignment = Alignment::Default;
    // Negative: from the size hint.
    double preferredWidth = -1;
    double preferredHeight = -1;

    // For CellLayout: negative is unset.
    double minimumWidth = -1;
    double minimumHeight = -1;
    double maximumWidth = -1;
    double maximumHeight = -1;
    // Default is left and vertically centred.
    Alignment horizontalAlignment = Alignment::Default;
    Alignment verticalAlignment = Alignment::Default;
    Margins margins;
    // An explicit width and height, which CellLayout takes as the preferred
    // size only when the implicit size is not positive: a spacer 1 wide, not
    // a field 40 high.
    double fallbackWidth = -1;
    double fallbackHeight = -1;

    bool operator==(const LayoutHints&) const = default;
};

// How a window lays its text out: the scale, and whether advances and line
// metrics are hinted onto whole pixels (Window::hintedTextLayout()).
struct TextLayoutMode
{
    double scale = 1;
    bool hinted = true;

    bool operator==(const TextLayoutMode&) const = default;
};

// A node of a window's retained tree. Geometry is in logical pixels,
// relative to the parent; the painter puts edges on device pixels.
class View
{
public:
    View();
    virtual ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // For tests, layout dumps and accessibility; not shown.
    const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    View* parent() const { return m_parent; }
    Window* window() const;
    // How the view's text lays out: as its window's, or out of a window as
    // the nearest setTextLayoutMode() up the tree says, else scale 1 hinted.
    TextLayoutMode textLayoutMode() const;
    // Text in this tree, while it is in no window, lays out as in a window of
    // this mode, so content measures as drawn before its window exists; a
    // popup's mode is its parent window's.
    void setTextLayoutMode(TextLayoutMode mode);

    template<typename T, typename... Args>
    T* addChild(Args&&... args)
    {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T* view = child.get();
        addChild(std::unique_ptr<View>(std::move(child)));
        return view;
    }
    void addChild(std::unique_ptr<View> child);
    std::unique_ptr<View> takeChild(View* child);
    // Moves the child above its siblings.
    void raiseChild(View* child);
    const std::vector<std::unique_ptr<View>>& children() const { return m_children; }

    const RectF& geometry() const { return m_geometry; }
    void setGeometry(const RectF& geometry);
    RectF localRect() const { return {0, 0, m_geometry.width, m_geometry.height}; }

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible);

    // Children paint only inside this view.
    bool clipsChildren() const { return m_clipsChildren; }
    void setClipsChildren(bool clips);
    // With clipsChildren, the clip's corners rounded by this radius
    // (antialiased), as a pill when it is half the height.
    void setChildClipRadius(double radius);

    // Applies to the view and its children.
    double opacity() const { return m_opacity; }
    void setOpacity(double opacity);

    // Applied as the view and its children are painted and hit; layout
    // ignores it. Anything but the identity makes the view layered.
    const Transform& transform() const { return m_transform; }
    void setTransform(const Transform& transform);
    // Paints the view and its children into a texture of their own, kept
    // until they change, so opacity and transform changes cost no repaint.
    // Opacity then applies to them as one, and text loses subpixel colour.
    bool isLayered() const { return m_layered; }
    void setLayered(bool layered);
    // A layered view that paints every pixel of its rect opaquely keeps
    // subpixel colour in its text; its texture holds that rect only, so
    // nothing it or its children paint reaches outside.
    bool hasOpaqueLayer() const { return m_opaqueLayer; }
    void setOpaqueLayer(bool opaque);
    // A layered view that asks itself for the parts a change of its size
    // changes keeps its texture when it grows or shrinks about its top
    // left: only what the new size uncovers is painted then, with whatever
    // it asked for. Otherwise a change of size paints it whole.
    bool keepsLayerOnResize() const { return m_keepsLayerOnResize; }
    void setKeepsLayerOnResize(bool keeps) { m_keepsLayerOnResize = keeps; }
    // Moves what the view last painted inside the rect, in its coordinates,
    // by (dx, dy) and paints only what that uncovers, along with whatever was
    // waiting to be painted there, moved with it. A view without a layer, or
    // a move that is not whole device pixels, paints the rect again.
    void scrollPainted(const RectF& rect, double dx, double dy);

    // Everything the view paints, in its own coordinates: its rect, and more
    // where shadows or focus rings reach out.
    virtual RectF visualRect() const { return localRect(); }

    // A disabled view and its children take no input or focus.
    bool isEnabled() const;
    void setEnabled(bool enabled);

    // Keyboard focus, which only views that accept it can take.
    virtual bool acceptsFocus() const { return false; }
    bool hasFocus() const;
    // Focus that came from the keyboard, which controls draw.
    bool hasVisibleFocus() const;
    void setFocus(FocusReason reason = FocusReason::Other);
    // Out of the Tab chain, focused by a press or from code only.
    void setClickFocusOnly(bool clickOnly) { m_clickFocusOnly = clickOnly; }
    bool isClickFocusOnly() const { return m_clickFocusOnly; }

    // Inside a WindowHeader, a press on the view that moves past the drag
    // distance moves the window, as a press on the header itself does; the
    // view then hears a release outside itself. Header buttons do.
    void setMovesWindow(bool moves) { m_movesWindow = moves; }
    bool movesWindow() const { return m_movesWindow; }

    // The pointer's shape over the view; views may also decide by overriding cursorShape().
    void setCursorShape(CursorShape shape);
    virtual CursorShape cursorShape() const { return CursorShape(m_cursorShape); }

    // Tells the input method that textInputState() changed.
    void updateTextInput();

    // Shown over the view after the pointer rests on it.
    const std::string& toolTip() const { return m_toolTip; }
    void setToolTip(std::string text) { m_toolTip = std::move(text); }

    const LayoutHints& layoutHints() const { return m_layoutHints; }
    void setLayoutHints(const LayoutHints& hints);

    virtual SizeF sizeHint() const { return {}; }
    // For views whose height follows their width, such as wrapped text.
    virtual double heightForWidth(double) const { return sizeHint().height; }
    // The size hint, overridden by the preferred sizes in the layout hints.
    SizeF preferredSize() const;

    // The view's size hint may have changed, or its children need placing
    // again: the view and its ancestors lay out in the next pass. Other
    // views lay out only when their size changes.
    void requestLayout();
    void requestPaint();
    // Only part of the view, in its coordinates, is painted again: the rest
    // of the window keeps what the last frame drew there.
    void requestPaint(const RectF& rect);
    // Changes whenever the view or one of its children asks for layout, or
    // the window's scale, theme or text layout changes, but not when a view
    // is only moved or resized. Measurements of the view taken in one
    // generation may be kept for it.
    uint64_t measurementGeneration() const;
    // The view and its children drawn alone, as for a drag image: device
    // pixels at the scale (the window's when 0), transparent around them.
    // Layers are drawn flat, without their transforms.
    Bitmap grab(double scale = 0);

    PointF mapFromWindow(PointF point) const;
    PointF mapToWindow(PointF point) const;

    // Only views that accept the pointer receive pointer input; a press goes
    // to the topmost one under the pointer, which keeps it until the release.
    virtual bool acceptsPointer() const { return false; }

    // The view is hovered while the pointer is within it, whatever is above it
    // inside it, and hears where the pointer is (hoverMove) on entering, on
    // every move, and when a layout or a scroll moves it under a still pointer.
    // It takes no presses. Off by default.
    void setTracksHover(bool tracks);
    bool tracksHover() const { return m_tracksHover; }
    bool isHovered() const { return m_hoverTracked; }

    // Hands the window's pointer to a drag of the source, from the press the
    // view holds: the view hears no release. False when nothing can drag,
    // as on Wayland so far.
    bool startDrag(DragSource source);

protected:
    virtual void pointerEnter() {}
    virtual void pointerLeave() {}
    virtual void pointerPress(const PointerEvent&) {}
    virtual void pointerMove(const PointerEvent&) {}
    virtual void pointerRelease(const PointerEvent&) {}
    // With setTracksHover: isHovered() changed, and the pointer's place in it.
    virtual void hoverChanged() {}
    virtual void hoverMove(const PointerEvent&) {}

    // The view, or a parent of it, was enabled or disabled.
    virtual void enabledChanged() {}

    virtual void focusIn(FocusReason) {}
    virtual void focusOut() {}
    // Return true when handled; the parent gets what the view leaves.
    virtual bool keyPress(const KeyEvent&) { return false; }
    virtual bool keyRelease(const KeyEvent&) { return false; }
    // To the view under the pointer, then up through its parents.
    virtual bool scroll(const ScrollEvent&) { return false; }

    // Views that edit text take input methods while they have focus.
    virtual bool acceptsTextInput() const { return false; }
    virtual TextInputState textInputState() const { return {}; }
    virtual void textInput(const TextInputEvent&) {}

    // Drops, as views::View has them: a drag goes to the topmost view under
    // it, or the nearest parent, whose canDrop() takes the offer. It hears
    // dragEnter(), then dragUpdate() on every move, which returns the action
    // it would take (one DropAction), then dragLeave() or drop(), which
    // returns the action taken.
    virtual bool canDrop(const DropOffer&) const { return false; }
    virtual void dragEnter(const DragEvent&) {}
    virtual uint32_t dragUpdate(const DragEvent&) { return 0; }
    virtual void dragLeave() {}
    virtual uint32_t drop(const DragEvent&) { return 0; }

    // Touchpad gestures, to the view under the pointer, then up through its
    // parents; the view that takes a begin gets the rest of that gesture.
    virtual bool gesture(const GestureEvent&) { return false; }

    // Positions the children once this view has its geometry.
    virtual void layout() {}
    // After a pass laid out the view or views under it, which have their
    // geometry by now: for placing what follows a view inside it.
    virtual void childrenLaidOut() {}
    virtual void paint(Painter&) {}

private:
    friend class BlurredCopy;
    friend class HeaderBackdrop;
    friend class TestInput;
    friend class Window;

    // Every view's measurements, in every window.
    static void invalidateMeasurements();
    // The view and every view under it lay out again, and measure again too
    // unless told otherwise.
    void invalidateSubtree(bool remeasure = true);
    // Lays out the views that need it, and those under them whose size
    // changes.
    void layoutTree();
    // Culls what lies outside the region, in the target's device pixels.
    void paintTree(Painter& painter, const SkRegion* region);
    // The view and its children as they paint, layers drawn flat, for grab().
    static void paintFlat(View* view, Painter& painter);
    // The view and its children as painted into its layer.
    void paintContent(Painter& painter, const SkRegion* region);
    bool needsLayer() const { return m_layered || !m_transform.isIdentity(); }
    // Drawn from its texture: as last painted, or as it will be.
    bool isComposited(bool painted) const { return painted ? m_layer != nullptr : needsLayer(); }
    // Before a change: damages where the view and its children are now,
    // once until the next frame, which damages where they are then.
    void invalidate();
    // As invalidate(), but keeps a layer's texture: its opacity or transform
    // changed, not what it shows.
    void invalidateComposite();
    // The topmost visible view at the point; with input, only enabled views
    // that accept the pointer.
    View* viewAt(PointF point, bool input);
    bool contains(const View* view) const;
    bool isVisibleInTree() const;

    std::string m_name;
    std::string m_toolTip;
    View* m_parent = nullptr;
    Window* m_window = nullptr;
    std::optional<TextLayoutMode> m_textLayoutMode;
    std::vector<std::unique_ptr<View>> m_children;
    RectF m_geometry;
    LayoutHints m_layoutHints;
    bool m_visible = true;
    bool m_enabled = true;
    bool m_clickFocusOnly = false;
    bool m_tracksHover = false;
    bool m_hoverTracked = false;
    bool m_movesWindow = false;
    // A byte, in the padding the installed applications were built with.
    uint8_t m_cursorShape = uint8_t(CursorShape::Default);
    // Where hoverMove last put the pointer, in the view.
    PointF m_hoverPosition;
    bool m_clipsChildren = false;
    double m_childClipRadius = 0;
    double m_opacity = 1;
    Transform m_transform;
    bool m_layered = false;
    // In the padding before m_layer, as m_cursorShape is.
    bool m_opaqueLayer = false;
    bool m_keepsLayerOnResize = false;
    std::unique_ptr<Layer> m_layer;
    bool m_dirty = false;
    bool m_compositeDirty = false;
    bool m_needsLayout = true;
    bool m_descendantNeedsLayout = false;
    uint64_t m_measurementStamp = 0;
};

} // namespace cutefish::ui
