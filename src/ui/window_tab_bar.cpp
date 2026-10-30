// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/window_tab_bar.h"

#include <algorithm>
#include <cmath>

#include "include/core/SkImage.h"

#include "ui/frame_client.h"
#include "ui/icon_loader.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/painter.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/window.h"

namespace cutefish::ui {

// The tab bar's timings and sizes.
static constexpr int kSlideMs = 150;
static constexpr int kLagMs = 120;
static constexpr double kDragThreshold = 8;
static constexpr double kDetachDistance = 16;
static constexpr double kAutoScrollEdge = 24;
// 8 px every 30 ms.
static constexpr double kAutoScrollSpeed = 8.0 / 30.0;

static double outCubic(double t)
{
    const double u = 1 - t;
    return 1 - u * u * u;
}

static Color withAlpha(Color color, double alpha)
{
    return color.withAlpha(alpha);
}

double WindowTabBar::Tween::value(uint64_t now) const
{
    if (!active || durationMs <= 0 || now <= startNs)
        return active && now <= startNs ? from : to;
    const double t = double(now - startNs) / (double(durationMs) * 1e6);
    return t >= 1 ? to : from + (to - from) * outCubic(t);
}

void WindowTabBar::Tween::start(double begin, double end, int duration, uint64_t now)
{
    from = begin;
    to = end;
    startNs = now;
    durationMs = duration;
    active = true;
}

class WindowTabBar::Ticker final : public FrameClient
{
public:
    Ticker(Window* window, WindowTabBar* bar)
        : FrameClient(window)
        , m_bar(bar)
    {
    }

    void run() { startTicking(); }
    void halt() { stopTicking(); }
    uint64_t frameNs = 0;

protected:
    void tick(uint64_t frameTimeNs) override
    {
        frameNs = frameTimeNs;
        m_bar->tick(frameTimeNs);
    }

private:
    WindowTabBar* m_bar;
};

class WindowTabBar::CloseButton final : public View
{
public:
    explicit CloseButton(WindowTabBar* bar, Tab* tab)
        : m_bar(bar)
        , m_tab(tab)
    {
    }

    bool acceptsPointer() const override { return isVisible(); }
    bool hovered() const { return m_hovered; }

protected:
    void paint(Painter& painter) override;
    void pointerEnter() override
    {
        m_hovered = true;
        requestPaint();
    }
    void pointerLeave() override
    {
        m_hovered = false;
        requestPaint();
    }
    void pointerPress(const PointerEvent& event) override
    {
        m_pressed = event.button == MouseButton::Left;
        requestPaint();
    }
    void pointerRelease(const PointerEvent& event) override;

private:
    WindowTabBar* m_bar;
    Tab* m_tab;
    bool m_hovered = false;
    bool m_pressed = false;
};

class WindowTabBar::Tab final : public View
{
public:
    Tab(WindowTabBar* bar, std::string title)
        : m_bar(bar)
        , m_title(std::move(title))
        , m_label(addChild<Label>(m_title))
        , m_close(addChild<CloseButton>(bar, this))
    {
        m_label->setFontSize(12);
        m_label->setElide(Label::Elide::Middle);
        m_label->setTextAlignment(Label::TextAlignment::Center);
        m_label->setVerticallyCentered(true);
    }

    int index() const
    {
        const auto& tabs = m_bar->m_tabs;
        return int(std::find(tabs.begin(), tabs.end(), this) - tabs.begin());
    }

    void setTitle(std::string title)
    {
        m_title = std::move(title);
        m_label->setText(m_title);
        requestLayout();
    }

    const std::string& title() const { return m_title; }

    void setModified(bool modified)
    {
        m_modified = modified;
        requestPaint();
    }

    bool hovered() const { return m_hovered || m_close->hovered(); }
    bool raised() const
    {
        const int i = index();
        return i == m_bar->m_current || i == m_bar->m_dragIndex || landing;
    }
    bool closeShown() const
    {
        const int i = index();
        return (i == m_bar->m_current || hovered()) && m_bar->m_dragIndex < 0 && m_bar->m_editing != i;
    }

    void updateLook()
    {
        const int i = index();
        const bool selected = i == m_bar->m_current;
        m_label->setWeight(selected ? 500 : 400);
        m_label->setColor(selected ? m_bar->textRecipe() : withAlpha(m_bar->textRecipe(), 0.6));
        m_label->setVisible(m_bar->m_editing != i);
        m_close->setVisible(closeShown());
        requestPaint();
    }

    void startEditing()
    {
        if (!m_editor) {
            m_editor = addChild<TextField>();
            m_editor->setFrameVisible(false);
            m_editor->setFadesWhenDisabled(false);
            m_editor->setTextMargins(0, 0);
            m_editor->setFontSize(12);
            m_editor->setContextMenuEnabled(false);
            m_editor->onAccepted = [this] {
                const int i = index();
                const std::string text = m_editor->text();
                m_bar->m_editing = -1;
                m_bar->updateSlots();
                if (m_bar->onRenameRequested)
                    m_bar->onRenameRequested(i, text);
            };
            m_editor->onEditingFinished = [this] {
                if (m_bar->m_editing == index()) {
                    m_bar->m_editing = -1;
                    m_bar->updateSlots();
                }
            };
        }
        const ColorRecipe text = m_bar->textRecipe();
        m_editor->setTextColor(text);
        m_editor->setSelectionColors(withAlpha(text, 0.25), text);
        m_editor->setText(m_title);
        m_editor->setVisible(true);
        m_editor->selectAll();
        m_editor->setFocus();
        requestLayout();
    }

    void stopEditing()
    {
        if (m_editor)
            m_editor->setVisible(false);
    }

    bool keyPress(const KeyEvent& event) override
    {
        if (event.key == key::Escape && m_editor && m_editor->isVisible()) {
            m_bar->m_editing = -1;
            m_bar->updateSlots();
            return true;
        }
        return false;
    }

    // The slot the tab stands for and the glide towards it.
    double slot = 0;
    double lastSlot = 0;
    Tween shift;
    bool landing = false;
    bool placed = false;

    void glideFrom(double drawnSlot, uint64_t now)
    {
        if (m_bar->sliding()) {
            shift.start(drawnSlot - slot, 0, kSlideMs, now);
        } else {
            shift = {};
            landing = false;
        }
        lastSlot = slot;
    }

    double drawnSlot(uint64_t now) const { return slot + shift.value(now); }

protected:
    void layout() override
    {
        const RectF bounds = localRect();
        // A title long enough to reach the close button keeps clear of it
        // whether or not the button shows, so hovering never re-elides it.
        const double margin = m_label->sizeHint().width > bounds.width - 56 ? 28 : 12;
        m_label->setGeometry({margin, 0, std::max(0.0, bounds.width - 2 * margin), bounds.height});
        m_close->setGeometry({8, std::round((bounds.height - 16) / 2), 16, 16});
        if (m_editor)
            m_editor->setGeometry({16, 0, std::max(0.0, bounds.width - 32), bounds.height});
    }

    void paint(Painter& painter) override
    {
        const int i = index();
        const RectF pill = localRect().adjusted(2, 2, -2, -2);
        const bool selected = raised();
        Color fill = {0, 0, 0, 0};
        if (selected)
            fill = m_bar->selectedColor();
        else if (m_pressed && m_bar->m_pressedFeedback)
            fill = withAlpha(m_bar->textColor(), 0.10);
        else if (hovered() || i == m_bar->m_springLoad)
            fill = m_bar->hoverColor();
        if (fill.a > 0) {
            const Color edge = selected ? m_bar->colors().get(ColorId::WindowTabBarSelectedEdge) : Color{0, 0, 0, 0};
            painter.drawSmoothRectangle(pill, pill.height / 2, fill, edge.a > 0 ? 1 : 0, edge);
        }
        // Between two resting tabs only; a raised or hovered pill has its own edge.
        if (!m_bar->sliding() && i < m_bar->count() - 1 && i != m_bar->m_current && !hovered()
            && m_bar->m_current != i + 1) {
            painter.fillRect({localRect().width - 1, std::round((localRect().height - 14) / 2), 1, 14},
                             m_bar->separatorColor());
        }
        // The editor's dot, in the close button's place.
        if (m_modified && !closeShown() && m_bar->m_editing != i) {
            const RectF button = m_close->geometry();
            painter.fillEllipse({button.x + 4, button.y + 4, 8, 8}, withAlpha(m_bar->textColor(), 0.6));
        }
    }

    void pointerEnter() override
    {
        m_hovered = true;
        m_bar->updateSlots();
    }

    void pointerLeave() override
    {
        m_hovered = false;
        m_bar->updateSlots();
    }

    void pointerPress(const PointerEvent& event) override
    {
        m_pressButton = event.button;
        m_pressed = true;
        m_didDrag = false;
        m_detached = false;
        const PointF inBar = m_bar->mapFromWindow(mapToWindow(event.position));
        m_pressX = inBar.x - m_bar->m_trackInsets.left + m_bar->m_scrollX;
        m_pressY = inBar.y;
        requestPaint();
    }

    void pointerMove(const PointerEvent& event) override
    {
        if (!m_pressed || m_pressButton != MouseButton::Left || m_detached || m_bar->m_editing == index())
            return;
        const PointF inBar = m_bar->mapFromWindow(mapToWindow(event.position));
        const double x = inBar.x - m_bar->m_trackInsets.left + m_bar->m_scrollX;
        const double y = inBar.y;
        if (!m_didDrag && (std::abs(x - m_pressX) > kDragThreshold || std::abs(y - m_pressY) > kDragThreshold)) {
            m_didDrag = true;
            m_held = m_bar->count() == 1 && !m_bar->m_loneTabDraggable;
            if (!m_held)
                m_bar->beginDrag(index(), m_pressX);
        }
        if (m_didDrag && (y < -kDetachDistance || y > m_bar->localRect().height + kDetachDistance)) {
            m_didDrag = false;
            m_detached = true;
            m_bar->detach(index(), mapToWindow(event.position));
            return;
        }
        if (m_didDrag && !m_held)
            m_bar->updateDrag(x - m_bar->m_scrollX);
    }

    void pointerRelease(const PointerEvent& event) override
    {
        m_pressed = false;
        requestPaint();
        if (m_detached) {
            // The press ended on the bar: the tab stays.
            m_bar->cancelDetach();
            return;
        }
        if (m_didDrag) {
            m_didDrag = false;
            if (!m_held)
                m_bar->endDrag(true);
            return;
        }
        if (!localRect().contains(event.position) || event.button != m_pressButton)
            return;
        const int i = index();
        if (event.button == MouseButton::Middle) {
            if (m_bar->onCloseRequested)
                m_bar->onCloseRequested(i);
        } else if (event.button == MouseButton::Right) {
            if (m_bar->onContextMenuRequested)
                m_bar->onContextMenuRequested(i, mapToWindow(event.position));
        } else if (event.button == MouseButton::Left) {
            m_bar->setCurrentIndex(i);
            if (m_bar->onCurrentChanged)
                m_bar->onCurrentChanged(i);
        }
    }

    bool acceptsPointer() const override { return true; }

private:
    WindowTabBar* m_bar;
    std::string m_title;
    Label* m_label;
    CloseButton* m_close;
    TextField* m_editor = nullptr;
    bool m_modified = false;
    bool m_hovered = false;
    bool m_pressed = false;
    MouseButton m_pressButton = MouseButton::Left;
    double m_pressX = 0;
    double m_pressY = 0;
    bool m_didDrag = false;
    // Dragged, but a lone tab that may not move.
    bool m_held = false;
    bool m_detached = false;
};

void WindowTabBar::CloseButton::paint(Painter& painter)
{
    const RectF bounds = localRect();
    const Color text = m_bar->textColor();
    if (m_hovered) {
        const Color plate = m_pressed ? withAlpha(text, 0.16) : m_bar->hoverColor();
        painter.fillRoundedRect(bounds, bounds.height / 2, plate);
    }
    const Rect box = painter.snappedRect(bounds);
    sk_sp<SkImage> glyph = IconLoader::instance().glyph("close", box.width, text);
    painter.save();
    painter.multiplyOpacity(m_hovered ? 1.0 : 0.6);
    painter.drawImage(glyph.get(), box.x, box.y);
    painter.restore();
}

void WindowTabBar::CloseButton::pointerRelease(const PointerEvent& event)
{
    const bool clicked = m_pressed && localRect().contains(event.position);
    m_pressed = false;
    requestPaint();
    if (clicked && m_bar->onCloseRequested)
        m_bar->onCloseRequested(m_tab->index());
}

// The track and the tabs, clipped to the pill while crowded.
class WindowTabBar::Strip final : public View
{
public:
    explicit Strip(WindowTabBar* bar)
        : m_bar(bar)
    {
        setClipsChildren(true);
        // The track around and after the tabs is the header's.
        setMovesWindow(true);
    }

    bool acceptsPointer() const override { return true; }

protected:
    void paint(Painter& painter) override
    {
        const double width = m_bar->m_trackWidth.value(m_bar->now());
        const RectF track = {-m_bar->m_scrollX, 0, width, localRect().height};
        painter.drawSmoothRectangle(track, track.height / 2, m_bar->trackColor());
    }

    bool scroll(const ScrollEvent& event) override
    {
        if (m_bar->contentWidth() <= localRect().width || m_bar->m_dragIndex >= 0)
            return false;
        // Either direction moves the strip: a wheel turns vertically.
        const double delta = std::abs(event.deltaX) > std::abs(event.deltaY) ? event.deltaX : event.deltaY;
        m_bar->setScroll(m_bar->m_scrollX + delta);
        return true;
    }

private:
    WindowTabBar* m_bar;
};

// A tab dragged in from another window.
class Ghost final : public View
{
public:
    Ghost()
        : m_label(addChild<Label>())
    {
        m_label->setFontSize(12);
        m_label->setWeight(500);
        m_label->setElide(Label::Elide::Middle);
        m_label->setTextAlignment(Label::TextAlignment::Center);
        m_label->setVerticallyCentered(true);
    }

    void set(const std::string& title, Color fill, Color border, Color text)
    {
        m_label->setText(title);
        m_label->setColor(text);
        m_fill = fill;
        m_border = border;
        requestPaint();
    }

protected:
    void layout() override
    {
        m_label->setGeometry(localRect().adjusted(12, 0, -12, 0));
    }

    void paint(Painter& painter) override
    {
        const RectF pill = localRect().adjusted(2, 2, -2, -2);
        painter.drawRoundedRect(pill, {pill.height / 2, 0, Corners::Circular}, m_fill, 1, m_border);
    }

private:
    Label* m_label;
    Color m_fill;
    Color m_border;
};

WindowTabBar::WindowTabBar()
    : m_strip(addChild<Strip>(this))
{
    auto ghost = std::make_unique<Ghost>();
    m_ghost = ghost.get();
    m_strip->addChild(std::move(ghost));
    m_ghost->setVisible(false);
}

WindowTabBar::~WindowTabBar() = default;

uint64_t WindowTabBar::now() const
{
    return m_ticker ? m_ticker->frameNs : 0;
}

const ColorProvider& WindowTabBar::colors() const
{
    return ColorProvider::forMode(m_darkMode.value_or(Theme::current().darkMode));
}

Color WindowTabBar::textColor() const
{
    return m_textColor ? m_textColor->resolve() : Theme::current().text;
}

ColorRecipe WindowTabBar::textRecipe() const
{
    return m_textColor.value_or(ColorId::Text);
}

Color WindowTabBar::trackColor() const
{
    return colors().get(ColorId::WindowTabBarTrack);
}

Color WindowTabBar::selectedColor() const
{
    return colors().get(ColorId::WindowTabBarSelected);
}

Color WindowTabBar::hoverColor() const
{
    return withAlpha(textColor(), 0.05);
}

Color WindowTabBar::separatorColor() const
{
    return withAlpha(textColor(), 0.12);
}

Color WindowTabBar::dimTextColor() const
{
    return withAlpha(textColor(), 0.6);
}

int WindowTabBar::count() const
{
    return int(m_tabs.size());
}

const std::string& WindowTabBar::tabTitle(int index) const
{
    static const std::string none;
    return index >= 0 && index < count() ? m_tabs[size_t(index)]->title() : none;
}

int WindowTabBar::slotCount() const
{
    return count() + (m_incoming ? 1 : 0) - (m_leaving >= 0 ? 1 : 0);
}

double WindowTabBar::tabWidth() const
{
    const int slots = slotCount();
    if (slots <= 0)
        return 0;
    const double width = std::max(0.0, localRect().width - m_trackInsets.horizontal());
    double each = width / slots;
    if (m_maximumTabWidth > 0)
        each = std::min(m_maximumTabWidth, each);
    return std::max(m_minimumTabWidth, each);
}

double WindowTabBar::contentWidth() const
{
    return tabWidth() * slotCount();
}

double WindowTabBar::trackWidth() const
{
    // At rest it follows the bar's width at once, before the bar lays out.
    return sliding() ? m_trackWidth.value(now()) : contentWidth();
}

bool WindowTabBar::sliding() const
{
    return m_dragIndex >= 0 || m_incoming || now() < m_settleUntil;
}

void WindowTabBar::restartSettle()
{
    // Every edge of a drag restarts it first: glides that follow the edge
    // would otherwise still see no sliding and jump.
    m_settleUntil = now() + uint64_t(kSlideMs) * 1000000;
}

double WindowTabBar::slotFor(int index) const
{
    if (m_incoming)
        return index + (index >= incomingIndex() ? 1 : 0);
    if (m_leaving >= 0)
        return index - (index > m_leaving ? 1 : 0);
    if (m_dragIndex < 0 || index == m_dragIndex)
        return index;
    const int without = index - (index > m_dragIndex ? 1 : 0);
    return without + (without >= m_dropIndex ? 1 : 0);
}

int WindowTabBar::incomingIndex() const
{
    if (!m_incoming)
        return -1;
    const double width = std::max(1.0, tabWidth());
    return std::clamp(int(std::round((m_incomingX - m_trackInsets.left + m_scrollX) / width - 0.5)), 0, count());
}

void WindowTabBar::insertTab(int index, std::string title)
{
    index = std::clamp(index, 0, count());
    auto tab = std::make_unique<Tab>(this, std::move(title));
    Tab* raw = tab.get();
    m_strip->addChild(std::move(tab));
    // The ghost stays above the tabs.
    m_strip->raiseChild(m_ghost);
    m_tabs.insert(m_tabs.begin() + index, raw);
    if (m_current >= index && count() > 1)
        ++m_current;
    if (m_leaving >= index)
        ++m_leaving;
    // A rename or a drag stays with its tab.
    if (m_editing >= index)
        ++m_editing;
    if (m_dragIndex >= index) {
        ++m_dragIndex;
        m_dropIndex = std::clamp(m_dropIndex + 1, 0, count() - 1);
    }
    raw->slot = slotFor(index);
    raw->lastSlot = raw->slot;
    // A tab dropped in from another window glides from where it was drawn.
    if (m_arrivalSlot >= 0) {
        raw->landing = true;
        raw->shift.start(m_arrivalSlot - raw->slot, 0, kSlideMs, now());
        m_arrivalSlot = -1;
    }
    raw->placed = true;
    updateSlots();
}

void WindowTabBar::removeTab(int index)
{
    if (index < 0 || index >= count())
        return;
    Tab* tab = m_tabs[size_t(index)];
    m_tabs.erase(m_tabs.begin() + index);
    if (m_leaving == index)
        m_leaving = -1;
    else if (m_leaving > index)
        --m_leaving;
    if (m_editing == index)
        m_editing = -1;
    else if (m_editing > index)
        --m_editing;
    if (m_dragIndex == index) {
        m_dragIndex = -1;
        m_dropIndex = -1;
    } else if (m_dragIndex > index) {
        --m_dragIndex;
        m_dropIndex = std::clamp(m_dropIndex - 1, 0, count() - 1);
    } else if (m_dragIndex >= 0) {
        m_dropIndex = std::clamp(m_dropIndex, 0, count() - 1);
    }
    if (m_current > index || (m_current == index && m_current >= count()))
        m_current = std::max(0, m_current - 1);
    m_strip->takeChild(tab);
    updateSlots();
}

void WindowTabBar::moveTab(int from, int to)
{
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to)
        return;
    Tab* tab = m_tabs[size_t(from)];
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, tab);
    if (m_current == from)
        m_current = to;
    else if (from < m_current && to >= m_current)
        --m_current;
    else if (from > m_current && to <= m_current)
        ++m_current;
    updateSlots();
}

void WindowTabBar::setTabTitle(int index, std::string title)
{
    if (index >= 0 && index < count())
        m_tabs[size_t(index)]->setTitle(std::move(title));
}

void WindowTabBar::setTabModified(int index, bool modified)
{
    if (index >= 0 && index < count())
        m_tabs[size_t(index)]->setModified(modified);
}

void WindowTabBar::setCurrentIndex(int index)
{
    if (index < 0 || index >= count())
        return;
    m_current = index;
    if (m_dragIndex < 0)
        ensureVisible(index);
    updateSlots();
}

void WindowTabBar::setDarkMode(std::optional<bool> dark)
{
    m_darkMode = dark;
    updateSlots();
    requestPaint();
}

void WindowTabBar::setTextColor(std::optional<ColorRecipe> color)
{
    m_textColor = std::move(color);
    updateSlots();
    requestPaint();
}

void WindowTabBar::setTabWidthRange(double minimum, double maximum)
{
    if (minimum == m_minimumTabWidth && maximum == m_maximumTabWidth)
        return;
    m_minimumTabWidth = minimum;
    m_maximumTabWidth = maximum;
    requestLayout();
}

void WindowTabBar::setTrackInsets(const Margins& insets)
{
    if (insets == m_trackInsets)
        return;
    m_trackInsets = insets;
    requestLayout();
}

void WindowTabBar::setPressedFeedback(bool pressed)
{
    m_pressedFeedback = pressed;
}

void WindowTabBar::setLoneTabDraggable(bool draggable)
{
    m_loneTabDraggable = draggable;
}

void WindowTabBar::setSpringLoadIndex(int index)
{
    if (index == m_springLoad)
        return;
    m_springLoad = index;
    for (Tab* tab : m_tabs)
        tab->requestPaint();
}

RectF WindowTabBar::tabRect(int index) const
{
    if (index < 0 || index >= count())
        return {};
    const RectF geometry = m_tabs[size_t(index)]->geometry();
    return {geometry.x + m_strip->geometry().x, geometry.y + m_strip->geometry().y, geometry.width, geometry.height};
}

int WindowTabBar::indexAtPosition(PointF position) const
{
    for (int i = 0; i < count(); ++i) {
        if (i != m_leaving && tabRect(i).contains(position))
            return i;
    }
    return -1;
}

void WindowTabBar::startRename(int index)
{
    if (index < 0 || index >= count())
        return;
    if (m_editing >= 0 && m_editing < count())
        m_tabs[size_t(m_editing)]->stopEditing();
    m_editing = index;
    ensureVisible(index);
    m_tabs[size_t(index)]->startEditing();
    updateSlots();
}

void WindowTabBar::beginIncoming(std::string title, double x)
{
    restartSettle();
    m_incomingTitle = std::move(title);
    m_incomingX = x;
    m_incoming = true;
    static_cast<Ghost*>(m_ghost)->set(m_incomingTitle, selectedColor(), separatorColor(), textColor());
    m_ghost->setVisible(true);
    updateSlots();
}

void WindowTabBar::setIncomingX(double x)
{
    m_incomingX = x;
    updateSlots();
}

void WindowTabBar::endIncoming(bool dropped)
{
    restartSettle();
    const double width = std::max(1.0, m_slotWidth.value(now()));
    m_arrivalSlot = dropped ? m_ghost->geometry().x / width + m_scrollX / width : -1;
    m_incoming = false;
    m_ghost->setVisible(false);
    updateSlots();
}

void WindowTabBar::cancelDetach()
{
    if (m_leaving < 0)
        return;
    restartSettle();
    m_leaving = -1;
    updateSlots();
}

void WindowTabBar::ensureVisible(int index)
{
    if (index < 0 || index >= count())
        return;
    const double width = tabWidth();
    const double left = index * width;
    const double viewport = m_strip->geometry().width;
    double x = m_scrollX;
    if (left < x)
        x = left;
    else if (left + width > x + viewport)
        x = left + width - viewport;
    setScroll(x);
}

void WindowTabBar::setScroll(double x)
{
    const double limit = std::max(0.0, contentWidth() - m_strip->geometry().width);
    const double clamped = std::clamp(x, 0.0, limit);
    if (clamped == m_scrollX)
        return;
    m_scrollX = clamped;
    placeTabs();
    m_strip->requestPaint();
}

void WindowTabBar::beginDrag(int index, double pressX)
{
    restartSettle();
    m_grabOffset = pressX - index * tabWidth();
    m_dragIndex = index;
    m_dropIndex = index;
    // A dragged tab is shown first, as a pressed one is.
    if (m_current != index) {
        m_current = index;
        if (onCurrentChanged)
            onCurrentChanged(index);
    }
    updateDrag(pressX - m_scrollX);
    // From its slot, rather than jumping the drag threshold at once.
    m_dragLag.start(index * tabWidth() - m_dragX, 0, kLagMs, now());
    updateSlots();
}

void WindowTabBar::updateDrag(double x)
{
    m_pointerX = x;
    const double width = tabWidth();
    const double first = m_scrollX;
    // Whole in view; at an end the others scroll under it.
    m_dragX = std::clamp(x + first - m_grabOffset, first,
                         first + std::max(0.0, m_strip->geometry().width - width));
    m_dropIndex = std::clamp(int(std::round(m_dragX / std::max(1.0, width))), 0, count() - 1);
    updateSlots();
}

void WindowTabBar::endDrag(bool moved)
{
    const int from = m_dragIndex;
    const int to = m_dropIndex;
    restartSettle();
    if (from >= 0 && from < count()) {
        Tab* tab = m_tabs[size_t(from)];
        // Let go, it glides from under the pointer into its slot.
        tab->landing = true;
        const double width = std::max(1.0, m_slotWidth.value(now()));
        tab->shift.start((m_dragX + m_dragLag.value(now())) / width - tab->slot, 0, kSlideMs, now());
        tab->lastSlot = tab->slot;
    }
    m_dragIndex = -1;
    m_dropIndex = -1;
    updateSlots();
    if (moved && from >= 0 && onMoveRequested)
        onMoveRequested(from, to);
}

void WindowTabBar::detach(int index, PointF windowPosition)
{
    // The rest close up behind it before the application lets it go.
    restartSettle();
    m_leaving = index;
    m_dragIndex = -1;
    m_dropIndex = -1;
    updateSlots();
    if (onDetachRequested)
        onDetachRequested(index, windowPosition);
}

void WindowTabBar::updateSlots()
{
    const uint64_t t = now();
    // The slot width and the track follow the tab width, eased while sliding.
    const double width = tabWidth();
    if (m_slotWidth.to != width) {
        if (sliding() && m_slotWidth.active)
            m_slotWidth.start(m_slotWidth.value(t), width, kSlideMs, t);
        else
            m_slotWidth = {width, width, t, 0, true};
    }
    const double track = contentWidth();
    if (m_trackWidth.to != track) {
        if (sliding() && m_trackWidth.active)
            m_trackWidth.start(m_trackWidth.value(t), track, kSlideMs, t);
        else
            m_trackWidth = {track, track, t, 0, true};
    }
    for (int i = 0; i < count(); ++i) {
        Tab* tab = m_tabs[size_t(i)];
        const double slot = slotFor(i);
        if (slot != tab->slot) {
            const double drawn = tab->lastSlot + tab->shift.value(t);
            tab->slot = slot;
            tab->glideFrom(drawn, t);
        }
        tab->updateLook();
    }
    placeTabs();
    scheduleTicks();
    requestPaint();
    m_strip->requestPaint();
}

void WindowTabBar::placeTabs()
{
    const uint64_t t = now();
    const double width = m_slotWidth.value(t);
    const double height = m_strip->geometry().height;
    for (int i = 0; i < count(); ++i) {
        Tab* tab = m_tabs[size_t(i)];
        double x = i == m_dragIndex ? m_dragX + m_dragLag.value(t) : tab->drawnSlot(t) * width;
        tab->setGeometry({x - m_scrollX, 0, width, height});
        // Not hidden: hiding would cancel the press a window drag goes on with.
        tab->setOpacity(i == m_leaving ? 0 : 1);
    }
    if (m_incoming) {
        const double ghostX = std::clamp(m_incomingX - m_trackInsets.left + m_scrollX - width / 2, 0.0,
                                         std::max(0.0, contentWidth() - width));
        m_ghost->setGeometry({ghostX - m_scrollX, 0, width, height});
    }
    // Crowded, the strip is clipped to the pill of the track.
    m_strip->setChildClipRadius(contentWidth() > m_strip->geometry().width ? height / 2 : 0);
}

void WindowTabBar::scheduleTicks()
{
    Window* window = this->window();
    if (!window)
        return;
    if (!m_ticker || m_ticker->window() != window)
        m_ticker = std::make_unique<Ticker>(window, this);
    m_ticker->run();
}

void WindowTabBar::tick(uint64_t t)
{
    // A tab held near an end of a crowded bar scrolls the hidden tabs in.
    if (m_dragIndex >= 0 && contentWidth() > m_strip->geometry().width) {
        const double elapsedMs = m_lastTickNs && t > m_lastTickNs ? double(t - m_lastTickNs) / 1e6 : 0;
        double delta = 0;
        if (m_pointerX < kAutoScrollEdge)
            delta = -kAutoScrollSpeed * elapsedMs;
        else if (m_pointerX > m_strip->geometry().width - kAutoScrollEdge)
            delta = kAutoScrollSpeed * elapsedMs;
        if (delta != 0) {
            setScroll(m_scrollX + delta);
            updateDrag(m_pointerX);
        }
    }
    m_lastTickNs = t;
    bool moving = m_dragIndex >= 0 || t < m_settleUntil || m_incoming;
    const auto running = [t](const Tween& tween) {
        return tween.active && tween.durationMs > 0 && t < tween.startNs + uint64_t(tween.durationMs) * 1000000;
    };
    moving = moving || running(m_slotWidth) || running(m_trackWidth) || running(m_dragLag);
    for (Tab* tab : m_tabs) {
        if (running(tab->shift)) {
            moving = true;
        } else if (tab->landing) {
            tab->landing = false;
            tab->requestPaint();
        }
    }
    placeTabs();
    for (Tab* tab : m_tabs)
        tab->updateLook();
    m_strip->requestPaint();
    if (!moving && m_ticker) {
        m_ticker->halt();
        m_lastTickNs = 0;
    }
}

void WindowTabBar::layout()
{
    const RectF bounds = localRect();
    m_strip->setGeometry({m_trackInsets.left, m_trackInsets.top,
                          std::max(0.0, bounds.width - m_trackInsets.horizontal()),
                          std::max(0.0, bounds.height - m_trackInsets.vertical())});
    setScroll(m_scrollX);
    // A narrower bar would otherwise leave the current tab cut off at an end.
    if (bounds.width != m_laidOutWidth) {
        m_laidOutWidth = bounds.width;
        if (m_dragIndex < 0)
            ensureVisible(m_current);
    }
    if (!sliding()) {
        m_slotWidth = {tabWidth(), tabWidth(), now(), 0, true};
        m_trackWidth = {contentWidth(), contentWidth(), now(), 0, true};
    }
    placeTabs();
}

void WindowTabBar::paint(Painter&)
{
}

} // namespace cutefish::ui
