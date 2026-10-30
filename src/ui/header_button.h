// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "ui/view.h"

namespace cutefish::ui {

// A title bar button: window button artwork on a round plate that
// shows on hover, dimmed while the window is inactive.
class HeaderButton : public View
{
public:
    // An artwork such as "close", "minimize", "maximize", "restore", "add" or "menu".
    explicit HeaderButton(std::string artwork = {}, double size = 32);

    void setArtwork(std::string artwork);
    const std::string& artwork() const { return m_artwork; }
    // One of the toolkit's glyphs instead of window artwork, such as "chevron-left".
    void setGlyph(std::string name, double size = 16);
    // The pressed plate while set, as while the button's menu is open.
    void setChecked(bool checked);
    bool isChecked() const { return m_checked; }
    // The hovered plate without the pointer, as for a drop target.
    void setHighlighted(bool highlighted);

    // The plate's alpha over the header tint, hovered and pressed.
    struct PlateAlphas
    {
        double hovered;
        double pressed;
    };
    // 0.11/0.21 in light and 0.12/0.06 in dark unless set.
    void setPlateAlphas(PlateAlphas light, PlateAlphas dark);
    // The glyph or artwork in this colour instead of the header mode's tint;
    // the plates keep the tint.
    void setGlyphColor(std::optional<Color> color);
    // A window control: a grey round plate at rest, and the artwork only while
    // the pointer is over the button or its group.
    void setWindowControl(bool windowControl);
    // The group's hover, set by the owner of the neighbouring controls.
    void setGroupHovered(bool hovered);

    SizeF sizeHint() const override { return {m_size, m_size}; }
    bool acceptsPointer() const override { return true; }

    std::function<void()> onClicked;

protected:
    // The artwork drawn now; a button may pick it from the window's state.
    virtual std::string currentArtwork() const { return m_artwork; }
    void paint(Painter& painter) override;
    void paintWindowControl(Painter& painter);
    void pointerEnter() override;
    void pointerLeave() override;
    void pointerPress(const PointerEvent& event) override;
    void pointerRelease(const PointerEvent& event) override;

private:
    std::string m_artwork;
    std::string m_glyph;
    double m_glyphSize = 16;
    double m_size;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_checked = false;
    bool m_highlighted = false;
    bool m_windowControl = false;
    bool m_groupHovered = false;
    std::optional<Color> m_glyphColor;
    PlateAlphas m_light = {0.11, 0.21};
    PlateAlphas m_dark = {0.12, 0.06};
};

} // namespace cutefish::ui
