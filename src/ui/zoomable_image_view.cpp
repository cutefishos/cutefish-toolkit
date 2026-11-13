// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/zoomable_image_view.h"

#include <algorithm>
#include <cmath>

#include "ui/animated_image.h"
#include "ui/animation.h"
#include "ui/image_view.h"
#include "ui/window.h"

namespace cutefish::ui {

// The largest texture to ask for; a 50 MP photo stays inside common GPU limits.
static SizeF cappedSize(double width, double height)
{
    const double scale = std::min(1.0, 8192 / std::max(width, height));
    return {std::round(width * scale), std::round(height * scale)};
}

// The scrolled content: the frame, centred while smaller than the view,
// and its two layers.
class ZoomableImageView::Canvas : public View
{
public:
    explicit Canvas(ZoomableImageView* view)
        : m_view(view)
    {
        fit = addChild<ImageView>();
        fit->setFillMode(ImageView::FillMode::PreserveAspectFit);
        fit->setAutoTransform(true);
        fit->onStatusChanged = [this](ImageView::Status) {
            if (m_view->onStatusChanged)
                m_view->onStatusChanged();
        };
        detail = addChild<ImageView>();
        detail->setFillMode(ImageView::FillMode::PreserveAspectFit);
        detail->setAutoTransform(true);
        detail->setVisible(false);
        detail->onStatusChanged = [this](ImageView::Status) { requestLayout(); };
    }

    SizeF frameSize() const
    {
        return {m_view->naturalWidth() * m_view->m_zoom, m_view->naturalHeight() * m_view->m_zoom};
    }

    SizeF sizeHint() const override
    {
        const SizeF frame = frameSize();
        const RectF view = m_view->geometry();
        return {std::max(view.width, frame.width), std::max(view.height, m_view->m_topInset + frame.height)};
    }

    bool acceptsPointer() const override { return true; }

    ImageView* fit;
    ImageView* detail;

    // GIF, APNG and animated WebP play as an AnimatedImage instead.
    void setAnimated(const std::string& path)
    {
        m_animatedPath = path;
        m_animated.reset();
        loadAnimated();
    }

    bool animatedLoading() const
    {
        return !m_animatedPath.empty() && (!m_animated || !m_animated->isLoaded()) && !m_animatedFailed;
    }
    bool animatedFailed() const { return m_animatedFailed; }

protected:
    void paint(Painter& painter) override
    {
        if (m_animated && m_animated->isLoaded())
            m_animated->draw(painter, fit->geometry());
    }

    void layout() override
    {
        const SizeF frame = frameSize();
        const RectF rect {m_view->snap(m_view->frameX(m_view->m_zoom)),
                              m_view->snap(m_view->frameY(m_view->m_zoom)), frame.width, frame.height};
        fit->setGeometry(rect);
        detail->setGeometry(rect);
        loadAnimated();
        detail->setVisible(m_view->isZoomed() && detail->status() == ImageView::Status::Ready);
    }

    void pointerPress(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Right && m_view->onContextMenuRequested) {
            m_view->onContextMenuRequested(mapToWindow(event.position));
            return;
        }
        if (event.clickCount == 2) {
            m_view->toggleActualSize(viewPoint(event.position));
            return;
        }
        m_panning = m_view->isZoomed();
        if (!m_panning)
            return;
        if (m_view->m_animation)
            m_view->m_animation->stop();
        m_view->m_anchored = false;
        m_start = mapToWindow(event.position);
        m_startPan = m_view->scrollPosition();
        setCursorShape(CursorShape::Grabbing);
    }

    void pointerMove(const PointerEvent& event) override
    {
        if (!m_panning)
            return;
        const PointF now = mapToWindow(event.position);
        m_view->setPan(m_startPan.x - (now.x - m_start.x), m_startPan.y - (now.y - m_start.y));
    }

    void pointerRelease(const PointerEvent&) override
    {
        m_panning = false;
        updateCursor();
    }

public:
    void updateCursor()
    {
        if (!m_panning)
            setCursorShape(m_view->isZoomed() ? CursorShape::Grab : CursorShape::Default);
    }

private:
    void loadAnimated()
    {
        if (m_animatedPath.empty() || m_animated || !window())
            return;
        m_animatedFailed = false;
        m_animated = std::make_unique<AnimatedImage>(window());
        m_animated->onLoaded = [this](bool ok) {
            m_animatedFailed = !ok;
            if (ok)
                m_animated->setPlaying(true);
            requestPaint();
            if (m_view->onStatusChanged)
                m_view->onStatusChanged();
        };
        m_animated->onFrameChanged = [this] { requestPaint(); };
        m_animated->load(m_animatedPath);
    }

    std::string m_animatedPath;
    std::unique_ptr<AnimatedImage> m_animated;
    bool m_animatedFailed = false;

    PointF viewPoint(PointF content) const
    {
        const PointF pan = m_view->scrollPosition();
        return {content.x - pan.x, content.y - pan.y};
    }

    ZoomableImageView* m_view;
    bool m_panning = false;
    PointF m_start;
    PointF m_startPan;
};

ZoomableImageView::ZoomableImageView()
{
    setScrollDirections(true, true);
    setClipsChildren(true);
    setTracksHover(true);
    m_canvas = setContent<Canvas>(this);

    // A vector image is drawn again at the zoom it comes to rest at.
    m_vectorTimer.setSingleShot(true);
    m_vectorTimer.setInterval(std::chrono::milliseconds(150));
    m_vectorTimer.onTimeout = [this] {
        if (isZoomed()) {
            const SizeF frame = upright(m_canvas->frameSize());
            const SizeF size = cappedSize(std::ceil(frame.width * m_ratio), std::ceil(frame.height * m_ratio));
            m_canvas->detail->setSourceSize({size.width / m_ratio, size.height / m_ratio});
        }
    };

    // The fitted decode grows with the view once a resize comes to rest.
    m_refitTimer.setSingleShot(true);
    m_refitTimer.setInterval(std::chrono::milliseconds(150));
    m_refitTimer.onTimeout = [this] {
        if (!m_fitted)
            return;
        const SizeF frame = upright(m_canvas->frameSize());
        if (std::ceil(frame.width * m_ratio) > std::ceil(m_fitSource.width * m_ratio)
            || std::ceil(frame.height * m_ratio) > std::ceil(m_fitSource.height * m_ratio)) {
            m_fitSource = frame;
            m_canvas->fit->setSourceSize(frame);
        }
    };
}

ZoomableImageView::~ZoomableImageView() = default;

void ZoomableImageView::setImage(const std::string& path, const ImageInfo& info, SizeF prepared, double ratio)
{
    m_path = path;
    m_info = info;
    m_ratio = ratio > 0 ? ratio : 1;
    m_quarterTurns = 0;
    m_canvas->fit->setQuarterTurns(0);
    m_canvas->detail->setQuarterTurns(0);
    m_canvas->detail->setSource({});
    resetZoom();
    // The size first, so the new source is decoded once.
    m_fitSource = prepared.width > 0 ? SizeF {prepared.width / m_ratio, prepared.height / m_ratio}
                                     : geometry().size();
    m_canvas->fit->setSourceSize(m_fitSource);
    m_canvas->fit->setSource(info.animated ? std::string() : path);
    m_canvas->setAnimated(info.animated ? path : std::string());
    m_canvas->requestLayout();
    requestLayout();
}

bool ZoomableImageView::isLoading() const
{
    if (m_info.animated)
        return m_canvas->animatedLoading();
    return m_canvas->fit->status() == ImageView::Status::Loading;
}

bool ZoomableImageView::hasFailed() const
{
    if (m_info.animated)
        return m_canvas->animatedFailed();
    return m_canvas->fit->status() == ImageView::Status::Error;
}

PointF ZoomableImageView::imagePoint(PointF at) const
{
    const PointF pan = scrollPosition();
    return {(at.x + pan.x - frameX(m_zoom)) / m_zoom * m_ratio, (at.y + pan.y - frameY(m_zoom)) / m_zoom * m_ratio};
}

double ZoomableImageView::naturalWidth() const
{
    if (m_info.width <= 0 || m_info.height <= 0)
        return std::max(1.0, geometry().width);
    return (m_quarterTurns % 2 ? m_info.height : m_info.width) / m_ratio;
}

double ZoomableImageView::naturalHeight() const
{
    if (m_info.width <= 0 || m_info.height <= 0)
        return std::max(1.0, geometry().height);
    return (m_quarterTurns % 2 ? m_info.width : m_info.height) / m_ratio;
}

void ZoomableImageView::setTopInset(double inset)
{
    if (inset == m_topInset)
        return;
    m_topInset = inset;
    setVerticalBarPadding(inset, 0);
    // A new viewport, as a resize is.
    m_lastSize = {};
    m_canvas->requestLayout();
    requestLayout();
}

void ZoomableImageView::setQuarterTurns(int turns)
{
    turns = ((turns % 4) + 4) % 4;
    if (turns == m_quarterTurns)
        return;
    m_quarterTurns = turns;
    m_canvas->fit->setQuarterTurns(turns);
    m_canvas->detail->setQuarterTurns(turns);
    resetZoom();
}

// The upright size of a frame of the image as shown.
SizeF ZoomableImageView::upright(SizeF frame) const
{
    return m_quarterTurns % 2 ? SizeF {frame.height, frame.width} : frame;
}

double ZoomableImageView::fitZoom() const
{
    const double width = geometry().width;
    const double height = geometry().height - m_topInset;
    if (width <= 0 || height <= 0)
        return 1;
    return std::min({1.0, width / naturalWidth(), height / naturalHeight()});
}

// Where the frame sits in the content at a zoom: centred while smaller than the view.
double ZoomableImageView::frameX(double zoom) const
{
    return std::max(0.0, (geometry().width - naturalWidth() * zoom) / 2);
}

double ZoomableImageView::frameY(double zoom) const
{
    return m_topInset + std::max(0.0, (geometry().height - m_topInset - naturalHeight() * zoom) / 2);
}

double ZoomableImageView::snap(double value) const
{
    return std::round(value * m_ratio) / m_ratio;
}

void ZoomableImageView::setPan(double x, double y)
{
    const double maxX = std::max(0.0, naturalWidth() * m_zoom - geometry().width);
    const double maxY = std::max(0.0, m_topInset + naturalHeight() * m_zoom - geometry().height);
    setScrollPosition({std::clamp(x, 0.0, maxX), std::clamp(y, 0.0, maxY)});
}

void ZoomableImageView::resetZoom()
{
    if (m_animation)
        m_animation->stop();
    m_anchored = false;
    m_fitted = true;
    m_zoom = fitZoom();
    m_canvas->requestLayout();
    requestLayout();
    setPan(0, 0);
    m_canvas->updateCursor();
}

void ZoomableImageView::setZoom(double zoom)
{
    m_zoom = zoom;
    // Laid out now, so the pan below is clamped against the new content.
    m_canvas->requestLayout();
    layout();
    if (m_anchored)
        setPan(frameX(m_zoom) + m_anchorImage.x * m_zoom - m_anchorView.x,
               frameY(m_zoom) + m_anchorImage.y * m_zoom - m_anchorView.y);
    else
        setPan(scrollPosition().x, scrollPosition().y);
    if (isZoomed())
        startDetail();
    if (m_info.vector)
        m_vectorTimer.start();
    m_canvas->updateCursor();
}

// The full-resolution decode, the first time the image is zoomed past fit.
void ZoomableImageView::startDetail()
{
    // A turned image shows its fitted layer until setImage().
    if (!m_canvas->detail->source().empty() || m_info.animated || m_quarterTurns)
        return;
    if (!m_info.vector && m_info.width > 0 && m_info.height > 0) {
        const SizeF size = cappedSize(m_info.width, m_info.height);
        m_canvas->detail->setSourceSize({size.width / m_ratio, size.height / m_ratio});
        m_canvas->detail->setMipmap(true);
    } else if (!m_info.vector) {
        return;
    }
    m_canvas->detail->setSource(m_path);
}

void ZoomableImageView::zoomTo(double target, PointF at, bool animate)
{
    target = std::clamp(target, fitZoom(), maxZoom());
    const PointF pan = scrollPosition();
    m_anchorImage = {std::clamp((at.x + pan.x - frameX(m_zoom)) / m_zoom, 0.0, naturalWidth()),
                     std::clamp((at.y + pan.y - frameY(m_zoom)) / m_zoom, 0.0, naturalHeight())};
    m_anchorView = at;
    m_fitted = target <= fitZoom() + 0.0001;
    if (m_animation)
        m_animation->stop();
    m_anchored = true;
    if (animate && std::abs(target - m_zoom) > 0.0001 && window()) {
        if (!m_animation) {
            m_animation = std::make_unique<Animation>(window());
            m_animation->setDuration(std::chrono::milliseconds(200));
            m_animation->setEasing(Animation::Easing::OutCubic);
            m_animation->onUpdate = [this](double value) {
                setZoom(m_animationFrom + (m_animationTo - m_animationFrom) * value);
            };
        }
        m_animationFrom = m_zoom;
        m_animationTo = target;
        m_animation->start();
    } else {
        setZoom(target);
    }
}

void ZoomableImageView::setZoomAndPosition(double zoom, PointF position)
{
    if (m_animation)
        m_animation->stop();
    zoom = std::clamp(zoom, fitZoom(), maxZoom());
    m_fitted = zoom <= fitZoom() + 0.0001;
    m_anchored = false;
    setZoom(zoom);
    setPan(position.x, position.y);
}

// Steps compound on an animation's target, so quick wheel clicks add up.
void ZoomableImageView::zoomBy(double factor, PointF at, bool animate)
{
    const double base = m_animation && m_animation->isRunning() ? m_animationTo : m_zoom;
    zoomTo(base * factor, at, animate);
}

void ZoomableImageView::toggleActualSize(PointF at)
{
    if (isZoomed())
        zoomTo(fitZoom(), at, true);
    else
        zoomTo(fitZoom() < 1 ? 1 : 2, at, true);
}

void ZoomableImageView::layout()
{
    const SizeF size = geometry().size();
    if (size != m_lastSize) {
        m_lastSize = size;
        if (m_fitted) {
            m_zoom = fitZoom();
            m_canvas->requestLayout();
            if (m_fitSource.width > 0 && m_fitSource.height > 0 && !m_info.animated)
                m_refitTimer.start();
        }
    }
    ScrollView::layout();
}

bool ZoomableImageView::scroll(const ScrollEvent& event)
{
    if (!(event.modifiers & ControlModifier)) {
        // A pan ends a running zoom, or its anchor would pull the view back.
        if (m_animation && m_animation->isRunning()) {
            m_animation->stop();
            setZoom(m_animationTo);
        }
        m_anchored = false;
        return ScrollView::scroll(event);
    }
    if (event.value120Y != 0)
        zoomBy(std::pow(1.25, -event.value120Y / 120.0), m_pointer, true);
    else if (event.deltaY != 0)
        zoomBy(std::pow(1.006, -event.deltaY), m_pointer, false);
    return true;
}

bool ZoomableImageView::gesture(const GestureEvent& event)
{
    if (event.type == GestureEvent::Type::PinchBegin) {
        m_pinchScale = 1;
        return true;
    }
    if (event.type == GestureEvent::Type::PinchUpdate) {
        const double delta = event.scale / m_pinchScale;
        m_pinchScale = event.scale;
        zoomTo(m_zoom * delta, m_pointer, false);
        return true;
    }
    return event.type == GestureEvent::Type::PinchEnd;
}

void ZoomableImageView::hoverMove(const PointerEvent& event)
{
    m_pointer = event.position;
}

} // namespace cutefish::ui
