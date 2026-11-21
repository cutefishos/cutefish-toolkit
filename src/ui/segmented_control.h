// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/color_mixer.h"
#include "ui/view.h"

namespace cutefish::ui {

// A row of equal buttons, one of them current: a segmented control, a pill, or
// its tab bar, a rounded box. Each button is a Tab stop of its own; Space
// presses it and picks it on release. The plate glides to a picked button on a
// critically damped spring, stretching a little with its speed; a pick during
// the glide carries on from where the plate is and how fast it moves.
class SegmentedControl : public View
{
public:
    enum class Style {
        Segmented,
        Tabs,
    };

    explicit SegmentedControl(std::vector<std::string> items = {}, Style style = Style::Segmented);
    ~SegmentedControl() override;

    void setItems(std::vector<std::string> items);
    size_t count() const { return m_segments.size(); }
    int currentIndex() const { return m_current; }
    // Does not call onCurrentChanged.
    void setCurrentIndex(int index, bool animated = false);
    bool isGliding() const;
    // The plate's rect as now drawn, in local coordinates.
    RectF plateRect() const;
    // The buttons' height inside the padding: 32 for a segmented control, 38
    // for a tab bar.
    void setContentHeight(double height);
    // Logical pixels; 0 follows the theme.
    void setFontSize(double size);
    // The segmented track; unset is the translucent one.
    void setTrackColor(std::optional<ColorRecipe> color);
    // Around the buttons: 2 for a segmented control, none for a tab bar.
    double padding() const;
    // A button's own width, its label's.
    double implicitItemWidth(size_t index) const;

    // The buttons' own widths, the padding and a small spacing; the buttons
    // share the width given equally.
    SizeF sizeHint() const override;
    bool acceptsPointer() const override { return true; }

    // After the user picked a button.
    std::function<void(int index)> onCurrentChanged;

protected:
    void layout() override;
    void paint(Painter& painter) override;
    void enabledChanged() override;
    void pointerPress(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;

private:
    class Segment;
    class Glide;

    RectF itemRect(size_t index) const;
    int itemAt(PointF point) const;
    void pick(int index);
    void moveTo(int index, bool animated);
    void setPlatePosition(double position, double stretch);
    void setPressed(int index);
    void updateColors();

    Style m_style;
    std::vector<Segment*> m_segments;
    int m_current = 0;
    int m_pressed = -1;
    bool m_pressShown = false;
    int m_pressTimer = 0;
    double m_contentHeight;
    double m_fontSize = 0;
    std::optional<ColorRecipe> m_trackColor;
    // In buttons: the current index at rest.
    double m_platePosition = 0;
    double m_plateStretch = 0;
    std::unique_ptr<Glide> m_glide;
};

// A tab bar.
class TabBar : public SegmentedControl
{
public:
    explicit TabBar(std::vector<std::string> items = {})
        : SegmentedControl(std::move(items), Style::Tabs)
    {
    }
};

} // namespace cutefish::ui
