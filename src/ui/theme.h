// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>

#include "base/signal.h"
#include "ui/geometry.h"

namespace cutefish::ui {

// The desktop's colours and metrics. Lengths are logical pixels; font sizes are
// kept in points, as the appearance settings store them.
struct Theme
{
    bool darkMode = false;

    Color background;
    Color secondBackground;
    Color alternateBackground;
    Color text;
    Color disabledText;
    Color highlight;
    Color highlightedText;

    double smallRadius = 8;
    double mediumRadius = 10;
    double bigRadius = 12;
    double hugeRadius = 14;
    double windowRadius = 14;

    double smallSpacing = 6;
    double mediumSpacing = 9;
    double largeSpacing = 12;

    // Empty: fontconfig's sans-serif.
    std::string fontFamily;
    // The appearance's fixed-pitch family; empty for the platform's default.
    std::string fixedFontFamily;
    double fontPointSize = 9;
    std::string iconTheme = "Cutefish";
    // The compositor's window corners on large panels.
    bool continuousCorners = true;

    // Points to logical pixels at the 96 dpi logical resolution.
    double fontSize() const { return fontPointSize * 4 / 3; }
    double smallFontSize() const { return (fontPointSize - 2) * 4 / 3; }

    static Theme light();
    static Theme dark();

    static const Theme& current();
    // Every window lays out and paints again.
    static void setCurrent(const Theme& theme);
    // Counts the changes, for caches of what the theme decides.
    static uint64_t generation();
    // After setCurrent has reached the windows: for what takes the theme
    // outside them.
    static base::Signal<>& changed();
};

namespace accent {
inline constexpr Color blue = Color::rgb(0x3385FF);
inline constexpr Color red = Color::rgb(0xFF5C6D);
inline constexpr Color green = Color::rgb(0x35BF56);
inline constexpr Color purple = Color::rgb(0x8266FF);
inline constexpr Color pink = Color::rgb(0xCA64AC);
inline constexpr Color orange = Color::rgb(0xFEA042);
inline constexpr Color grey = Color::rgb(0x4F596B);
} // namespace accent

} // namespace cutefish::ui
