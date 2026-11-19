// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/color_provider.h"
#include "ui/header_backdrop.h"

#include <algorithm>
#include <cstdio>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkRegion.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkRuntimeEffect.h"

#include "ui/application.h"
#include "ui/embedded_files.h"
#include "ui/multi_effect_blur.h"
#include "ui/painter.h"
#include "ui/text_context.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"
#include "ui/window.h"

namespace cutefish::ui {

// Content below the bar feeds the blur, so its edge does not fade out.
static constexpr double kBlurPadding = 32;
static constexpr double kFadeDistance = 16;
static constexpr double kTintAlpha = 0.72;
// A blurMax of 32 takes four blur levels.
static constexpr int kBlurItems = 4;
static constexpr double kBlurMax = 32;

// The blur as last made, and what it was made from: on the CPU the finished
// image, on the GPU the blur items the effect draws from. Kept outside the
// class, whose layout the applications are built against.
struct BlurCache
{
    sk_sp<SkImage> image;
    std::vector<sk_sp<SkImage>> items;
    GrRecordingContext* context = nullptr;
    uint64_t changes = 0;
    double scale = 0;
    SkISize size;
    PointF viewPosition;
    PointF contentPosition;
    int firstRow = 0;
    int lastRow = 0;
};

static std::unordered_map<const HeaderBackdrop*, BlurCache> s_blurs;
// The window each backdrop's damage watch is in: a backdrop taken out of its
// window before it goes no longer finds the window through its parents.
static std::unordered_map<const HeaderBackdrop*, Window*> s_watchedWindows;

static bool isOpen(const Window* window)
{
    const std::vector<Window*>& windows = Application::instance()->windows();
    return std::find(windows.begin(), windows.end(), window) != windows.end();
}

// The same passes as runtime shaders, when the window draws on the GPU.
static sk_sp<SkRuntimeEffect> makeEffect(const char* code)
{
    SkRuntimeEffect::Result result = SkRuntimeEffect::MakeForShader(SkString(code));
    if (!result.effect)
        fprintf(stderr, "cutefish-toolkit: %s\n", result.errorText.c_str());
    return result.effect;
}

static sk_sp<SkShader> textureShader(const sk_sp<SkImage>& image)
{
    return image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions(SkFilterMode::kLinear));
}

// The blur items of the source on the GPU, as textures; none when the
// shaders or the surfaces cannot be made.
static std::vector<sk_sp<SkImage>> makeBlurItemsOnGpu(SkSurface* source, double width, double height, double scale)
{
    static const sk_sp<SkRuntimeEffect> blurItem = makeEffect(embeddedText("header_blur_item.sksl").c_str());
    if (!blurItem)
        return {};
    sk_sp<SkImage> previous = source->makeImageSnapshot();
    std::vector<sk_sp<SkImage>> blurs;
    double itemWidth = std::ceil(width / 16) * 8;
    double itemHeight = std::ceil(height / 16) * 8;
    for (int i = 0; i < kBlurItems; ++i) {
        const int pixelWidth = textureLength(itemWidth, scale);
        const int pixelHeight = textureLength(itemHeight, scale);
        sk_sp<SkSurface> surface = source->makeSurface(pixelWidth, pixelHeight);
        if (!surface)
            return {};
        SkRuntimeShaderBuilder builder(blurItem);
        builder.child("source") = textureShader(previous);
        builder.uniform("sourceSize") = SkV2{float(previous->width()), float(previous->height())};
        builder.uniform("outputSize") = SkV2{float(pixelWidth), float(pixelHeight)};
        builder.uniform("offset") = SkV2{float(1 / itemWidth), float(1 / itemHeight)};
        SkPaint paint;
        paint.setShader(builder.makeShader());
        paint.setBlendMode(SkBlendMode::kSrc);
        surface->getCanvas()->drawRect(SkRect::MakeIWH(pixelWidth, pixelHeight), paint);
        previous = surface->makeImageSnapshot();
        blurs.push_back(previous);
        itemWidth /= 2;
        itemHeight /= 2;
    }
    return blurs;
}

// The effect over the blur items, drawn at the device position for the rows
// from first to last of a source of the size.
static void drawCompositeOnGpu(SkCanvas* canvas, const std::vector<sk_sp<SkImage>>& blurs, SkISize sourceSize,
                               SkPoint origin, int firstRow, int lastRow, float opacity)
{
    static const sk_sp<SkRuntimeEffect> compositeEffect = makeEffect(embeddedText("header_composite.sksl").c_str());
    if (!compositeEffect || blurs.empty())
        return;
    // The source's own weight is nil at blur 1.
    const std::array<double, 6> weights = multiEffectBlurWeights(kBlurMax, 1);
    SkRuntimeShaderBuilder builder(compositeEffect);
    const char* const children[] = {"blur1", "blur2", "blur3", "blur4"};
    const char* const sizes[] = {"size1", "size2", "size3", "size4"};
    for (size_t i = 0; i < blurs.size(); ++i) {
        builder.child(children[i]) = textureShader(blurs[i]);
        builder.uniform(sizes[i]) = SkV2{float(blurs[i]->width()), float(blurs[i]->height())};
    }
    builder.uniform("outputSize") = SkV2{float(sourceSize.width()), float(sourceSize.height())};
    builder.uniform("weights") = SkV4{float(weights[1]), float(weights[2]), float(weights[3]), float(weights[4])};
    SkPaint paint;
    paint.setShader(builder.makeShader());
    paint.setAlphaf(opacity);
    canvas->save();
    canvas->translate(origin.fX, origin.fY);
    canvas->drawRect(SkRect::MakeLTRB(0, float(firstRow), float(sourceSize.width()), float(lastRow)), paint);
    canvas->restore();
}

HeaderBackdrop::HeaderBackdrop(ScrollView* view, Edge edge)
    : m_view(view)
    , m_edge(edge)
{
    m_scrolled = view->scrolled.connect([this] { requestPaint(); });
}

HeaderBackdrop::~HeaderBackdrop()
{
    s_blurs.erase(this);
    if (const auto it = s_watchedWindows.find(this); it != s_watchedWindows.end()) {
        if (isOpen(it->second))
            it->second->unwatchDamage(this);
        s_watchedWindows.erase(it);
    }
}

void HeaderBackdrop::setSeparatorVisible(bool visible)
{
    m_separatorVisible = visible;
    requestPaint();
}

void HeaderBackdrop::setBaseColor(std::optional<ColorRecipe> color)
{
    m_baseColor = std::move(color);
    requestPaint();
}

void HeaderBackdrop::setTintColor(std::optional<ColorRecipe> color)
{
    m_tintColor = std::move(color);
    requestPaint();
}

double HeaderBackdrop::fade() const
{
    const double position = m_view->scrollPosition().y;
    const double end = m_view->contentSize().height - m_view->geometry().height;
    const double distance = m_edge == Edge::Bottom ? end - position : position;
    return std::clamp(distance / kFadeDistance, 0.0, 1.0);
}

void HeaderBackdrop::paint(Painter& painter)
{
    const double opacity = fade();
    View* content = m_view->contentView();
    if (opacity <= 0 || !content)
        return;
    const Theme& theme = Theme::current();
    const RectF bounds = localRect();
    const bool bottom = m_edge == Edge::Bottom;
    painter.save();
    painter.multiplyOpacity(opacity);
    painter.clip(bounds);

    // The view's content is still drawn beneath; an opaque base hides it.
    painter.fillRect(bounds, m_baseColor ? m_baseColor->resolve() : theme.background);

    // The source: the content alone, from where the bar lies over the view, the
    // padding on the content's side.
    const double scale = painter.scale();
    const double width = bounds.width;
    const double height = bounds.height + kBlurPadding;
    const PointF viewPosition = mapToWindow({}) - m_view->mapToWindow({});
    const SkImageInfo info = SkImageInfo::MakeN32Premul(textureLength(width, scale), textureLength(height, scale));
    // Only the rows the clip leaves: the padding lies outside the bar.
    const int padding = info.height() - textureLength(bounds.height, scale);
    const int firstRow = bottom ? std::max(0, padding) : 0;
    const int lastRow = bottom ? info.height() : info.height() - std::max(0, padding);
    const PointF origin = painter.offset() + PointF{0, bottom ? -kBlurPadding : 0};
    const SkPoint deviceOrigin = SkPoint::Make(SkScalar(std::round(origin.x * scale)),
                                               SkScalar(std::round(origin.y * scale)));

    // The blur stays while the content under the bar paints as it did.
    // Damage is tracked untransformed, as the content paints into the source.
    SkCanvas* canvas = painter.canvas();
    GrRecordingContext* context = canvas->recordingContext();
    BlurCache* cached = nullptr;
    uint64_t changes = 0;
    bool untransformed = true;
    for (const View* view = content; view; view = view->parent())
        untransformed = untransformed && view->transform().isIdentity();
    Window* window = this->window();
    if (window && untransformed) {
        PointF viewOrigin;
        for (const View* view = m_view; view; view = view->parent())
            viewOrigin += view->geometry().topLeft();
        const PointF sourceOrigin = viewOrigin + viewPosition + PointF{0, bottom ? -kBlurPadding : 0};
        const SkIRect sourceRect = SkIRect::MakeLTRB(int(std::floor(sourceOrigin.x * scale)) - 1,
                                                     int(std::floor(sourceOrigin.y * scale)) - 1,
                                                     int(std::ceil((sourceOrigin.x + width) * scale)) + 1,
                                                     int(std::ceil((sourceOrigin.y + height) * scale)) + 1);
        Window*& watched = s_watchedWindows[this];
        if (watched && watched != window && isOpen(watched))
            watched->unwatchDamage(this);
        watched = window;
        changes = window->watchDamage(this, content, sourceRect);
        cached = &s_blurs[this];
        const bool current = cached->context == context && cached->changes == changes && cached->scale == scale
                && cached->size == info.dimensions() && cached->viewPosition == viewPosition
                && cached->contentPosition == content->geometry().topLeft() && cached->firstRow == firstRow
                && cached->lastRow == lastRow;
        if (!current)
            *cached = {};
    }

    const float alpha = float(painter.opacity());
    if (cached && !context && cached->image) {
        SkPaint paint;
        paint.setAlphaf(alpha);
        canvas->drawImage(cached->image, deviceOrigin.fX, deviceOrigin.fY + float(firstRow), SkSamplingOptions(),
                          &paint);
    } else if (cached && context && !cached->items.empty()) {
        drawCompositeOnGpu(canvas, cached->items, info.dimensions(), deviceOrigin, firstRow, lastRow, alpha);
    } else {
        // Greyscale text, as in any texture that starts transparent.
        const SkSurfaceProps properties = Application::instance()->text().surfaceProps(false);
        // Made by the window's canvas, so layered views draw from their textures.
        sk_sp<SkSurface> surface = canvas->makeSurface(info, &properties);
        if (!surface)
            surface = SkSurfaces::Raster(info, &properties);
        if (surface) {
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            Painter source(surface->getCanvas(), scale);
            source.translate({-viewPosition.x, -(viewPosition.y - (bottom ? kBlurPadding : 0))});
            const SkRegion sourceRegion(SkIRect::MakeWH(info.width(), info.height()));
            content->paintTree(source, &sourceRegion);
        }

        BlurCache made;
        SkBitmap pixels;
        SkPixmap pixmap;
        if (surface && surface->recordingContext()) {
            made.items = makeBlurItemsOnGpu(surface.get(), width, height, scale);
            drawCompositeOnGpu(canvas, made.items, info.dimensions(), deviceOrigin, firstRow, lastRow, alpha);
        } else if (surface && pixels.tryAllocPixels(info) && surface->readPixels(pixels.pixmap(), 0, 0)
                   && pixels.peekPixels(&pixmap)) {
            const Texture texture = readTexture(pixmap);
            // The first blur item is half the source, in multiples of eight;
            // each next one half the one before.
            std::vector<Texture> blurs;
            double itemWidth = std::ceil(width / 16) * 8;
            double itemHeight = std::ceil(height / 16) * 8;
            for (int i = 0; i < kBlurItems; ++i) {
                blurs.push_back(blurPass(i == 0 ? texture : blurs.back(), itemWidth, itemHeight, scale));
                itemWidth /= 2;
                itemHeight /= 2;
            }

            made.image = composite(texture, blurs, multiEffectBlurWeights(kBlurMax, 1), firstRow, lastRow).asImage();
            SkPaint paint;
            paint.setAlphaf(alpha);
            canvas->drawImage(made.image, deviceOrigin.fX, deviceOrigin.fY + float(firstRow), SkSamplingOptions(),
                              &paint);
        }
        // A surface the GPU canvas could not make falls back to the CPU: its
        // image is not one this canvas keeps.
        if (cached && bool(context) == bool(surface && surface->recordingContext())) {
            made.context = context;
            made.changes = changes;
            made.scale = scale;
            made.size = info.dimensions();
            made.viewPosition = viewPosition;
            made.contentPosition = content->geometry().topLeft();
            made.firstRow = firstRow;
            made.lastRow = lastRow;
            *cached = std::move(made);
        }
    }

    painter.fillRect(bounds, (m_tintColor ? m_tintColor->resolve() : theme.secondBackground).withAlpha(kTintAlpha));

    // A 0.5 px hairline; below 2x a device pixel with its alpha scaled to
    // the same weight.
    if (m_separatorVisible) {
        const double devicePixels = std::max(1.0, std::round(0.5 * scale));
        const double lineHeight = devicePixels / scale;
        const Color separator = color(ColorId::HeaderSeparator);
        painter.drawRoundedRect({0, bottom ? 0 : bounds.height - lineHeight, bounds.width, lineHeight}, {0},
                                separator.withAlpha(separator.a / 255.0 * 0.5 * scale / devicePixels));
    }
    painter.restore();
}

} // namespace cutefish::ui
