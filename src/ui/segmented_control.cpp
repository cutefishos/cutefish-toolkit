// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/segmented_control.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/frame_client.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/theme.h"
#include "ui/window.h"

namespace cutefish::ui {

// 32 high inside a padding of 2, the plate 2 inside its button. A tab bar: a
// tab button high, a medium icon and a small spacing, without padding.
static constexpr double kSegmentedHeight = 32;
static constexpr double kTabHeight = 38;
static constexpr double kSegmentedPadding = 2;
static constexpr double kPlateInset = 2;
static constexpr std::chrono::milliseconds kPressDelay{50};
// The plate's spring: the period it would swing at undamped, in seconds.
static constexpr double kGlideResponse = 0.3;
// How much wider the plate grows at the top speed of a glide of a button, and
// at any faster.
static constexpr double kStretch = 0.08;

class SegmentedControl::Segment : public View
{
public:
    Segment(SegmentedControl* control, std::string text)
        : m_control(control)
        , m_label(addChild<Label>(std::move(text)))
    {
        // The label is plain text, coloured rather than faded.
        m_label->setFadesWhenDisabled(false);
        m_label->setTextAlignment(Label::TextAlignment::Center);
        m_label->setVerticallyCentered(true);
    }

    Label* label() const { return m_label; }
    bool acceptsFocus() const override { return true; }

protected:
    void layout() override { m_label->setGeometry(localRect()); }

    bool keyPress(const KeyEvent& event) override
    {
        if (event.key != key::Space || event.modifiers != NoModifier)
            return false;
        m_control->setPressed(index());
        return true;
    }

    bool keyRelease(const KeyEvent& event) override
    {
        if (event.key != key::Space || m_control->m_pressed != index())
            return false;
        m_control->setPressed(-1);
        m_control->pick(index());
        return true;
    }

    void focusOut() override
    {
        if (m_control->m_pressed == index())
            m_control->setPressed(-1);
    }

private:
    int index() const
    {
        const auto& segments = m_control->m_segments;
        return int(std::find(segments.begin(), segments.end(), this) - segments.begin());
    }

    SegmentedControl* m_control;
    Label* m_label;
};

class SegmentedControl::Glide : public FrameClient
{
public:
    Glide(SegmentedControl* control, Window* window)
        : FrameClient(window)
        , m_control(control)
    {
    }

    bool isRunning() const { return isTicking(); }

    void start(double target)
    {
        m_target = target;
        if (!isTicking()) {
            m_lastFrameNs = 0;
            startTicking();
        }
    }

    void stop()
    {
        m_velocity = 0;
        stopTicking();
    }

protected:
    void tick(uint64_t frameTimeNs) override
    {
        const double seconds = m_lastFrameNs && frameTimeNs > m_lastFrameNs
                                   ? double(frameTimeNs - m_lastFrameNs) / 1e9
                                   : 0;
        m_lastFrameNs = frameTimeNs;
        const double omega = 2 * std::numbers::pi / kGlideResponse;
        const double offset = m_control->m_platePosition - m_target;
        const double coefficient = m_velocity + omega * offset;
        const double decay = std::exp(-omega * seconds);
        const double next = (offset + coefficient * seconds) * decay;
        m_velocity = (coefficient - omega * (offset + coefficient * seconds)) * decay;
        if (std::abs(next) < 1e-3 && std::abs(m_velocity) < 1e-2) {
            stop();
            m_control->setPlatePosition(m_target, 0);
            return;
        }
        // From rest, a glide of one button peaks at omega / e buttons a second.
        const double topSpeed = omega / std::numbers::e;
        m_control->setPlatePosition(m_target + next, kStretch * std::min(1.0, std::abs(m_velocity) / topSpeed));
    }

private:
    SegmentedControl* m_control;
    double m_target = 0;
    double m_velocity = 0;
    uint64_t m_lastFrameNs = 0;
};

SegmentedControl::SegmentedControl(std::vector<std::string> items, Style style)
    : m_style(style)
    , m_contentHeight(style == Style::Segmented ? kSegmentedHeight : kTabHeight)
    , m_plateInset(kPlateInset)
{
    setItems(std::move(items));
}

SegmentedControl::~SegmentedControl()
{
    if (m_pressTimer)
        Application::instance()->loop().stopTimer(m_pressTimer);
}

void SegmentedControl::setItems(std::vector<std::string> items)
{
    for (Segment* segment : m_segments)
        takeChild(segment);
    m_segments.clear();
    for (std::string& item : items) {
        Segment* segment = addChild<Segment>(this, std::move(item));
        segment->label()->setFontSize(m_fontSize);
        m_segments.push_back(segment);
    }
    m_current = std::clamp(m_current, 0, std::max(0, int(m_segments.size()) - 1));
    m_pressed = -1;
    if (m_glide)
        m_glide->stop();
    setPlatePosition(m_current, 0);
    updateColors();
    requestLayout();
}

void SegmentedControl::setCurrentIndex(int index, bool animated)
{
    if (index < 0 || index >= int(m_segments.size()) || index == m_current)
        return;
    moveTo(index, animated);
}

bool SegmentedControl::isGliding() const
{
    return m_glide && m_glide->isRunning();
}

void SegmentedControl::moveTo(int index, bool animated)
{
    m_current = index;
    Window* window = this->window();
    if (!animated || !window || std::abs(m_platePosition - index) < 1e-6) {
        if (m_glide)
            m_glide->stop();
        setPlatePosition(index, 0);
        return;
    }
    if (!m_glide)
        m_glide = std::make_unique<Glide>(this, window);
    m_glide->start(index);
}

void SegmentedControl::setPlatePosition(double position, double stretch)
{
    m_platePosition = position;
    m_plateStretch = std::max(0.0, stretch);
    requestPaint();
}

void SegmentedControl::setContentHeight(double height)
{
    if (height == m_contentHeight)
        return;
    m_contentHeight = height;
    requestLayout();
}

void SegmentedControl::setPlateInset(double inset)
{
    if (inset == m_plateInset)
        return;
    m_plateInset = inset;
    requestPaint();
}

void SegmentedControl::setFontSize(double size)
{
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    for (Segment* segment : m_segments)
        segment->label()->setFontSize(size);
    requestLayout();
}

void SegmentedControl::setTrackColor(std::optional<ColorRecipe> color)
{
    m_trackColor = std::move(color);
    requestPaint();
}

double SegmentedControl::padding() const
{
    return m_style == Style::Segmented ? kSegmentedPadding : 0;
}

double SegmentedControl::implicitItemWidth(size_t index) const
{
    return index < m_segments.size() ? m_segments[index]->label()->sizeHint().width : 0;
}

SizeF SegmentedControl::sizeHint() const
{
    double width = 0;
    for (size_t i = 0; i < m_segments.size(); ++i)
        width += implicitItemWidth(i);
    return {width + 2 * padding() + Theme::current().smallSpacing, m_contentHeight + 2 * padding()};
}

// Buttons without a width of their own get equal shares.
RectF SegmentedControl::itemRect(size_t index) const
{
    const RectF inner = localRect().adjusted(padding(), padding(), -padding(), -padding());
    const double width = m_segments.empty() ? 0 : inner.width / double(m_segments.size());
    return {inner.x + width * double(index), inner.y, width, inner.height};
}

RectF SegmentedControl::plateRect() const
{
    if (m_segments.empty())
        return {};
    const RectF inner = localRect().adjusted(padding(), padding(), -padding(), -padding());
    const double width = inner.width / double(m_segments.size());
    const double grown = width * (1 + m_plateStretch);
    // Kept inside the track while it stretches at an end button.
    const double x = std::max(inner.x, std::min(inner.x + width * m_platePosition - (grown - width) / 2,
                                                inner.right() - grown));
    return RectF{x, inner.y, grown, inner.height}.adjusted(m_plateInset, m_plateInset, -m_plateInset, -m_plateInset);
}

int SegmentedControl::itemAt(PointF point) const
{
    for (size_t i = 0; i < m_segments.size(); ++i) {
        if (itemRect(i).contains(point))
            return int(i);
    }
    return -1;
}

void SegmentedControl::layout()
{
    for (size_t i = 0; i < m_segments.size(); ++i)
        m_segments[i]->setGeometry(itemRect(i));
}

void SegmentedControl::paint(Painter& painter)
{
    const Theme& theme = Theme::current();
    const RectF bounds = localRect();
    const Color plate = color(ColorId::SegmentedControlPlate);
    const double tabRadius = theme.smallRadius + 2;
    if (m_style == Style::Segmented) {
        painter.drawRoundedRect(bounds, {bounds.height / 2},
                                m_trackColor ? m_trackColor->resolve() : color(ColorId::SegmentedControlTrack));
    } else {
        painter.drawRoundedRect(bounds, {tabRadius, 0, themeCorners()}, color(ColorId::SegmentedTabBackground));
    }
    if (m_current < 0 || m_current >= int(m_segments.size()))
        return;

    const RectF highlight = plateRect();
    if (m_style == Style::Segmented) {
        // A plain rectangle with a drop shadow: Gaussian deviation (radius + 1) / 3.33.
        painter.drawShadow(highlight, highlight.height / 2, 1.2, color(ColorId::SegmentedControlPlateShadow),
                           {0, 1});
        painter.drawSmoothRectangle(highlight, highlight.height / 2, plate);
    } else {
        // Concentric with the bar through the inset.
        const Color shadow = color(ColorId::SegmentedTabPlateShadow);
        if (shadow.a > 0)
            painter.drawShadow(highlight, theme.smallRadius, 0.9, shadow);
        painter.drawRoundedRect(highlight, {tabRadius, m_plateInset, themeCorners()}, plate);
    }
}

void SegmentedControl::enabledChanged()
{
    updateColors();
}

void SegmentedControl::updateColors()
{
    for (size_t i = 0; i < m_segments.size(); ++i) {
        // The disabled text colour, half the text colour while pressed.
        Label* label = m_segments[i]->label();
        if (!isEnabled())
            label->setColor(ColorId::DisabledText);
        else if (int(i) == m_pressed && m_pressShown)
            label->setColor(ColorId::SegmentedControlTextPressed);
        else
            label->setColor(std::nullopt);
    }
}

void SegmentedControl::setPressed(int index)
{
    if (index == m_pressed)
        return;
    m_pressed = index;
    // A quick click shows no pressed label; a held press does.
    m_pressShown = false;
    if (m_pressTimer) {
        Application::instance()->loop().stopTimer(m_pressTimer);
        m_pressTimer = 0;
    }
    if (index >= 0) {
        m_pressTimer = Application::instance()->loop().startTimer(kPressDelay, false, [this] {
            m_pressTimer = 0;
            m_pressShown = true;
            updateColors();
        });
    }
    updateColors();
}

void SegmentedControl::pick(int index)
{
    if (index < 0 || index >= int(m_segments.size()) || index == m_current)
        return;
    moveTo(index, true);
    if (onCurrentChanged)
        onCurrentChanged(index);
}

void SegmentedControl::pointerPress(const PointerEvent& event)
{
    if (event.button != MouseButton::Left)
        return;
    const int index = itemAt(event.position);
    if (index >= 0)
        m_segments[size_t(index)]->setFocus(FocusReason::Mouse);
    setPressed(index);
}

void SegmentedControl::pointerRelease(const PointerEvent& event)
{
    const int pressed = m_pressed;
    setPressed(-1);
    if (pressed >= 0 && itemAt(event.position) == pressed)
        pick(pressed);
}

} // namespace cutefish::ui
