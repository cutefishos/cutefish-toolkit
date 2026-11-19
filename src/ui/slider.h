// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ui/view.h"

namespace cutefish::ui {

// A horizontal slider: a thin track filled in the accent colour up to
// a round white handle with a soft shadow. A stepped slider marks its steps
// under the track and is taller.
class Slider : public View
{
public:
    // When the handle lands on a step.
    enum class SnapMode {
        NoSnap,
        SnapAlways,
        // The handle follows the pointer and lands on a step when let go.
        SnapOnRelease,
    };

    Slider();

    void setRange(double from, double to);
    // Values the pointer and the arrow keys move between; zero is continuous,
    // and the arrow keys then move by 0.1.
    void setStepSize(double step);
    // Marks the steps under the track. The snap mode then defaults to
    // SnapAlways, otherwise to SnapOnRelease.
    void setStepped(bool stepped);
    void setSnapMode(SnapMode mode);
    double value() const { return m_value; }
    // Clamped to the range; does not call onMoved.
    void setValue(double value);
    // A tool tip over the handle while it is held, its text the value as
    // given.
    void setValueToolTip(std::function<std::string(double value)> text);

    SizeF sizeHint() const override;
    RectF visualRect() const override;
    bool acceptsPointer() const override { return true; }
    bool acceptsFocus() const override { return true; }

    // While the user moves the handle.
    std::function<void(double value)> onMoved;
    // While the handle is held, as Slider.pressed.
    bool isPressed() const { return m_pressed; }
    std::function<void()> onPressedChanged;

protected:
    void paint(Painter& painter) override;
    void pointerPress(const PointerEvent& event) override;
    void pointerMove(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;
    bool keyPress(const KeyEvent& event) override;

private:
    friend class TestDriver;

    double position() const;
    // Where the handle shows: under the pointer while dragged.
    double visualPosition() const;
    double valueAt(double position) const;
    double snapPosition(double position) const;
    SnapMode snapMode() const;
    RectF handleRect() const;
    void moveTo(double value);
    void updateValueToolTip();

    double m_from = 0;
    double m_to = 1;
    double m_step = 0;
    double m_value = 0;
    bool m_stepped = false;
    std::optional<SnapMode> m_snapMode;
    bool m_pressed = false;
    double m_dragPosition = 0;
    double m_grabOffset = 0;
    std::function<std::string(double)> m_valueToolTip;
};

} // namespace cutefish::ui
