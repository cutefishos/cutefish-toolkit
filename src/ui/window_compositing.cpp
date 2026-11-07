// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkSurface.h"

#include "ui/application.h"
#include "ui/bitmap.h"
#include "ui/layer.h"
#include "ui/painter.h"
#include "ui/text_context.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/window.h"

namespace cutefish::ui {

// Frames whose damage is kept for buffer age; older buffers repaint whole.
static constexpr size_t kDamageHistory = 4;
// Past this many rects a region clips as its bounds, which is cheaper to draw.
static constexpr int kMaximumDamageRects = 16;

static SkIRect intersection(const SkIRect& a, const SkIRect& b)
{
    SkIRect result;
    return result.intersect(a, b) ? result : SkIRect::MakeEmpty();
}

// Repaints the target inside the clip; `paint` culls by the region it gets.
// The GPU clips. The raster backend antialiases edges a clip of many parts
// crosses differently, so it paints aside, clipped to the parts' bounds
// alone, and copies the clip in, to match full repaints. The scratch reaches
// from the origin: shaders evaluated at other coordinates differ by a level.
// It is kept from frame to frame.
template<typename Paint>
static void repaint(SkCanvas* target, const SkRegion& clip, const Paint& paint)
{
    if (clip.isEmpty())
        return;
    if (target->recordingContext()) {
        target->save();
        target->clipRegion(clip);
        paint(target);
        target->restore();
        return;
    }
    const SkIRect bounds = clip.getBounds();
    const SkSurfaceProps properties = target->getBaseProps();
    const SkImageInfo info = target->imageInfo();
    thread_local sk_sp<SkSurface> scratch;
    if (!scratch || scratch->width() < bounds.right() || scratch->height() < bounds.bottom()
        || scratch->imageInfo().colorType() != info.colorType() || scratch->imageInfo().alphaType() != info.alphaType()
        || scratch->props() != properties) {
        scratch = SkSurfaces::Raster(info.makeWH(std::max(info.width(), bounds.right()),
                                                  std::max(info.height(), bounds.bottom())), &properties);
        if (!scratch)
            return;
    }
    SkCanvas* canvas = scratch->getCanvas();
    canvas->save();
    canvas->clipIRect(bounds);
    paint(canvas);
    canvas->restore();
    SkPaint copy;
    copy.setBlendMode(SkBlendMode::kSrc);
    target->save();
    target->clipRegion(clip);
    target->drawImage(scratch->makeImageSnapshot(), 0, 0, SkSamplingOptions(), &copy);
    target->restore();
}

static SkIRect toSkIRect(const Rect& rect)
{
    return SkIRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height);
}

// The device pixels a logical rect touches, and one more for antialiasing.
static SkIRect deviceBounds(const RectF& rect, double scale)
{
    return SkIRect::MakeLTRB(int(std::floor(rect.x * scale)) - 1, int(std::floor(rect.y * scale)) - 1,
                             int(std::ceil(rect.right() * scale)) + 1,
                             int(std::ceil(rect.bottom() * scale)) + 1);
}

// As Painter::clip() rounds it.
static SkIRect clipBounds(const View* view, PointF position, double scale)
{
    const RectF& geometry = view->geometry();
    return SkIRect::MakeLTRB(int(std::lround(position.x * scale)), int(std::lround(position.y * scale)),
                             int(std::lround((position.x + geometry.width) * scale)),
                             int(std::lround((position.y + geometry.height) * scale)));
}

static SkMatrix layerMatrix(const View* view, PointF position, double scale)
{
    const Transform& transform = view->transform();
    // Whole device pixels, so a texture at rest lands on the pixels it was drawn for.
    const double dx = std::round(transform.translation.x * scale);
    const double dy = std::round(transform.translation.y * scale);
    // Exact, where going through the centre in floats could land a hair short.
    if (transform.scale == 1)
        return SkMatrix::Translate(SkScalar(dx), SkScalar(dy));
    const RectF& geometry = view->geometry();
    const double centreX = (position.x + geometry.width / 2) * scale;
    const double centreY = (position.y + geometry.height / 2) * scale;
    SkMatrix matrix = SkMatrix::Translate(SkScalar(centreX + dx), SkScalar(centreY + dy));
    matrix.preScale(SkScalar(transform.scale), SkScalar(transform.scale));
    matrix.preTranslate(SkScalar(-centreX), SkScalar(-centreY));
    return matrix;
}

SkIRect Window::subtreeRect(const View* view, PointF origin, bool painted) const
{
    if (!view->m_visible || view->m_opacity <= 0)
        return SkIRect::MakeEmpty();
    const PointF position = origin + view->m_geometry.topLeft();
    const RectF visual = view->visualRect();
    SkIRect rect = deviceBounds({position.x + visual.x, position.y + visual.y, visual.width, visual.height},
                                m_scale);
    for (const auto& child : view->m_children)
        rect.join(placedRect(child.get(), position, painted));
    // An opaque texture holds the view's rect and nothing around it.
    if (view->m_clipsChildren || (view->m_opaqueLayer && view->needsLayer()))
        rect = intersection(rect, clipBounds(view, position, m_scale));
    return rect;
}

SkIRect Window::placedRect(const View* view, PointF origin, bool painted) const
{
    const SkIRect rect = subtreeRect(view, origin, painted);
    if (rect.isEmpty() || !view->isComposited(painted))
        return rect;
    return mapThroughTransform(view, rect, origin + view->m_geometry.topLeft());
}

SkIRect Window::mapThroughTransform(const View* view, const SkIRect& rect, PointF position) const
{
    const SkMatrix matrix = layerMatrix(view, position, m_scale);
    if (view->m_transform.scale == 1)
        return rect.makeOffset(int(matrix.getTranslateX()), int(matrix.getTranslateY()));
    // Filtering reaches a pixel beyond the scaled edge.
    return matrix.mapRect(SkRect::Make(rect)).roundOut().makeOutset(1, 1);
}

void Window::addDamage(const View* view, SkIRect rect, bool painted)
{
    // The ancestors with their untransformed positions, nearest first.
    std::vector<std::pair<const View*, PointF>> ancestors;
    for (const View* ancestor = view->m_parent; ancestor; ancestor = ancestor->m_parent)
        ancestors.emplace_back(ancestor, PointF());
    PointF origin;
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        origin += it->first->m_geometry.topLeft();
        it->second = origin;
    }

    // Unclipped, through the transforms up to the subtree's own: as the
    // subtree paints on its own.
    for (FrameDamage::Watch& watch : m_frameDamage->watches) {
        SkIRect placed = rect;
        bool inside = view == watch.subtree;
        for (const auto& [ancestor, position] : ancestors) {
            if (inside)
                break;
            if (ancestor->isComposited(painted))
                placed = mapThroughTransform(ancestor, placed, position);
            inside = ancestor == watch.subtree;
        }
        if (inside && SkIRect::Intersects(placed, watch.rect))
            ++watch.changes;
    }

    for (const auto& [ancestor, position] : ancestors) {
        if (!ancestor->m_visible || ancestor->m_opacity <= 0)
            return;
        if (ancestor->m_clipsChildren || (ancestor->m_opaqueLayer && ancestor->needsLayer()))
            rect = intersection(rect, clipBounds(ancestor, position, m_scale));
        if (rect.isEmpty())
            return;
        if (ancestor->isComposited(painted)) {
            if (ancestor->m_layer)
                ancestor->m_layer->damage.op(rect, SkRegion::kUnion_Op);
            rect = mapThroughTransform(ancestor, rect, position);
        }
    }
    const SkIRect window = SkIRect::MakeWH(int(std::lround(m_size.width * m_scale)),
                                           int(std::lround(m_size.height * m_scale)));
    m_frameDamage->pending.op(intersection(rect, window), SkRegion::kUnion_Op);
}

void Window::damageNow(const View* view)
{
    PointF origin;
    for (const View* ancestor = view->m_parent; ancestor; ancestor = ancestor->m_parent)
        origin += ancestor->m_geometry.topLeft();
    addDamage(view, placedRect(view, origin, true), true);
}

void Window::damageRect(const View* view, const RectF& rect)
{
    PointF origin;
    for (const View* ancestor = view->m_parent; ancestor; ancestor = ancestor->m_parent)
        origin += ancestor->m_geometry.topLeft();
    const PointF position = origin + view->m_geometry.topLeft();
    SkIRect device = SkRect::MakeXYWH(SkScalar((position.x + rect.x) * m_scale), SkScalar((position.y + rect.y) * m_scale),
                                      SkScalar(rect.width * m_scale), SkScalar(rect.height * m_scale))
                             .roundOut();
    if (view->isComposited(true))
        device = mapThroughTransform(view, device, position);
    addDamage(view, device, true);
}

// The view's untransformed position in the window, as its layer is laid out.
static PointF untransformedPosition(const View* view)
{
    PointF position = view->geometry().topLeft();
    for (const View* ancestor = view->parent(); ancestor; ancestor = ancestor->parent())
        position += ancestor->geometry().topLeft();
    return position;
}

static SkRect untransformedDeviceRect(const View* view, const RectF& rect, double scale)
{
    const PointF position = untransformedPosition(view);
    return SkRect::MakeXYWH(SkScalar((position.x + rect.x) * scale), SkScalar((position.y + rect.y) * scale),
                            SkScalar(rect.width * scale), SkScalar(rect.height * scale));
}

void Window::damageLayer(View* view, const RectF& rect)
{
    view->m_layer->damage.op(untransformedDeviceRect(view, rect, m_scale).roundOut(), SkRegion::kUnion_Op);
}

bool Window::scrollLayer(View* view, const RectF& rect, double dx, double dy)
{
    Layer* layer = view->m_layer.get();
    if (!layer || !layer->surface || layer->repaintAll || layer->bounds.isEmpty())
        return false;
    const double deviceX = dx * m_scale;
    const double deviceY = dy * m_scale;
    const int moveX = int(std::lround(deviceX));
    const int moveY = int(std::lround(deviceY));
    if (std::abs(deviceX - moveX) > 1e-3 || std::abs(deviceY - moveY) > 1e-3)
        return false;

    // Only pixels wholly inside the rect move; partly covered edge pixels
    // are painted again.
    const SkRect exact = untransformedDeviceRect(view, rect, m_scale);
    const SkIRect area = intersection(exact.roundIn(), layer->bounds);
    const SkIRect kept = intersection(area, area.makeOffset(moveX, moveY));
    if (kept.isEmpty())
        return false;
    layer->scrolls.push_back({area, moveX, moveY});

    SkRegion waiting = layer->damage;
    waiting.op(area, SkRegion::kIntersect_Op);
    waiting.translate(moveX, moveY);
    waiting.op(area, SkRegion::kIntersect_Op);
    layer->damage.op(area, SkRegion::kDifference_Op);
    layer->damage.op(waiting, SkRegion::kUnion_Op);
    SkRegion uncovered(exact.roundOut());
    uncovered.op(kept, SkRegion::kDifference_Op);
    layer->damage.op(uncovered, SkRegion::kUnion_Op);

    if (!view->m_compositeDirty)
        damageRect(view, rect);
    scheduleFrame();
    return true;
}

void Window::collectDamage(View* view, PointF origin)
{
    // A hidden view stays dirty and is damaged once shown.
    if (!view->m_visible)
        return;
    if (view->m_dirty || view->m_compositeDirty) {
        if (view->m_dirty && view->m_layer)
            view->m_layer->repaintAll = true;
        addDamage(view, placedRect(view, origin, false), false);
        view->m_dirty = false;
        view->m_compositeDirty = false;
    }
    const PointF position = origin + view->m_geometry.topLeft();
    for (const auto& child : view->m_children)
        collectDamage(child.get(), position);
}

void Window::damageWhole()
{
    m_frameDamage->full = true;
    for (FrameDamage::Watch& watch : m_frameDamage->watches)
        ++watch.changes;
}

uint64_t Window::watchDamage(const View* watcher, const View* subtree, const SkIRect& rect)
{
    for (FrameDamage::Watch& watch : m_frameDamage->watches) {
        if (watch.watcher != watcher)
            continue;
        if (watch.subtree != subtree) {
            watch.subtree = subtree;
            ++watch.changes;
        }
        watch.rect = rect;
        return watch.changes;
    }
    m_frameDamage->watches.push_back({watcher, subtree, rect});
    return 0;
}

void Window::unwatchDamage(const View* watcher)
{
    std::erase_if(m_frameDamage->watches, [watcher](const FrameDamage::Watch& watch) {
        return watch.watcher == watcher;
    });
}

void Window::damageAll()
{
    damageWhole();
    const auto markLayers = [](const auto& self, View* view) -> void {
        if (view->m_layer)
            view->m_layer->repaintAll = true;
        for (const auto& child : view->m_children)
            self(self, child.get());
    };
    if (m_contentView)
        markLayers(markLayers, m_contentView.get());
    if (m_overlay)
        markLayers(markLayers, m_overlay.get());
    if (m_toolTip)
        markLayers(markLayers, m_toolTip.get());
    scheduleFrame();
}

void Window::updateLayers(View* view, PointF origin, SkCanvas* target, const SkIRect* visible)
{
    if (!view->m_visible || view->m_opacity <= 0)
        return;
    // Inner textures first: an outer one draws them.
    const PointF position = origin + view->m_geometry.topLeft();
    SkIRect inside;
    const SkIRect* childVisible = nullptr;
    if (visible && !view->needsLayer()) {
        inside = *visible;
        if (view->m_clipsChildren)
            inside = intersection(inside, clipBounds(view, position, m_scale));
        childVisible = &inside;
    }
    for (const auto& child : view->m_children)
        updateLayers(child.get(), position, target, childVisible);
    if (!view->needsLayer()) {
        view->m_layer.reset();
        return;
    }

    if (!view->m_layer)
        view->m_layer = std::make_unique<Layer>();
    Layer& layer = *view->m_layer;
    // Moves apply only to the texture they were asked of.
    const std::vector<Layer::Scroll> scrolls = std::move(layer.scrolls);
    layer.scrolls.clear();
    const SkIRect bounds = subtreeRect(view, origin, false);
    if (bounds.isEmpty()) {
        layer.surface.reset();
        layer.bounds = bounds;
        return;
    }
    // Clipped away whole, as rows scrolled out of a list: painting it would
    // show nothing. The texture is kept for when it comes back, but drawn
    // nowhere meanwhile.
    if (visible && !SkIRect::Intersects(mapThroughTransform(view, bounds, position), *visible)) {
        layer.bounds = SkIRect::MakeEmpty();
        return;
    }
    // A texture holding the bounds is kept while it is not much larger, so a
    // view resized every frame does not get a new one every frame.
    const bool fits = layer.surface && layer.surface->width() >= bounds.width()
            && layer.surface->height() >= bounds.height()
            && int64_t(layer.surface->width()) * layer.surface->height()
                    <= 2 * int64_t(bounds.width()) * bounds.height();
    if (!fits || layer.surface->recordingContext() != target->recordingContext()) {
        // Room to grow once the size has changed.
        const bool resized = layer.surface != nullptr;
        const auto room = [resized](int size) { return resized ? size + std::max(32, size / 8) : size; };
        // A transparent texture needs greyscale text.
        const bool opaque = view->m_opaqueLayer;
        const SkSurfaceProps properties = Application::instance()->text().surfaceProps(opaque);
        const SkImageInfo info = target->imageInfo()
                                     .makeWH(room(bounds.width()), room(bounds.height()))
                                     .makeAlphaType(opaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
        layer.surface = target->makeSurface(info, &properties);
        layer.repaintAll = true;
        if (!layer.surface)
            return;
    }
    if (bounds != layer.bounds) {
        layer.bounds = bounds;
        layer.repaintAll = true;
    }
    if (!layer.repaintAll) {
        SkCanvas* canvas = layer.surface->getCanvas();
        for (const Layer::Scroll& scroll : scrolls) {
            const SkIRect destination = intersection(scroll.rect, scroll.rect.makeOffset(scroll.dx, scroll.dy))
                                                .makeOffset(-bounds.x(), -bounds.y());
            const sk_sp<SkImage> pixels = layer.surface->makeImageSnapshot(
                destination.makeOffset(-scroll.dx, -scroll.dy));
            if (!pixels) {
                layer.repaintAll = true;
                break;
            }
            SkPaint copy;
            copy.setBlendMode(SkBlendMode::kSrc);
            canvas->drawImage(pixels, SkScalar(destination.x()), SkScalar(destination.y()), SkSamplingOptions(),
                              &copy);
        }
    }
    if (layer.repaintAll) {
        layer.damage.setRect(bounds);
        layer.repaintAll = false;
    } else {
        layer.damage.op(bounds, SkRegion::kIntersect_Op);
    }
    if (layer.damage.isEmpty())
        return;

    SkRegion clip = layer.damage;
    clip.translate(-bounds.x(), -bounds.y());
    // What an opaque view leaves unpainted shows the window's background,
    // as it would without the texture.
    const SkColor cleared = view->m_opaqueLayer ? SkColor(backgroundColor().argb()) : SK_ColorTRANSPARENT;
    repaint(layer.surface->getCanvas(), clip, [&](SkCanvas* canvas) {
        canvas->clear(cleared);
        canvas->save();
        canvas->translate(SkScalar(-bounds.x()), SkScalar(-bounds.y()));
        Painter painter(canvas, m_scale);
        painter.setDamage(&layer.damage);
        painter.translate(origin);
        view->paintContent(painter, &layer.damage);
        canvas->restore();
    });
    layer.damage.setEmpty();
}

void View::paintFlat(View* view, Painter& painter)
{
    if (!view->isVisible() || view->opacity() <= 0)
        return;
    painter.save();
    painter.multiplyOpacity(view->opacity());
    painter.translate(view->geometry().topLeft());
    if (view->clipsChildren())
        painter.clip(view->localRect());
    view->paint(painter);
    for (const auto& child : view->children())
        paintFlat(child.get(), painter);
    painter.restore();
}

Bitmap View::grab(double scale)
{
    if (scale <= 0)
        scale = window() ? window()->scale() : 1.0;
    const int width = int(std::ceil(m_geometry.width * scale));
    const int height = int(std::ceil(m_geometry.height * scale));
    Bitmap bitmap(width, height, Bitmap::Format::Argb32Premultiplied);
    if (bitmap.isNull())
        return bitmap;
    const SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    const SkSurfaceProps properties = Application::instance()->text().surfaceProps(false);
    const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, bitmap.bits(), bitmap.stride(), &properties);
    if (!surface)
        return {};
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    Painter painter(surface->getCanvas(), scale);
    painter.translate({-m_geometry.x, -m_geometry.y});
    paintFlat(this, painter);
    return bitmap;
}

void View::paintTree(Painter& painter, const SkRegion* region)
{
    if (!m_visible || m_opacity <= 0)
        return;
    if (!m_layer) {
        painter.save();
        painter.multiplyOpacity(m_opacity);
        paintContent(painter, region);
        painter.restore();
        return;
    }

    if (!m_layer->surface || m_layer->bounds.isEmpty())
        return;
    const double scale = painter.scale();
    const PointF position = painter.offset() + m_geometry.topLeft();
    const SkMatrix matrix = layerMatrix(this, position, scale);
    const SkIRect& bounds = m_layer->bounds;
    if (region && !region->intersects(matrix.mapRect(SkRect::Make(bounds)).roundOut().makeOutset(1, 1)))
        return;
    SkCanvas* canvas = painter.canvas();
    SkPaint paint;
    paint.setAlphaf(float(painter.opacity() * m_opacity));
    canvas->save();
    canvas->concat(matrix);
    // At rest the texture lands pixel for pixel; scaled, it filters.
    const SkSamplingOptions sampling = m_transform.scale == 1 ? SkSamplingOptions()
                                                              : SkSamplingOptions(SkFilterMode::kLinear);
    // The texture may be larger than what it holds; filtering stays inside.
    const SkCanvas::SrcRectConstraint constraint = m_transform.scale == 1 ? SkCanvas::kFast_SrcRectConstraint
                                                                          : SkCanvas::kStrict_SrcRectConstraint;
    canvas->drawImageRect(m_layer->surface->makeImageSnapshot(), SkRect::MakeIWH(bounds.width(), bounds.height()),
                          SkRect::Make(bounds), sampling, &paint, constraint);
    canvas->restore();
}

void View::paintContent(Painter& painter, const SkRegion* region)
{
    painter.save();
    painter.translate(m_geometry.topLeft());
    if (m_clipsChildren) {
        if (region && !region->intersects(toSkIRect(painter.deviceRect(localRect())))) {
            painter.restore();
            return;
        }
        if (m_childClipRadius > 0)
            painter.clipRoundedRect(localRect(), m_childClipRadius);
        else
            painter.clip(localRect());
    }
    if (!region || region->intersects(toSkIRect(painter.deviceRect(visualRect())).makeOutset(1, 1)))
        paint(painter);
    for (const auto& child : m_children)
        child->paintTree(painter, region);
    painter.restore();
}

void Window::paintContent(SkCanvas* canvas, const SkRegion* region)
{
    const SkIRect window = SkIRect::MakeWH(canvas->imageInfo().width(), canvas->imageInfo().height());
    updateLayers(m_contentView.get(), {}, canvas, &window);
    if (m_overlay)
        updateLayers(m_overlay.get(), {}, canvas, &window);
    if (m_toolTip)
        updateLayers(m_toolTip.get(), {}, canvas, &window);

    const auto paintInside = [&](SkCanvas* target) {
        target->clear(backgroundColor().argb());
        Painter painter(target, m_scale);
        if (region)
            painter.setDamage(region);
        m_contentView->paintTree(painter, region);
        if (m_overlay)
            m_overlay->paintTree(painter, region);
        if (m_toolTip)
            m_toolTip->paintTree(painter, region);
    };
    if (region)
        repaint(canvas, *region, paintInside);
    else
        paintInside(canvas);
}

void Window::applyTextTarget() const
{
    Application* application = Application::instance();
    application->text().setTranslucentSubpixel(m_translucentSubpixel);
    // Embedded windows already draw into canvases without a pixel geometry
    // when they are not opaque.
    application->text().setTransparentWindow(m_backgroundColor && backgroundColor().a == 0
                                             && application->platform() != Application::Platform::Embedded);
}

bool Window::hasPendingDamage() const
{
    return m_frameDamage->full || !m_frameDamage->pending.isEmpty();
}

std::vector<Rect> Window::paintFrame(SkCanvas* canvas, int bufferAge)
{
    applyTextTarget();
    FrameDamage& damage = *m_frameDamage;
    const SkIRect window = SkIRect::MakeWH(canvas->imageInfo().width(), canvas->imageInfo().height());
    if (damage.full)
        damage.pending.setRect(window);
    if (damage.pending.computeRegionComplexity() > kMaximumDamageRects)
        damage.pending.setRect(damage.pending.getBounds());

    // The buffer shows the frame `bufferAge` frames back: repaint what
    // changed since then too.
    SkRegion repaint = damage.pending;
    if (bufferAge <= 0 || size_t(bufferAge) > damage.history.size() + 1) {
        repaint.setRect(window);
    } else {
        for (int i = 0; i < bufferAge - 1; ++i)
            repaint.op(damage.history[size_t(i)], SkRegion::kUnion_Op);
    }
    if (repaint.computeRegionComplexity() > kMaximumDamageRects)
        repaint.setRect(repaint.getBounds());
    paintContent(canvas, &repaint);

    std::vector<Rect> rects;
    for (SkRegion::Iterator it(damage.pending); !it.done(); it.next()) {
        const SkIRect& rect = it.rect();
        rects.push_back({rect.x(), rect.y(), rect.width(), rect.height()});
    }
    // An unchanged frame still commits, for the frame callback; one pixel
    // says nothing else moved.
    if (rects.empty())
        rects.push_back({0, 0, 1, 1});
    damage.history.push_front(damage.pending);
    if (damage.history.size() > kDamageHistory)
        damage.history.pop_back();
    damage.pending.setEmpty();
    damage.full = false;
    return rects;
}

} // namespace cutefish::ui
