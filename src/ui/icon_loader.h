// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"

#include "ui/geometry.h"

namespace cutefish::ui {

// Icons rasterised at the exact device size they are drawn at, so they land on
// whole pixels at every scale, and kept for reuse.
class IconLoader
{
public:
    static IconLoader& instance();

    // One of the toolkit's single-colour glyphs, in the colour, at a device
    // size; the scale tells its logical size, which picks the heavier
    // -small chevrons at 12 and below.
    sk_sp<SkImage> glyph(const std::string& name, int size, Color color, double scale = 1);
    // An icon of the theme, its parents or hicolor, for an item of the
    // logical size at the scale: the file the lookup picks, at the device
    // size, a smaller picture stretched as an item stretches it; null when
    // none has it. An application's own icon from outside the theme sits on
    // the theme's plate when the theme has one.
    sk_sp<SkImage> themeIcon(const std::string& name, int size, double scale);
    // themeIcon(name, size, scale) for each name ahead of time: the lookup
    // here, the rasterising on the global pool, the images stored on this
    // thread, then done.
    void preloadThemeIcons(const std::vector<std::string>& names, int size, double scale,
                           std::function<void()> done);
    // The theme searched at size and the integer scale, rasterised at pixels.
    sk_sp<SkImage> themeIcon(const std::string& name, int size, int iconScale, int pixels);
    // The theme's icon for a name, or the file at an absolute path,
    // as a pixmap `pixels` square at a device pixel ratio of 1: drawings at
    // that size, larger pictures scaled down, smaller ones as they are, ICNS
    // icons with their plate as large as the theme's; null when nothing is
    // found.
    sk_sp<SkImage> pixmap(const std::string& name, int pixels);
    // The file the theme lookup settles on, for tests.
    std::string themeIconPath(const std::string& name, int size, double scale);
    // An SVG file filling the size, tinted when a tint is given.
    sk_sp<SkImage> svg(const std::string& path, int width, int height, const Color* tint);
    // The same drawing, uncached, on any thread, as image views decode files.
    static sk_sp<SkImage> renderSvgFile(const std::string& path, int width, int height);
    // An application's own single-colour glyph, drawn as the built-in ones.
    void registerGlyph(const std::string& name, std::string svg);

private:
    struct Directory
    {
        std::string path;
        int size = 0;
        int minimum = 0;
        int maximum = 0;
        int threshold = 2;
        int scale = 1;
        // Fixed, Scalable or Threshold.
        char type = 'T';
        // Applications and MimeTypes are left out for a shortened name.
        bool generic = true;
        bool applications = false;
    };

    // A theme base's GTK icon-theme.cache: the directories holding a name, in
    // the cache's order, with the suffixes the files there have.
    class GtkCache
    {
    public:
        struct Image
        {
            std::string directory;
            bool png = false;
            bool svg = false;
        };

        explicit GtkCache(const std::string& base);
        bool isValid() const { return m_valid; }
        std::vector<Image> lookup(const std::string& name);

    private:
        uint16_t read16(size_t offset);
        uint32_t read32(size_t offset);

        std::vector<unsigned char> m_data;
        bool m_valid = false;
    };

    struct IconTheme
    {
        std::vector<std::string> bases;
        std::vector<std::shared_ptr<GtkCache>> caches;
        std::vector<Directory> directories;
        std::vector<std::string> parents;
        // Where the theme may still appear.
        std::vector<std::string> missing;
        bool loaded = false;
        bool found = false;
    };

    // A file the lookup settles on, and the theme's plate when the file is an
    // application's own icon from outside the theme.
    struct Resolved
    {
        std::string path;
        std::string plate;
    };

    IconTheme& theme(const std::string& name);
    std::string lookup(const std::string& name, const std::string& requested, int size, int scale, bool* foreign);
    Resolved resolve(const std::string& name, int size, double scale);
    // Touches nothing of the loader: any thread.
    static sk_sp<SkImage> rasterise(const std::string& path, int size, const Color* tint,
                                    const std::string& plate = {});
    std::unordered_map<std::string, std::string> m_glyphs;
    sk_sp<SkImage> cached(const std::string& key);
    void store(const std::string& key, sk_sp<SkImage> image);

    std::unordered_map<std::string, IconTheme> m_themes;
    std::unordered_map<std::string, Resolved> m_paths;
    // Least recently used last.
    std::list<std::pair<std::string, sk_sp<SkImage>>> m_images;
    std::unordered_map<std::string, std::list<std::pair<std::string, sk_sp<SkImage>>>::iterator> m_index;
};

} // namespace cutefish::ui
