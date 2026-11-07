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

// The shell's settings file, followed for the tint while the program runs.
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
        if (tint == m_tint && blur == m_blur)
            return;
        m_tint = tint;
        m_blur = blur;
        ++m_generation;
        changed();
    }

    std::string m_path;
    double m_tint = 0.5;
    double m_blur = 0.3;
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
    GlassMaterial glass = popupGlass(Theme::current().darkMode, 0.0, shellGlassBlur());
    const double alpha = std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 1.0;
    glass.tintRed = tint.r / 255.0;
    glass.tintGreen = tint.g / 255.0;
    glass.tintBlue = tint.b / 255.0;
    glass.tintAlpha = alpha;
    glass.rim = 1.0 - alpha;
    return glass;
}

// From 0.3 to 8, the widest sigma the 30 device pixel kernel carries,
// geometrically, so 0.3 of the travel is the 0.8 the dock had.
static double glassBlurSigma(double blur)
{
    return 0.3 * std::pow(8.0 / 0.3, std::clamp(blur, 0.0, 1.0));
}

GlassMaterial popupGlass(bool, double tint, double blur)
{
    // The lens is the dock's and does not move with the Glass effect slider:
    // that sets only how much of the theme's background colour tints it.
    GlassMaterial glass;
    glass.blurSigma = glassBlurSigma(blur);
    glass.displacement = 36;
    // A colour split along the lip reads as a rainbow fringe next to the rim.
    glass.chroma = 0;
    glass.saturation = 1.35;
    // The lit rim fades with the tint: a near-solid surface has no shine.
    const double amount = glassAmount(tint);
    glass.rim = glassBlend(amount, 1.0, 0.6, 0.0);
    const double alpha = glassBlend(amount, 0.10, 0.35, 0.95);
    const Color background = Theme::current().background;
    glass.tintRed = background.r / 255.0;
    glass.tintGreen = background.g / 255.0;
    glass.tintBlue = background.b / 255.0;
    glass.tintAlpha = std::clamp(alpha, 0.0, 1.0);
    return glass;
}

GlassMaterial popupGlass(bool darkMode, double tint)
{
    return popupGlass(darkMode, tint, shellGlassBlur());
}

GlassMaterial popupGlass(bool darkMode)
{
    return popupGlass(darkMode, shellGlassTint());
}

bool PopupGlass::update(Window& window)
{
    if (!window.backgroundGlassAvailable())
        return false;
    const bool darkMode = Theme::current().darkMode;
    // m_tint holds the generation: the blur changes the material too.
    const double generation = double(shellGlassGeneration());
    if (m_darkMode != darkMode || m_tint != generation) {
        m_darkMode = darkMode;
        m_tint = generation;
        window.setBackgroundGlass(popupGlass(darkMode));
    }
    return true;
}

void PopupGlass::prepare(Window& window)
{
    window.setBackgroundColor(Color::rgb(0x000000).withAlpha(0));
    window.setTranslucentSubpixelText(!window.backgroundGlassAvailable());
}

} // namespace cutefish::ui
