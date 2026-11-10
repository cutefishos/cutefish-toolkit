// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/view.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "ui/drag.h"
#include "ui/layer.h"
#include "ui/painter.h"
#include "ui/window.h"

namespace cutefish::ui {

// Measurement stamps and the epoch come from one counter, so a view's
// generation changes both when it is stamped and when the epoch moves.
static uint64_t s_measurementCounter = 1;
static uint64_t s_measurementEpoch = 1;
// Inside a layout pass, a resized view's parent placed it already.
static int s_layoutDepth = 0;
// The view whose layout() runs: views under it lay out after it anyway.
static View* s_layingOut = nullptr;

View::View() = default;

View::~View()
{
    if (Window* window = this->window())
        window->viewDestroyed(this);
}

Window* View::window() const
{
    const View* view = this;
    while (view->m_parent)
        view = view->m_parent;
    return view->m_window;
}

TextLayoutMode View::textLayoutMode() const
{
    if (const Window* window = this->window())
        return window->textLayoutMode();
    for (const View* view = this; view; view = view->m_parent) {
        if (view->m_textLayoutMode)
            return *view->m_textLayoutMode;
    }
    return {};
}

void View::setTextLayoutMode(TextLayoutMode mode)
{
    m_textLayoutMode = mode;
    invalidateSubtree();
    requestLayout();
}

void View::addChild(std::unique_ptr<View> child)
{
    child->m_parent = this;
    m_children.push_back(std::move(child));
    // It may have been measured in another window, at another scale.
    m_children.back()->invalidateSubtree();
    m_children.back()->invalidate();
    requestLayout();
}

std::unique_ptr<View> View::takeChild(View* child)
{
    const auto it = std::find_if(m_children.begin(), m_children.end(),
                                 [child](const auto& view) { return view.get() == child; });
    if (it == m_children.end())
        return nullptr;

    child->invalidate();
    if (Window* window = this->window())
        window->viewRemoved(child);
    std::unique_ptr<View> view = std::move(*it);
    m_children.erase(it);
    view->m_parent = nullptr;
    requestLayout();
    return view;
}

void View::raiseChild(View* child)
{
    const auto it = std::find_if(m_children.begin(), m_children.end(),
                                 [child](const auto& view) { return view.get() == child; });
    if (it == m_children.end() || it + 1 == m_children.end())
        return;
    child->invalidate();
    std::rotate(it, it + 1, m_children.end());
    // Layouts place children in their order.
    requestLayout();
}

void View::setGeometry(const RectF& geometry)
{
    if (geometry == m_geometry)
        return;
    const bool resized = geometry.size() != m_geometry.size();
    // A texture moved whole device pixels is kept (Window::updateLayers), as
    // is one resized about its top left that asks for its changes itself.
    const bool kept = !resized || (m_keepsLayerOnResize && geometry.topLeft() == m_geometry.topLeft());
    if (kept && needsLayer())
        invalidateComposite();
    else
        invalidate();
    m_geometry = geometry;
    if (!resized)
        return;
    m_needsLayout = true;
    // Outside a pass, the parent places the view again, as it would have.
    if (m_parent && s_layoutDepth == 0)
        m_parent->m_needsLayout = true;
    View* root = this;
    for (View* view = m_parent; view; view = view->m_parent) {
        view->m_descendantNeedsLayout = true;
        root = view;
    }
    if (root->m_window)
        root->m_window->scheduleLayout();
}

void View::setVisible(bool visible)
{
    if (visible == m_visible)
        return;
    invalidate();
    m_visible = visible;
    if (!visible) {
        if (Window* window = this->window())
            window->viewRemoved(this);
    }
    // Hidden views take no space in layouts.
    requestLayout();
}

void View::setClipsChildren(bool clips)
{
    if (clips == m_clipsChildren)
        return;
    invalidate();
    m_clipsChildren = clips;
}

void View::setChildClipRadius(double radius)
{
    if (radius == m_childClipRadius)
        return;
    invalidate();
    m_childClipRadius = radius;
}

void View::setOpacity(double opacity)
{
    if (opacity == m_opacity)
        return;
    invalidateComposite();
    m_opacity = opacity;
}

void View::setTransform(const Transform& transform)
{
    if (transform == m_transform)
        return;
    invalidateComposite();
    m_transform = transform;
    // Hit testing follows the transform.
    if (Window* window = this->window())
        window->scheduleLayout();
}

void View::setLayered(bool layered)
{
    if (layered == m_layered)
        return;
    invalidateComposite();
    m_layered = layered;
}

void View::setOpaqueLayer(bool opaque)
{
    if (opaque == m_opaqueLayer)
        return;
    m_opaqueLayer = opaque;
    if (m_layer)
        m_layer->surface.reset();
    invalidate();
}

void View::scrollPainted(const RectF& rect, double dx, double dy)
{
    Window* window = this->window();
    if (!window || m_dirty || !window->scrollLayer(this, rect, dx, dy))
        requestPaint(rect);
}

void View::invalidate()
{
    Window* window = this->window();
    if (window && !m_dirty && !m_compositeDirty)
        window->damageNow(this);
    m_dirty = true;
    if (window)
        window->scheduleFrame();
}

void View::invalidateComposite()
{
    Window* window = this->window();
    if (window && !m_dirty && !m_compositeDirty)
        window->damageNow(this);
    m_compositeDirty = true;
    if (window)
        window->scheduleFrame();
}

bool View::isEnabled() const
{
    for (const View* view = this; view; view = view->m_parent) {
        if (!view->m_enabled)
            return false;
    }
    return true;
}

void View::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (!enabled) {
        if (Window* window = this->window())
            window->viewRemoved(this);
    }
    const std::function<void(View*)> notify = [&notify](View* view) {
        view->enabledChanged();
        for (const auto& child : view->m_children)
            notify(child.get());
    };
    notify(this);
    // Views may lay out by it, and a layout looks again at what is under
    // the pointer.
    requestLayout();
}

bool View::hasFocus() const
{
    const Window* window = this->window();
    return window && window->focusView() == this;
}

bool View::hasVisibleFocus() const
{
    const Window* window = this->window();
    return hasFocus() && window->m_focusReason == FocusReason::Keyboard && window->isActive();
}

void View::setCursorShape(CursorShape shape)
{
    if (uint8_t(shape) == m_cursorShape)
        return;
    m_cursorShape = uint8_t(shape);
    // A still pointer takes the new shape at once.
    if (Window* window = this->window(); window && window->m_pointerInside)
        window->updateCursor();
}

void View::setFocus(FocusReason reason)
{
    if (Window* window = this->window())
        window->setFocusView(this, reason);
}

void View::updateTextInput()
{
    if (Window* window = this->window(); window && window->focusView() == this)
        window->updateTextInput();
}

bool View::startDrag(DragSource source)
{
    Window* window = this->window();
    return window && window->startDrag(std::move(source));
}

void View::setLayoutHints(const LayoutHints& hints)
{
    if (hints == m_layoutHints)
        return;
    m_layoutHints = hints;
    requestLayout();
}

SizeF View::preferredSize() const
{
    SizeF size = sizeHint();
    if (m_layoutHints.preferredWidth >= 0)
        size.width = m_layoutHints.preferredWidth;
    if (m_layoutHints.preferredHeight >= 0)
        size.height = m_layoutHints.preferredHeight;
    return size;
}

void View::requestLayout()
{
    invalidate();
    const uint64_t stamp = ++s_measurementCounter;
    View* root = this;
    bool placing = true;
    for (View* view = this; view; view = view->m_parent) {
        view->m_measurementStamp = stamp;
        // Asked from the layout() of a view above it, as when that view
        // fills its children: they lay out next in this pass, and that view
        // and those above it keep their places.
        if (view == s_layingOut)
            placing = false;
        if (placing)
            view->m_needsLayout = true;
        root = view;
    }
    if (root->m_window)
        root->m_window->scheduleLayout();
}

uint64_t View::measurementGeneration() const
{
    return std::max(m_measurementStamp, s_measurementEpoch);
}

void View::invalidateMeasurements()
{
    s_measurementEpoch = ++s_measurementCounter;
}

void View::invalidateSubtree(bool remeasure)
{
    const uint64_t stamp = ++s_measurementCounter;
    const auto mark = [stamp, remeasure](const auto& self, View* view) -> void {
        if (remeasure)
            view->m_measurementStamp = stamp;
        view->m_needsLayout = true;
        view->m_descendantNeedsLayout = !view->m_children.empty();
        for (const auto& child : view->m_children)
            self(self, child.get());
    };
    mark(mark, this);
}

void View::setTracksHover(bool tracks)
{
    if (tracks == m_tracksHover)
        return;
    m_tracksHover = tracks;
    if (Window* window = this->window())
        window->updateHover();
}

void View::requestPaint()
{
    invalidate();
}

void View::requestPaint(const RectF& rect)
{
    Window* window = this->window();
    if (!window) {
        invalidate();
        return;
    }
    if (!m_dirty) {
        if (m_layer)
            window->damageLayer(this, rect);
        if (!m_compositeDirty)
            window->damageRect(this, rect);
    }
    window->scheduleFrame();
}

// From the parent's coordinates into the view's, through its transform.
static PointF mapFromParent(const View* view, PointF point)
{
    const RectF& geometry = view->geometry();
    const Transform& transform = view->transform();
    if (!transform.isIdentity()) {
        const PointF centre = {geometry.x + geometry.width / 2, geometry.y + geometry.height / 2};
        point = centre + (point - centre - transform.translation) / transform.scale;
    }
    return point - geometry.topLeft();
}

static PointF mapToParent(const View* view, PointF point)
{
    const RectF& geometry = view->geometry();
    const Transform& transform = view->transform();
    point += geometry.topLeft();
    if (!transform.isIdentity()) {
        const PointF centre = {geometry.x + geometry.width / 2, geometry.y + geometry.height / 2};
        point = centre + transform.translation + (point - centre) * transform.scale;
    }
    return point;
}

PointF View::mapFromWindow(PointF point) const
{
    std::vector<const View*> path;
    for (const View* view = this; view; view = view->m_parent)
        path.push_back(view);
    for (auto it = path.rbegin(); it != path.rend(); ++it)
        point = mapFromParent(*it, point);
    return point;
}

PointF View::mapToWindow(PointF point) const
{
    for (const View* view = this; view; view = view->m_parent)
        point = mapToParent(view, point);
    return point;
}

void View::layoutTree()
{
    ++s_layoutDepth;
    if (m_needsLayout) {
        m_needsLayout = false;
        const uint64_t stamp = m_measurementStamp;
        // What its parent placed it by, as measured already.
        const SizeF size = m_parent ? preferredSize() : SizeF{};
        const double height = m_parent ? heightForWidth(m_geometry.width) : 0;
        View* const outer = std::exchange(s_layingOut, this);
        layout();
        s_layingOut = outer;
        // A view under it changed what this view measures: the parent
        // places it again.
        if (m_parent && m_measurementStamp != stamp
            && (!(preferredSize() == size) || heightForWidth(m_geometry.width) != height)) {
            for (View* view = m_parent; view; view = view->m_parent)
                view->m_needsLayout = true;
        }
    }
    // Views below marked meanwhile set it again.
    m_descendantNeedsLayout = false;
    for (size_t i = 0; i < m_children.size(); ++i) {
        View* child = m_children[i].get();
        if (child->m_needsLayout || child->m_descendantNeedsLayout)
            child->layoutTree();
    }
    childrenLaidOut();
    m_descendantNeedsLayout = std::any_of(m_children.begin(), m_children.end(), [](const auto& child) {
        return child->m_needsLayout || child->m_descendantNeedsLayout;
    });
    --s_layoutDepth;
}

View* View::viewAt(PointF point, bool input)
{
    if (!m_visible || !localRect().contains(point) || (input && !m_enabled))
        return nullptr;
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        if (View* view = (*it)->viewAt(mapFromParent(it->get(), point), input))
            return view;
    }
    return !input || acceptsPointer() ? this : nullptr;
}

bool View::isVisibleInTree() const
{
    for (const View* view = this; view; view = view->m_parent) {
        if (!view->m_visible)
            return false;
    }
    return true;
}

bool View::contains(const View* view) const
{
    for (; view; view = view->m_parent) {
        if (view == this)
            return true;
    }
    return false;
}

} // namespace cutefish::ui
