// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/bitmap.h"
#include "ui/color_mixer.h"

#include "ui/view.h"

namespace cutefish::ui {

// One of the toolkit's single-colour glyphs, such as "check", "chevron-down" or
// "search", in a colour; the theme's text colour unless set. Drawn square,
// centred and whole on device pixels.
class Icon : public View
{
public:
    explicit Icon(std::string name = {}, double size = 16);

    // Adds an application's own single-colour glyph, drawn tinted like the
    // built-in ones; the SVG's contents, not a file name.
    static void registerGlyph(const std::string& name, std::string svg);

    void setIconName(std::string name);
    const std::string& iconName() const { return m_name; }
    // Unset is the theme's text colour.
    void setColor(std::optional<ColorRecipe> color);
    void setSize(double size);

    SizeF sizeHint() const override { return {m_size, m_size}; }

protected:
    void paint(Painter& painter) override;

private:
    std::string m_name;
    std::optional<ColorRecipe> m_color;
    double m_size;
};

// An icon from the user's icon theme, by its freedesktop name, such as an
// application's icon.
class ThemeIcon : public View
{
public:
    explicit ThemeIcon(std::string name = {}, double size = 32);

    void setIconName(std::string name);
    const std::string& iconName() const { return m_name; }
    void setSize(double size);
    // The icon as a silhouette in one colour (its alpha times the colour's);
    // unset, or fully transparent, draws it as it is.
    void setColor(std::optional<ColorRecipe> color);
    // The file the theme gives for the name at this size, at the scale.
    std::string path(double scale = 1) const;
    // Looked up as an image provider does: the device-pixel size at the
    // application's integer scale, and the larger pixmap filtered down.
    void setImageProviderLookup(bool provider);
    // The artwork the theme has for this size at 1x (an outline glyph at 22,
    // say), drawn at the icon's size and the window's density; 0 looks up the
    // drawn size at the scale.
    void setThemeSize(int size);

    SizeF sizeHint() const override { return {m_size, m_size}; }

protected:
    void paint(Painter& painter) override;

private:
    std::string m_name;
    double m_size;
    bool m_provider = false;
    int m_themeSize = 0;
    std::optional<ColorRecipe> m_color;
};

// The file the icon theme (its parents, hicolor, the dash fallbacks, then
// /usr/share/pixmaps) gives for the name at a logical size and scale, as
// ThemeIcon draws it; empty when nothing has it. Needs no view or window;
// cached.
std::string themeIconPath(const std::string& name, int size = 16, double scale = 1);

// Makes the theme icons a ThemeIcon of the size draws at the scale ahead of
// time: the lookup now, the rasterising on the global thread pool, the
// images into the icon cache on this thread, then `done` here. Icons cached
// already are skipped; up to 512 icons stay cached.
void preloadThemeIcons(std::vector<std::string> names, int size, double scale, std::function<void()> done = {});
// Forgets the icon themes and the lookups made in them, so that icons
// installed since, such as a new application's, are found.
void reloadThemeIcons();
// A theme icon as a ThemeIcon of the size draws it at the scale, as
// `icon.grabToImage()`: round(size * scale) square, premultiplied RGBA,
// from the cache when it is there; null when the theme has none.
Bitmap themeIconBitmap(const std::string& name, int size, double scale);

} // namespace cutefish::ui
