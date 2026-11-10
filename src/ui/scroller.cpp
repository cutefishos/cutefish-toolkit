// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/scroller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>

#include "base/event_loop.h"
#include "ui/application.h"

namespace cutefish::ui {

static constexpr double friction = 4.5;
static constexpr double springFrequency = 18.0;
static constexpr double rubberBandResistance = 0.55;
static constexpr double minimumFlickSpeed = 40.0;
static constexpr double maximumFlickSpeed = 6000.0;
static constexpr double stopSpeed = 5.0;
// Hover follows a coast again below this speed, with about 22 pixels of it left.
static constexpr double hoverResumeSpeed = 100.0;
static constexpr double velocityTimeout = 80;
// The flick velocity is a line fitted to the last samples of this window.
static constexpr double velocityWindow = 100;
static constexpr size_t velocitySamples = 10;
// Frames predict past the last sample only while samples arrive at a steady rate.
static constexpr double minimumSampleInterval = 2;
static constexpr double maximumSampleInterval = 20;
static constexpr double maximumPrediction = 8;
// A release faster than this turns the page however little it moved.
static constexpr double pageFlickSpeed = 400.0;
static constexpr double snapDuration = 180;
static constexpr double snapPageDuration = 220;
static constexpr double minimumSnapDuration = 120;
static constexpr int wheelStepDuration = 120;
// Three lines of 15 pixels per wheel click.
static constexpr double wheelPixelsPer120 = 45.0 / 120;
// The compositor sends 10 axis units per click.
static constexpr double wheelPixelsPerUnit = 4.5;
static constexpr double never = -std::numeric_limits<double>::infinity();

static double monotonicClock()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return double(now.tv_sec) * 1000 + double(now.tv_nsec) / 1e6;
}

static double (*currentClock)() = monotonicClock;
// Event timestamps come from the compositor's clock; its offset to ours is the
// smallest delivery delay seen.
static double eventClockOffset = 0;
static bool eventClockKnown = false;

static double bounded(double position, double minimum, double maximum)
{
    return std::clamp(position, minimum, std::max(minimum, maximum));
}

// Fuzzy, at 1e-12: the stop conditions were tuned with this comparison.
static bool same(PointF a, PointF b)
{
    return std::abs(a.x - b.x) <= 1e-12 && std::abs(a.y - b.y) <= 1e-12;
}

Scroller::Scroller(Window* window, Target* target)
    : FrameClient(window)
    , m_target(target)
    , m_shownTime(never)
{
}

Scroller::~Scroller()
{
    if (m_wheelStepTimer)
        Application::instance()->loop().stopTimer(m_wheelStepTimer);
}

double Scroller::clock()
{
    return currentClock();
}

void Scroller::setClock(double (*clock)())
{
    currentClock = clock ? clock : monotonicClock;
    eventClockKnown = false;
}

void Scroller::tick(uint64_t frameTimeNs)
{
    m_frameRequested = false;
    advance(double(frameTimeNs) / 1e6);
    if (!m_frameRequested)
        stopTicking();

    bool fast = m_gesture && m_consumed;
    if (!fast && m_coasting) {
        const Axis x = m_target->horizontalAxis();
        const Axis y = m_target->verticalAxis();
        const PointF position = m_target->position();
        // A rubber band's speed crosses zero where it turns round.
        fast = m_paged || position.x != bounded(position.x, x.minimum, x.maximum)
                || position.y != bounded(position.y, y.minimum, y.maximum)
                || std::hypot(m_velocity.x, m_velocity.y) > hoverResumeSpeed;
    }
    if (fast)
        deferHover();
}

void Scroller::interrupt(Interruption reason)
{
    switch (reason) {
    case Interruption::Input:
    case Interruption::Deactivated:
        cancel();
        break;
    case Interruption::FingersDown:
        stopCoast();
        break;
    case Interruption::ScrollBegan:
        // An untouched overscrolled view still settles; the touched view takes over.
        if (isActive())
            settle();
        break;
    }
}

void Scroller::setPosition(PointF position)
{
    m_writing = true;
    m_target->applyPosition(position);
    m_writing = false;
}

void Scroller::viewMoved()
{
    if (!m_writing)
        cancel(false);
}

void Scroller::scheduleFrame()
{
    m_frameRequested = true;
    if (!isTicking())
        startTicking();
}

double Scroller::eventTime(const ScrollEvent& event) const
{
    const double now = clock();
    if (!event.time)
        return now;

    const double timestamp = event.time;
    const double offset = now - timestamp;
    // A jump the other way is a new clock or a wrapped 32-bit timestamp.
    if (!eventClockKnown || offset < eventClockOffset || offset > eventClockOffset + 1000) {
        eventClockOffset = offset;
        eventClockKnown = true;
    }
    return timestamp + eventClockOffset;
}

void Scroller::updateScrolling()
{
    const bool scrolling = (m_gesture && m_consumed) || m_coasting || m_wheelStepTimer;
    if (scrolling == m_scrolling)
        return;
    m_scrolling = scrolling;
    m_target->movingChanged(scrolling);
}

void Scroller::cancel(bool restoreBounds)
{
    reset(restoreBounds);
    updateScrolling();
}

void Scroller::reset(bool restoreBounds)
{
    const bool wasActive = isActive();
    const bool snapping = m_paged && m_coasting;
    const bool moved = m_paged && m_gesture && m_consumed;
    m_gesture = false;
    m_coasting = false;
    m_extrapolated = false;
    m_consumed = false;
    m_velocity = {};
    m_samples.clear();
    m_cancelled |= wasActive;
    if (!restoreBounds || !wasActive)
        return;

    const Axis x = m_target->horizontalAxis();
    const Axis y = m_target->verticalAxis();
    const PointF current = m_target->position();
    if (snapping)
        setPosition(m_snapTarget);
    else if (moved)
        setPosition({nearestPage(current.x, x), nearestPage(current.y, y)});
    else if (!m_paged)
        setPosition({bounded(current.x, x.minimum, x.maximum),
                     bounded(current.y, y.minimum, y.maximum)});
}

void Scroller::settle()
{
    // A page snap is how a paged view settles.
    if (m_paged && m_coasting)
        return;

    const double time = m_shownTime == never ? clock() - resampleLatency : m_shownTime;
    reset(false);
    startCoast(m_target->position(), time);
    updateScrolling();
}

void Scroller::stopCoast()
{
    if (m_coasting)
        settle();
}

double Scroller::dragPosition(double position, const Axis& axis) const
{
    const double edge = bounded(position, axis.minimum, axis.maximum);
    if (!m_elastic || axis.viewport <= 0)
        return edge;

    const double distance = position - edge;
    if (m_paged) {
        // Each pixel past an end gives less, up to a quarter of a page.
        const double limit = axis.viewport / 4;
        return edge + limit * distance / (std::abs(distance) + limit);
    }
    return edge + distance * rubberBandResistance
            / (1.0 + std::abs(distance) * rubberBandResistance / axis.viewport);
}

double Scroller::unboundedPosition(double position, const Axis& axis) const
{
    const double edge = bounded(position, axis.minimum, axis.maximum);
    const double distance = position - edge;
    if (!m_elastic || axis.viewport <= 0)
        return edge;

    if (m_paged) {
        const double limit = axis.viewport / 4;
        return edge + limit * distance / std::max(1.0, limit - std::abs(distance));
    }
    return edge + distance / (rubberBandResistance
                              * std::max(0.01, 1.0 - std::abs(distance) / axis.viewport));
}

double Scroller::pageTarget(double position, double velocity, const Axis& axis) const
{
    if (!axis.enabled || axis.viewport <= 0)
        return position;

    const double page = (position - axis.minimum) / axis.viewport;
    double target = std::round(page);
    if (velocity > pageFlickSpeed)
        target = std::floor(page) + 1;
    else if (velocity < -pageFlickSpeed)
        target = std::ceil(page) - 1;
    return bounded(axis.minimum + target * axis.viewport, axis.minimum, axis.maximum);
}

double Scroller::nearestPage(double position, const Axis& axis) const
{
    return pageTarget(position, 0, axis);
}

PointF Scroller::contentPosition(PointF dragPosition) const
{
    PointF content = m_target->position();
    if (m_dragAxes & Horizontal)
        content.x = this->dragPosition(dragPosition.x, m_target->horizontalAxis());
    if (m_dragAxes & Vertical)
        content.y = this->dragPosition(dragPosition.y, m_target->verticalAxis());
    return content;
}

void Scroller::begin()
{
    m_gesture = true;
    // A page snap runs on until the fingers move.
    m_coasting = m_paged && m_coasting;
    m_extrapolated = false;
    m_consumed = false;
    m_cancelled = false;
    m_velocity = {};
    m_accumulatedDelta = {};
    m_samples.clear();
    m_axes = BothAxes;
    m_dragAxes = NoAxes;
    m_axisChosen = false;
    m_shownTime = never;
    const PointF current = m_target->position();
    m_dragPosition = {unboundedPosition(current.x, m_target->horizontalAxis()),
                      unboundedPosition(current.y, m_target->verticalAxis())};
    m_shownDragPosition = m_dragPosition;
}

void Scroller::addSample(double time)
{
    if (!m_samples.empty())
        time = std::max(time, m_samples.back().time);
    m_samples.push_back({time, m_dragPosition});
    m_extrapolated = false;
    while (m_samples.front().time < time - 2 * velocityWindow)
        m_samples.erase(m_samples.begin());

    if (window())
        scheduleFrame();
    else
        advance(time + resampleLatency);
}

PointF Scroller::resampled(double time, bool extrapolate, bool* extrapolated) const
{
    if (extrapolated)
        *extrapolated = false;
    const Sample& last = m_samples.back();
    if (time >= last.time) {
        if (!extrapolate || time == last.time || m_samples.size() < 2)
            return last.position;
        const Sample& previous = m_samples[m_samples.size() - 2];
        const double interval = last.time - previous.time;
        if (interval < minimumSampleInterval || interval > maximumSampleInterval)
            return last.position;
        const double ahead = std::min(time - last.time, std::min(interval / 2, maximumPrediction));
        if (extrapolated)
            *extrapolated = true;
        return last.position + (last.position - previous.position) * (ahead / interval);
    }

    for (size_t i = m_samples.size() - 1; i > 0; --i) {
        const Sample& before = m_samples[i - 1];
        if (before.time > time)
            continue;
        const Sample& after = m_samples[i];
        const double fraction = (time - before.time) / (after.time - before.time);
        return before.position + (after.position - before.position) * fraction;
    }
    return m_samples.front().position;
}

PointF Scroller::flickVelocity() const
{
    const double end = m_samples.back().time;
    size_t first = m_samples.size() - 1;
    while (first > 0 && m_samples.size() - first < velocitySamples
           && end - m_samples[first - 1].time <= velocityWindow) {
        --first;
    }

    // A least-squares line is steadier than the last interval with
    // millisecond timestamps and uneven event delivery.
    const double count = double(m_samples.size() - first);
    double meanTime = 0;
    PointF meanPosition;
    for (size_t i = first; i < m_samples.size(); ++i) {
        meanTime += m_samples[i].time / count;
        meanPosition += contentPosition(m_samples[i].position) / count;
    }
    double timeVariance = 0;
    PointF covariance;
    for (size_t i = first; i < m_samples.size(); ++i) {
        const double time = m_samples[i].time - meanTime;
        timeVariance += time * time;
        covariance += (contentPosition(m_samples[i].position) - meanPosition) * time;
    }
    if (timeVariance <= 0)
        return {};

    const auto limit = [](double velocity) {
        return std::abs(velocity) < minimumFlickSpeed
                ? 0.0
                : bounded(velocity, -maximumFlickSpeed, maximumFlickSpeed);
    };
    const PointF velocity = covariance * (1000.0 / timeVariance);
    return {limit(velocity.x), limit(velocity.y)};
}

bool Scroller::handleScroll(const ScrollEvent& event)
{
    struct Notify
    {
        Scroller* scroller;
        ~Notify() { scroller->updateScrolling(); }
    } notify{this};

    const double time = eventTime(event);
    // Deltas here point the way the fingers drag the content: away from
    // the end the event's deltas point to.
    if (!event.precise) {
        // A wheel is left to the paged view's own paging.
        if (m_paged)
            return false;
        const bool clicks = event.value120X || event.value120Y;
        const PointF delta = clicks ? PointF{-event.value120X * wheelPixelsPer120,
                                             -event.value120Y * wheelPixelsPer120}
                                    : PointF{-event.deltaX, -event.deltaY} * wheelPixelsPerUnit;
        return handleDelta(delta, time, true, event.modifiers);
    }

    // A touchpad gesture has no begin of its own: the first finger event starts
    // it, and the stop ends it.
    if (event.stop) {
        m_inputGesture = false;
        if (!m_gesture)
            return false;
        const bool consumed = m_consumed;
        release(time);
        return consumed;
    }
    if (!m_inputGesture) {
        m_inputGesture = true;
        begin();
    } else if (!m_gesture && !m_cancelled) {
        begin();
    }
    if (m_cancelled)
        return true;
    return handleDelta({-event.deltaX, -event.deltaY}, time, false, event.modifiers);
}

bool Scroller::handleDelta(PointF delta, double time, bool wheel, uint32_t modifiers)
{
    const Axis x = m_target->horizontalAxis();
    const Axis y = m_target->verticalAxis();

    if (wheel) {
        cancel();
        if ((modifiers & ShiftModifier) || (!y.enabled && x.enabled && delta.x == 0)) {
            delta.x = delta.y;
            delta.y = 0;
        }
        if (modifiers & ControlModifier) {
            delta.x = delta.x == 0 ? 0 : std::copysign(x.viewport, delta.x);
            delta.y = delta.y == 0 ? 0 : std::copysign(y.viewport, delta.y);
        }
    }

    if (m_gesture && !m_axisChosen) {
        m_accumulatedDelta += delta;
        const double dx = std::abs(m_accumulatedDelta.x);
        const double dy = std::abs(m_accumulatedDelta.y);
        if (std::max(dx, dy) >= 6) {
            if (m_paged)
                m_axes = dx > dy ? Horizontal : Vertical;
            else if (dx > 2.5 * dy)
                m_axes = Horizontal;
            else if (dy > 2.5 * dx)
                m_axes = Vertical;
            m_axisChosen = true;
            // A pager moves only once the direction is known, then by all of it.
            if (m_paged) {
                delta = m_accumulatedDelta;
                // The seed took the first movement on both axes; keep the pager's.
                if (!m_samples.empty()) {
                    PointF& seed = m_samples.front().position;
                    if (!x.enabled || m_axes != Horizontal)
                        seed.x = m_dragPosition.x;
                    if (!y.enabled || m_axes != Vertical)
                        seed.y = m_dragPosition.y;
                }
            }
        } else if (m_paged) {
            // Frames then ease into the held-back movement instead of jumping to it.
            if (m_samples.empty() && !m_coasting && !delta.isNull())
                m_samples.push_back({time, m_dragPosition - delta});
            return false;
        }
    }
    if (!x.enabled || (m_gesture && !(m_axes & Horizontal)))
        delta.x = 0;
    if (!y.enabled || (m_gesture && !(m_axes & Vertical)))
        delta.y = 0;

    if (delta.isNull())
        return m_gesture && m_consumed;

    if (m_gesture) {
        if (m_coasting) {
            // Moving fingers catch a page snap where it is shown.
            m_coasting = false;
            const PointF current = m_target->position();
            m_dragPosition = {unboundedPosition(current.x, x), unboundedPosition(current.y, y)};
            m_shownDragPosition = m_dragPosition;
        }
        // After a pause, movement continues from the predicted position on
        // screen rather than stepping back to the last sample.
        if (m_extrapolated && time - m_samples.back().time > maximumSampleInterval) {
            m_dragPosition = m_shownDragPosition;
            m_samples.push_back({m_shownTime, m_shownDragPosition});
            m_extrapolated = false;
        }
        if (delta.x != 0)
            m_dragAxes |= Horizontal;
        if (delta.y != 0)
            m_dragAxes |= Vertical;
        const PointF previous = contentPosition(m_dragPosition);
        m_dragPosition -= delta;
        if (!m_elastic) {
            m_dragPosition = {bounded(m_dragPosition.x, x.minimum, x.maximum),
                              bounded(m_dragPosition.y, y.minimum, y.maximum)};
        }
        const PointF next = contentPosition(m_dragPosition);
        m_consumed |= !same(next, previous);
        addSample(time);
        return !same(next, previous) || m_consumed;
    }

    const PointF previous = m_target->position();
    const PointF next = {bounded(previous.x - delta.x, x.minimum, x.maximum),
                         bounded(previous.y - delta.y, y.minimum, y.maximum)};
    setPosition(next);
    if (wheel && !same(next, previous)) {
        auto& loop = Application::instance()->loop();
        if (m_wheelStepTimer)
            loop.stopTimer(m_wheelStepTimer);
        m_wheelStepTimer = loop.startTimer(std::chrono::milliseconds(wheelStepDuration), false,
                                           [this] {
                                               m_wheelStepTimer = 0;
                                               updateScrolling();
                                           });
    }
    if (m_coasting)
        m_coastPosition = next;
    m_consumed |= !same(next, previous);
    return !same(next, previous) || (m_gesture && m_consumed);
}

void Scroller::release(double time)
{
    m_gesture = false;
    // Fingers that never moved a paged view leave it as it is, snapping or not.
    if (m_paged && !m_consumed) {
        if (!m_coasting)
            m_samples.clear();
        return;
    }
    if (m_samples.empty()) {
        m_velocity = {};
        if (!m_coasting)
            startCoast(m_target->position(), time);
        return;
    }

    const Sample last = m_samples.back();
    m_velocity = time - last.time > velocityTimeout ? PointF() : flickVelocity();
    if (!m_velocity.isNull()) {
        // The fingers left with the last sample; frames before it still show samples.
        startCoast(contentPosition(last.position), last.time);
    } else if (m_extrapolated) {
        m_samples.clear();
        startCoast(contentPosition(m_shownDragPosition), std::max(m_shownTime, time));
    } else {
        startCoast(contentPosition(last.position), std::max(last.time, time));
    }
}

void Scroller::startCoast(PointF position, double time)
{
    const Axis x = m_target->horizontalAxis();
    const Axis y = m_target->verticalAxis();
    m_coastPosition = position;
    m_coastTime = time;
    if (m_paged) {
        m_snapTarget = {pageTarget(position.x, m_velocity.x, x),
                        pageTarget(position.y, m_velocity.y, y)};
        const auto snap = [this](double from, double to, double velocity, const Axis& axis,
                                 double& duration, double& startVelocity) {
            const double distance = std::abs(to - from);
            duration = snapDuration + snapPageDuration * std::min(1.0, distance / axis.viewport);
            if (axis.enabled && (to - from) * velocity < 0) {
                // Heading away, as past an end, the page turns round from the
                // speed it was shown at instead of jumping back.
                const double unbounded = unboundedPosition(from, axis);
                startVelocity = velocity * (dragPosition(unbounded + 0.5, axis)
                                            - dragPosition(unbounded - 0.5, axis));
                return;
            }
            // OutCubic starts at three times its average speed. Fingers faster
            // than that shorten it, so a quick release does not brake the page.
            if ((to - from) * velocity > 0)
                duration = std::max(minimumSnapDuration,
                                    std::min(duration, 3000 * distance / std::abs(velocity)));
            if (distance == 0)
                duration = 0;
            startVelocity = duration > 0 ? 3000 * (to - from) / duration : 0;
        };
        snap(position.x, m_snapTarget.x, m_velocity.x, x, m_snapDuration.x, m_snapVelocity.x);
        snap(position.y, m_snapTarget.y, m_velocity.y, y, m_snapDuration.y, m_snapVelocity.y);
        m_coasting = !same(position, m_snapTarget) || !m_snapVelocity.isNull()
                || !same(position, m_target->position());
    } else {
        m_coasting = !m_velocity.isNull() || !same(position, m_target->position())
                || position.x != bounded(position.x, x.minimum, x.maximum)
                || position.y != bounded(position.y, y.minimum, y.maximum);
    }
    if (m_coasting)
        scheduleFrame();
    else
        m_samples.clear();
}

void Scroller::advance(double frameTime)
{
    const double time = frameTime - resampleLatency;
    if (!isActive())
        return;
    // A frame clock that ran ahead and was set back shows nothing new until it
    // catches up, but must not stop asking for frames.
    if (time < m_shownTime) {
        scheduleFrame();
        return;
    }

    PointF next;
    bool pending = false;
    if (m_gesture && !m_coasting) {
        if (m_samples.empty())
            return;
        m_shownDragPosition = resampled(time, true, &m_extrapolated);
        next = contentPosition(m_shownDragPosition);
        pending = time < m_samples.back().time;
    } else if (m_coasting && time < m_coastTime) {
        next = m_samples.empty() ? m_target->position() : contentPosition(resampled(time, false));
        pending = true;
    } else if (m_coasting && m_paged) {
        const double elapsed = time - m_coastTime;
        // A cubic Hermite from the start velocity to rest on the target.
        const auto eased = [elapsed](double from, double to, double velocity, double duration) {
            if (elapsed >= duration)
                return to;
            const double progress = elapsed / duration;
            const double remaining = 1.0 - progress;
            return to + (from - to) * (1.0 + 2.0 * progress) * remaining * remaining
                    + velocity * duration / 1000 * progress * remaining * remaining;
        };
        next = {eased(m_coastPosition.x, m_snapTarget.x, m_snapVelocity.x, m_snapDuration.x),
                eased(m_coastPosition.y, m_snapTarget.y, m_snapVelocity.y, m_snapDuration.y)};
        if (elapsed >= std::max(m_snapDuration.x, m_snapDuration.y)) {
            m_coasting = false;
            m_samples.clear();
        }
        pending = m_coasting;
    } else if (m_coasting) {
        const Axis x = m_target->horizontalAxis();
        const Axis y = m_target->verticalAxis();
        const double seconds = (time - m_coastTime) / 1000.0;
        advanceAxis(m_coastPosition.x, m_velocity.x, x, seconds);
        advanceAxis(m_coastPosition.y, m_velocity.y, y, seconds);
        m_coastTime = time;
        next = m_coastPosition;
        if (m_velocity.isNull() && next.x >= x.minimum && next.x <= x.maximum
            && next.y >= y.minimum && next.y <= y.maximum) {
            m_coasting = false;
            m_samples.clear();
        }
        pending = m_coasting;
    } else {
        return;
    }

    m_shownTime = time;
    const bool changed = !same(next, m_target->position());
    setPosition(next);
    if (changed || pending)
        scheduleFrame();
    updateScrolling();
}

void Scroller::advanceAxis(double& position, double& velocity, const Axis& axis, double seconds)
{
    const double edge = bounded(position, axis.minimum, axis.maximum);
    if (!axis.enabled || (!m_elastic && edge != position)) {
        position = edge;
        velocity = 0;
        return;
    }

    if (edge != position) {
        const double offset = position - edge;
        const double coefficient = velocity + springFrequency * offset;
        const double decay = std::exp(-springFrequency * seconds);
        position = edge + (offset + coefficient * seconds) * decay;
        velocity = (velocity - springFrequency * coefficient * seconds) * decay;
        if (std::abs(position - edge) < 0.5 && std::abs(velocity) < stopSpeed) {
            position = edge;
            velocity = 0;
        }
    } else {
        const double initialPosition = position;
        const double initialVelocity = velocity;
        const double decay = std::exp(-friction * seconds);
        position += velocity * (1.0 - decay) / friction;
        velocity *= decay;
        const double limit = bounded(position, axis.minimum, axis.maximum);
        if (limit != position) {
            if (m_elastic) {
                // Split the frame at the edge so rebound does not depend on refresh rate.
                const double crossing = -std::log1p(-(limit - initialPosition) * friction
                                                    / initialVelocity)
                        / friction;
                const double remaining = std::max(0.0, seconds - crossing);
                const double edgeVelocity = initialVelocity * std::exp(-friction * crossing);
                const double springDecay = std::exp(-springFrequency * remaining);
                position = limit + edgeVelocity * remaining * springDecay;
                velocity = edgeVelocity * (1.0 - springFrequency * remaining) * springDecay;
            } else {
                position = limit;
                velocity = 0;
            }
        } else if (std::abs(velocity) < stopSpeed) {
            velocity = 0;
        }
    }
}

} // namespace cutefish::ui
