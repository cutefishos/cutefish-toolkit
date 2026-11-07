// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/icon.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"

#include "ui/icon_loader.h"
#include "ui/painter.h"
#include "ui/theme.h"

namespace cutefish::ui {

// The square, in device pixels, an icon of the size takes in the view: its
// raster centred on the view, then snapped.
static Rect iconRect(const Painter& painter, const RectF& bounds, double size)
{
    const int side = std::max(1, int(std::lround(size * painter.scale())));
    const double extent = side / painter.scale();
    return painter.snappedRect({bounds.x + (bounds.width - extent) / 2, bounds.y + (bounds.height - extent) / 2,
                                extent, extent});
}

Icon::Icon(std::string name, double size)
    : m_name(std::move(name))
    , m_size(size)
{
}

void Icon::registerGlyph(const std::string& name, std::string svg)
{
    IconLoader::instance().registerGlyph(name, std::move(svg));
}

void Icon::setIconName(std::string name)
{
    m_name = std::move(name);
    requestPaint();
}

void Icon::setColor(std::optional<ColorRecipe> color)
{
    m_color = std::move(color);
    requestPaint();
}

void Icon::setSize(double size)
{
    if (size == m_size)
        return;
    m_size = size;
    requestLayout();
}

void Icon::paint(Painter& painter)
{
    if (m_name.empty())
        return;
    const Theme& theme = Theme::current();
    const Color color = isEnabled() ? (m_color ? m_color->resolve() : theme.text) : theme.disabledText;
    const Rect rect = iconRect(painter, localRect(), m_size);
    if (sk_sp<SkImage> image = IconLoader::instance().glyph(m_name, rect.width, color, painter.scale()))
        painter.drawImage(image.get(), rect.x, rect.y);
}

ThemeIcon::ThemeIcon(std::string name, double size)
    : m_name(std::move(name))
    , m_size(size)
{
}

void ThemeIcon::setIconName(std::string name)
{
    m_name = std::move(name);
    requestPaint();
}

void ThemeIcon::setSize(double size)
{
    if (size == m_size)
        return;
    m_size = size;
    requestLayout();
}

std::string themeIconPath(const std::string& name, int size, double scale)
{
    return IconLoader::instance().themeIconPath(name, size, scale);
}

void preloadThemeIcons(std::vector<std::string> names, int size, double scale, std::function<void()> done)
{
    IconLoader::instance().preloadThemeIcons(names, size, scale, std::move(done));
}

void reloadThemeIcons()
{
    IconLoader::instance().reload();
}

Bitmap themeIconBitmap(const std::string& name, int size, double scale)
{
    const sk_sp<SkImage> image = IconLoader::instance().themeIcon(name, size, scale);
    if (!image)
        return {};
    Bitmap bitmap(image->width(), image->height(), Bitmap::Format::Rgba8888Premultiplied);
    if (bitmap.isNull())
        return {};
    const SkImageInfo info = SkImageInfo::Make(image->width(), image->height(), kRGBA_8888_SkColorType,
                                               kPremul_SkAlphaType);
    if (!image->readPixels(info, bitmap.bits(), bitmap.stride(), 0, 0))
        return {};
    return bitmap;
}

std::string ThemeIcon::path(double scale) const
{
    return IconLoader::instance().themeIconPath(m_name, int(m_size), scale);
}

void ThemeIcon::setColor(std::optional<ColorRecipe> color)
{
    if (sameColor(color, m_color))
        return;
    m_color = std::move(color);
    requestPaint();
}

void ThemeIcon::paint(Painter& painter)
{
    if (m_name.empty())
        return;
    const Rect rect = iconRect(painter, localRect(), m_size);
    // Tinted in device pixels with SourceIn, before placing the raster.
    sk_sp<SkColorFilter> tint;
    const Color color = m_color ? m_color->resolve() : Color{0, 0, 0, 0};
    if (color.a > 0)
        tint = SkColorFilters::Blend(SkColorSetARGB(color.a, color.r, color.g, color.b), SkBlendMode::kSrcIn);
    if (m_provider) {
        // At the application's integer scale (the outputs'): the theme is
        // searched at sourceSize × scale at that scale, for a pixmap that many
        // times larger.
        const double scale = painter.scale();
        const int requested = int(std::lround(m_size * scale));
        const int iconScale = int(std::ceil(scale));
        const int lookupSize = requested;
        const int pixels = requested * iconScale;
        if (sk_sp<SkImage> image = IconLoader::instance().themeIcon(m_name, lookupSize, iconScale, pixels)) {
            SkPaint paint;
            paint.setAlphaf(float(painter.opacity()));
            paint.setColorFilter(tint);
            painter.canvas()->drawImageRect(image, SkRect::MakeXYWH(SkScalar(rect.x), SkScalar(rect.y),
                                                                    SkScalar(rect.width), SkScalar(rect.height)),
                                            SkSamplingOptions(SkFilterMode::kLinear), &paint);
        }
        return;
    }
    // The size is taken in whole logical pixels; a theme size picks the
    // artwork at 1x.
    sk_sp<SkImage> found = m_themeSize > 0
            ? IconLoader::instance().themeIcon(m_name, m_themeSize, 1, int(std::lround(m_size * painter.scale())))
            : IconLoader::instance().themeIcon(m_name, int(m_size), painter.scale());
    if (sk_sp<SkImage> image = std::move(found)) {
        if (!tint) {
            painter.drawImage(image.get(), rect.x, rect.y);
            return;
        }
        SkPaint paint;
        paint.setAlphaf(float(painter.opacity()));
        paint.setColorFilter(tint);
        painter.canvas()->drawImage(image.get(), SkScalar(rect.x), SkScalar(rect.y), SkSamplingOptions(), &paint);
    }
}

void ThemeIcon::setThemeSize(int size)
{
    if (size == m_themeSize)
        return;
    m_themeSize = size;
    requestPaint();
}

void ThemeIcon::setImageProviderLookup(bool provider)
{
    m_provider = provider;
    requestPaint();
}

} // namespace cutefish::ui
