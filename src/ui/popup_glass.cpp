// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/popup_glass.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <system_error>

#include "base/file_watcher.h"
#include "base/paths.h"
#include "base/settings.h"
#include "ui/theme.h"

namespace cutefish::ui {

namespace {

// The dock's glass slider, as cutefish-compositor's shell has it: the
// slider's position as the amount the material blends by, on a pace that
// keeps the middle of the travel near the default.
constexpr double kPace[] = {0.000, 0.048, 0.116, 0.174, 0.249, 0.314, 0.361, 0.398, 0.435, 0.469, 0.500,
                            0.587, 0.630, 0.666, 0.701, 0.739, 0.781, 0.827, 0.879, 0.931, 1.000};

double glassAmount(double tint)
{
    const int last = int(std::size(kPace)) - 1;
    const double x = std::clamp(tint, 0.0, 1.0) * last;
    const int i = std::min(int(std::floor(x)), last - 1);
    return kPace[i] + (x - i) * (kPace[i + 1] - kPace[i]);
}

// A monotone cubic through the More Clear, default and More Tinted values.
double glassBlend(double amount, double clearValue, double defaultValue, double tintedValue)
{
    const double d0 = 2.0 * (defaultValue - clearValue);
    const double d1 = 2.0 * (tintedValue - defaultValue);
    const double middle = d0 * d1 > 0 ? 2.0 * d0 * d1 / (d0 + d1) : 0.0;
    const bool upper = amount > 0.5;
    const double t = upper ? 2.0 * amount - 1.0 : 2.0 * amount;
    const double y0 = upper ? defaultValue : clearValue;
    const double y1 = upper ? tintedValue : defaultValue;
    const double m0 = 0.5 * (upper ? middle : d0);
    const double m1 = 0.5 * (upper ? d1 : middle);
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * y0 + (t3 - 2.0 * t2 + t) * m0 + (3.0 * t2 - 2.0 * t3) * y1
        + (t3 - t2) * m1;
}

// A blur slider's position (0 Sharper, 1 Softer, 0.3 the default) as the
// factor the blur is scaled by: a decade each way from the default, over
// the travel on that side.
double glassBlurFactor(double blur)
{
    const double position = std::isfinite(blur) ? std::clamp(blur, 0.0, 1.0) : 0.3;
    const double decades = position < 0.3 ? (position - 0.3) / 0.3 : (position - 0.3) / 0.7;
    return std::pow(10.0, decades);
}

// The widest blur the glass kernel carries (30 device pixels).
constexpr double kMaxBlurSigma = 8.0;

// The shell's settings file, followed for the glass while the program runs.
class ShellGlassTint
{
public:
    static ShellGlassTint& instance()
    {
        static ShellGlassTint tint;
        return tint;
    }

    double tint() const { return m_tint; }
    double blur() const { return m_blur; }
    bool frosted() const { return m_frosted; }
    uint64_t generation() const { return m_generation; }
    base::Signal<> changed;

private:
    ShellGlassTint()
        : m_path(base::paths::cutefishConfig("shell"))
    {
        // A file replaced by a rename or not written yet is followed too.
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), error);
        m_watch = base::FileWatch::file(m_path, [this] { reload(); });
        reload();
    }

    void reload()
    {
        const base::Settings settings(m_path);
        const auto read = [&settings](const char* key, double fallback) {
            const double value = settings.doubleValue(key, fallback);
            return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : fallback;
        };
        const double tint = read("Appearance/GlassTint", 0.5);
        const double blur = read("Appearance/GlassBlur", 0.3);
        const bool frosted = settings.boolValue("Appearance/GlassFrosted", false);
        if (tint == m_tint && blur == m_blur && frosted == m_frosted)
            return;
        m_tint = tint;
        m_blur = blur;
        m_frosted = frosted;
        ++m_generation;
        changed();
    }

    std::string m_path;
    double m_tint = 0.5;
    double m_blur = 0.3;
    bool m_frosted = false;
    uint64_t m_generation = 0;
    base::FileWatch m_watch;
};

} // namespace

double shellGlassTint()
{
    return ShellGlassTint::instance().tint();
}

double shellGlassBlur()
{
    return ShellGlassTint::instance().blur();
}

bool shellGlassFrosted()
{
    return ShellGlassTint::instance().frosted();
}

uint64_t shellGlassGeneration()
{
    return ShellGlassTint::instance().generation();
}

base::Signal<>& shellGlassTintChanged()
{
    return ShellGlassTint::instance().changed;
}

GlassMaterial windowGlass(bool darkMode, double tint)
{
    // The dock's material as it is, its rim included.
    return popupGlass(darkMode, tint);
}

GlassMaterial windowGlass(const Color& tint, double opacity)
{
    GlassMaterial glass = popupGlass(Theme::current().darkMode, shellGlassTint(), shellGlassBlur());
    const double alpha = std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 1.0;
    glass.tintRed = tint.r / 255.0;
    glass.tintGreen = tint.g / 255.0;
    glass.tintBlue = tint.b / 255.0;
    glass.tintAlpha = alpha;
    glass.rim = 1.0 - alpha;
    return glass;
}

// The frost menus and drop-downs had before the sliders: a white or dark
// grey, the lens kept to the lip; the edge is the compositor's outline.
static GlassMaterial frostedPopupGlass(bool darkMode)
{
    GlassMaterial glass;
    glass.blurSigma = 6;
    glass.displacement = 20;
    glass.chroma = 0.03;
    glass.saturation = 1.2;
    if (darkMode) {
        glass.tintRed = 0.17;
        glass.tintGreen = 0.17;
        glass.tintBlue = 0.19;
    } else {
        glass.tintRed = 1;
        glass.tintGreen = 1;
        glass.tintBlue = 1;
    }
    glass.tintAlpha = 0.6;
    return glass;
}

GlassMaterial popupGlass(bool darkMode, double tint, double blur)
{
    // The dock's recipe (cutefish-compositor's tintedGlass), value for value.
    const double amount = glassAmount(tint);
    const auto blend = [amount](double clear, double middle, double tinted) {
        return glassBlend(amount, clear, middle, tinted);
    };
    GlassMaterial glass;
    glass.blurSigma = std::min(kMaxBlurSigma, blend(0.6, 1.0, 8.0) * glassBlurFactor(blur));
    glass.displacement = blend(36.0, 36.0, 0.0);
    // A colour split along the lip reads as a rainbow fringe next to the rim.
    glass.chroma = 0;
    glass.saturation = blend(1.4, 1.3, 1.0);
    glass.rim = 1.0;
    // A grey tint, blended as its grey and its alpha; in the dark theme the
    // theme's own grey. More Tinted reaches the frosted panels' opacity
    // (0.86); the default is the dock's as it was.
    const double alpha = darkMode ? blend(0.12, 0.29, 0.86) : blend(0.16, 0.25, 0.86);
    if (darkMode) {
        const Color background = Theme::current().background;
        glass.tintRed = background.r / 255.0;
        glass.tintGreen = background.g / 255.0;
        glass.tintBlue = background.b / 255.0;
    } else {
        glass.tintRed = glass.tintGreen = glass.tintBlue = std::clamp(blend(0.375, 0.6, 1.0), 0.0, 1.0);
    }
    glass.tintAlpha = std::clamp(alpha, 0.0, 1.0);
    return glass;
}

GlassMaterial popupGlass(bool darkMode, double tint)
{
    return popupGlass(darkMode, tint, shellGlassBlur());
}

GlassMaterial popupGlass(bool darkMode)
{
    if (shellGlassFrosted())
        return frostedPopupGlass(darkMode);
    return popupGlass(darkMode, shellGlassTint());
}

bool PopupGlass::update(Window& window)
{
    if (!window.backgroundGlassAvailable())
        return false;
    // The theme's generation covers its dark mode and its colours.
    const uint64_t theme = Theme::generation();
    const uint64_t generation = shellGlassGeneration();
    if (!m_sent || m_theme != theme || m_generation != generation) {
        m_sent = true;
        m_theme = theme;
        m_generation = generation;
        window.setBackgroundGlass(popupGlass(Theme::current().darkMode));
    }
    return true;
}

void PopupGlass::prepare(Window& window)
{
    window.setBackgroundColor(Color::rgb(0x000000).withAlpha(0));
    window.setTranslucentSubpixelText(!window.backgroundGlassAvailable());
}

} // namespace cutefish::ui
