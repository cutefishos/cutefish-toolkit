// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/scroll_view.h"

#include <algorithm>

#include "ui/scroll_bar.h"
#include "ui/scroller.h"
#include "ui/window.h"

namespace cutefish::ui {

// Connects the scroll view to its scroller, which needs the window and so
// is made on the first scroll.
struct ScrollView::Private final : Scroller::Target
{
    explicit Private(ScrollView* view)
        : view(view)
    {
    }

    Scroller::Axis axis(bool vertical) const
    {
        const double viewport = vertical ? view->geometry().height : view->geometry().width;
        const double content = vertical ? view->m_contentSize.height : view->m_contentSize.width;
        const bool scrolls = vertical ? view->m_vertical : view->m_horizontal;
        return {0, std::max(0.0, content - viewport), viewport, scrolls && content > viewport};
    }

    Scroller::Axis horizontalAxis() const override { return axis(false); }
    Scroller::Axis verticalAxis() const override { return axis(true); }
    PointF position() const override { return view->m_position; }
    void applyPosition(PointF position) override { view->applyPosition(position); }

    void movingChanged(bool moving) override
    {
        view->m_verticalBar->setMoving(moving);
        view->m_horizontalBar->setMoving(moving);
    }

    Scroller* scroller()
    {
        // Content that moves to another window, as a drop-down's between
        // openings, needs a scroller on the frames of the new one.
        if (instance && instance->window() != view->window())
            instance.reset();
        if (!instance && view->window()) {
            instance = std::make_unique<Scroller>(view->window(), this);
            instance->setElastic(elastic);
            instance->setPaged(paged);
        }
        return instance.get();
    }

    ScrollView* view;
    std::unique_ptr<Scroller> instance;
    bool elastic = true;
    bool paged = false;
};

ScrollView::ScrollView()
    : d(std::make_unique<Private>(this))
    , m_verticalBar(addChild<ScrollBar>(ScrollBar::Orientation::Vertical))
    , m_horizontalBar(addChild<ScrollBar>(ScrollBar::Orientation::Horizontal))
    , m_verticalBarTop(ScrollBar::inset)
    , m_verticalBarBottom(ScrollBar::inset)
{
    setClipsChildren(true);
    m_verticalBar->onPositionRequested = [this](double y) {
        setScrollPosition({m_position.x, y});
    };
    m_horizontalBar->onPositionRequested = [this](double x) {
        setScrollPosition({x, m_position.y});
    };
}

ScrollView::~ScrollView() = default;

void ScrollView::setContentView(std::unique_ptr<View> view)
{
    if (m_content)
        takeChild(m_content);
    m_content = view.get();
    addChild(std::move(view));
    if (m_barsBelow) {
        raiseChild(m_content);
    } else {
        raiseChild(m_verticalBar);
        raiseChild(m_horizontalBar);
    }
}

void ScrollView::setScrollDirections(bool horizontal, bool vertical)
{
    if (horizontal == m_horizontal && vertical == m_vertical)
        return;
    m_horizontal = horizontal;
    m_vertical = vertical;
    requestLayout();
}

void ScrollView::setElastic(bool elastic)
{
    d->elastic = elastic;
    if (d->instance)
        d->instance->setElastic(elastic);
}

void ScrollView::setVerticalBarPadding(double top, double bottom)
{
    m_verticalBarTop = top;
    m_verticalBarBottom = bottom;
    requestLayout();
}

void ScrollView::setCornerRadius(double radius)
{
    if (radius == m_cornerRadius)
        return;
    m_cornerRadius = radius;
    setChildClipRadius(radius);
    requestLayout();
}

void ScrollView::setVerticalBarPolicy(BarPolicy policy)
{
    if (policy == m_verticalPolicy)
        return;
    m_verticalPolicy = policy;
    m_verticalBar->setAlwaysShown(policy == BarPolicy::AlwaysOn);
    requestLayout();
}

void ScrollView::setBarsBelowContent(bool below)
{
    m_barsBelow = below;
    if (below) {
        if (m_content)
            raiseChild(m_content);
    } else {
        raiseChild(m_verticalBar);
        raiseChild(m_horizontalBar);
    }
    requestPaint();
}

void ScrollView::setPaged(bool paged)
{
    d->paged = paged;
    if (d->instance)
        d->instance->setPaged(paged);
}

PointF ScrollView::maximumPosition() const
{
    return {std::max(0.0, m_contentSize.width - geometry().width),
            std::max(0.0, m_contentSize.height - geometry().height)};
}

void ScrollView::setScrollPosition(PointF position)
{
    const PointF maximum = maximumPosition();
    if (d->instance)
        d->instance->cancel(false);
    applyPosition({std::clamp(position.x, 0.0, maximum.x), std::clamp(position.y, 0.0, maximum.y)});
}

void ScrollView::ensureVisible(const RectF& rect)
{
    PointF position = m_position;
    const SizeF viewport = geometry().size();
    if (rect.y < position.y)
        position.y = rect.y;
    else if (rect.bottom() > position.y + viewport.height)
        position.y = rect.bottom() - viewport.height;
    if (rect.x < position.x)
        position.x = rect.x;
    else if (rect.right() > position.x + viewport.width)
        position.x = rect.right() - viewport.width;
    if (position != m_position)
        setScrollPosition(position);
}

void ScrollView::applyPosition(PointF position)
{
    m_position = position;
    if (m_content)
        m_content->setGeometry({-position.x, -position.y, m_contentSize.width, m_contentSize.height});
    updateBars();
    positionChanged();
    if (onScrolled)
        onScrolled();
    scrolled.emit();
}

void ScrollView::updateBars()
{
    const RectF bounds = localRect();
    m_verticalBar->setRange(bounds.height, m_contentSize.height, m_position.y);
    m_horizontalBar->setRange(bounds.width, m_contentSize.width, m_position.x);
}

void ScrollView::layout()
{
    const RectF bounds = localRect();
    if (m_content) {
        const LayoutHints& hints = m_content->layoutHints();
        const SizeF preferred = m_content->preferredSize();
        double width = m_horizontal ? preferred.width : bounds.width;
        if (m_horizontal && hints.fillWidth)
            width = std::max(width, bounds.width);
        double height = m_vertical ? m_content->heightForWidth(width) : bounds.height;
        if (m_vertical && hints.fillHeight)
            height = std::max(height, bounds.height);
        m_contentSize = {width, height};
    } else {
        m_contentSize = {};
    }

    m_verticalBar->setGeometry({bounds.width - ScrollBar::thickness, 0, ScrollBar::thickness,
                                bounds.height});
    m_horizontalBar->setGeometry({0, bounds.height - ScrollBar::thickness, bounds.width,
                                  ScrollBar::thickness});
    m_verticalBar->setVisible(m_vertical && m_verticalPolicy != BarPolicy::AlwaysOff);

    // Where a handle's rounded end keeps the inset to a rounded corner.
    const double corner = std::max(ScrollBar::inset, m_cornerRadius - ScrollBar::thickness / 2 + ScrollBar::inset);
    m_verticalBar->setPadding(std::max(m_verticalBarTop, corner), std::max(m_verticalBarBottom, corner));
    m_horizontalBar->setPadding(corner, corner);
    m_horizontalBar->setVisible(m_horizontal);

    // A resize keeps the position inside the content, unless it is
    // overscrolled on purpose by a gesture.
    PointF position = m_position;
    if (!d->instance || !d->instance->isActive()) {
        const PointF maximum = maximumPosition();
        position = {std::clamp(position.x, 0.0, maximum.x), std::clamp(position.y, 0.0, maximum.y)};
    }
    applyPosition(position);
}

bool ScrollView::scroll(const ScrollEvent& event)
{
    Scroller* scroller = d->scroller();
    return scroller && scroller->handleScroll(event);
}

} // namespace cutefish::ui
