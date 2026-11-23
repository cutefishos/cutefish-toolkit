// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/view_underlay.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string_view>
#include <unordered_map>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkRegion.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"

#include "ui/painter.h"
#include "ui/view.h"
#include "ui/window.h"
#include "ui/window_backdrop_private.h"

namespace cutefish::ui {

// Kept beside the views, which keep their layout.
static std::unordered_map<const View*, std::vector<ViewUnderlay>>& underlaysOfViews()
{
    static std::unordered_map<const View*, std::vector<ViewUnderlay>> underlays;
    return underlays;
}

static std::atomic<UnderlayDrawnHandler> drawnHandler {nullptr};

void setUnderlayDrawnHandler(UnderlayDrawnHandler handler)
{
    drawnHandler.store(handler);
}

static RectF reachRect(const ViewUnderlay& layer)
{
    return {layer.rect.x - layer.reach, layer.rect.y - layer.reach, layer.rect.width + 2 * layer.reach,
            layer.rect.height + 2 * layer.reach};
}

// The view's untransformed position in the window.
static PointF positionInWindow(const View* view)
{
    PointF position = view->geometry().topLeft();
    for (const View* ancestor = view->parent(); ancestor; ancestor = ancestor->parent())
        position += ancestor->geometry().topLeft();
    return position;
}

void View::setUnderlays(std::vector<ViewUnderlay> underlays)
{
    auto& all = underlaysOfViews();
    const auto known = all.find(this);
    if (known == all.end() && underlays.empty())
        return;
    Window* window = this->window();
    const auto damage = [&](const ViewUnderlay& layer) {
        if (window && layer.opacity > 0)
            window->damageRect(this, reachRect(layer));
    };
    if (known != all.end()) {
        if (known->second == underlays)
            return;
        // A layer whose values changed paints where it was and where it is.
        for (const ViewUnderlay& old : known->second) {
            const auto now = std::find_if(underlays.begin(), underlays.end(),
                                          [&](const ViewUnderlay& layer) { return layer.key == old.key; });
            if (now == underlays.end() || !(*now == old))
                damage(old);
        }
        for (const ViewUnderlay& layer : underlays) {
            const auto before = std::find_if(known->second.begin(), known->second.end(),
                                             [&](const ViewUnderlay& old) { return old.key == layer.key; });
            if (before == known->second.end() || !(*before == layer))
                damage(layer);
        }
    } else {
        for (const ViewUnderlay& layer : underlays)
            damage(layer);
    }
    if (underlays.empty())
        all.erase(known);
    else
        all[this] = std::move(underlays);
}

void View::forgetUnderlays(const View* view)
{
    auto& all = underlaysOfViews();
    if (!all.empty())
        all.erase(view);
}

bool View::hasUnderlays() const
{
    const auto& all = underlaysOfViews();
    return !all.empty() && all.count(this);
}

bool windowHasUnderlays(const Window* window)
{
    for (const auto& entry : underlaysOfViews()) {
        if (entry.first->window() == window)
            return true;
    }
    return false;
}

// Shaders by their source, made once.
static sk_sp<SkRuntimeEffect> effectFor(const std::string& source)
{
    static std::unordered_map<std::string, sk_sp<SkRuntimeEffect>> effects;
    const auto found = effects.find(source);
    if (found != effects.end())
        return found->second;
    SkRuntimeEffect::Result result = SkRuntimeEffect::MakeForShader(SkString(source.c_str()));
    if (!result.effect)
        fprintf(stderr, "cutefish-toolkit: an underlay's shader: %s\n", result.errorText.c_str());
    effects.emplace(source, result.effect);
    return result.effect;
}

// The colour raised to a power, as an input asks before its blur.
static sk_sp<SkImageFilter> raisedBy(float power)
{
    static const sk_sp<SkRuntimeEffect> raising = [] {
        SkRuntimeEffect::Result result = SkRuntimeEffect::MakeForColorFilter(SkString(
            "uniform half power;"
            "half4 main(half4 c) {"
            "    if (c.a <= 0.0) return c;"
            "    return half4(pow(clamp(c.rgb / c.a, 0.0, 1.0), half3(power)) * c.a, c.a);"
            "}"));
        if (!result.effect)
            fprintf(stderr, "cutefish-toolkit: %s\n", result.errorText.c_str());
        return result.effect;
    }();
    if (!raising)
        return nullptr;
    const sk_sp<SkData> data = SkData::MakeWithCopy(&power, sizeof(power));
    return SkImageFilters::ColorFilter(raising->makeColorFilter(data), nullptr);
}

static void told(SkCanvas* canvas, const View* view, uint64_t key, bool after)
{
    const UnderlayDrawnHandler handler = drawnHandler.load();
    if (!handler)
        return;
    if (GrDirectContext* direct = GrAsDirectContext(canvas->recordingContext()))
        direct->flushAndSubmit();
    handler({view, key, after});
}

void View::paintUnderlays(Painter& painter, double opacity) const
{
    const auto& all = underlaysOfViews();
    if (all.empty())
        return;
    const auto known = all.find(this);
    if (known == all.end())
        return;
    SkCanvas* canvas = painter.canvas();
    const SkMatrix total = canvas->getTotalMatrix();
    if (!total.isScaleTranslate())
        return;
    const double scale = painter.scale();
    const PointF offset = painter.offset();
    const auto toDevice = [&](double x, double y) {
        return total.mapPoint({SkScalar((offset.x + x) * scale), SkScalar((offset.y + y) * scale)});
    };
    const float unit = float(scale * std::abs(total.getScaleY()));
    const SkPoint origin = toDevice(0, 0);
    for (const ViewUnderlay& layer : known->second) {
        const double strength = layer.opacity * opacity;
        if (strength <= 0)
            continue;
        const sk_sp<SkRuntimeEffect> effect = effectFor(layer.shader);
        if (!effect)
            continue;
        const SkPoint topLeft = toDevice(layer.rect.x, layer.rect.y);
        const SkPoint bottomRight = toDevice(layer.rect.right(), layer.rect.bottom());
        const SkRect device = SkRect::MakeLTRB(topLeft.x(), topLeft.y(), bottomRight.x(), bottomRight.y());
        told(canvas, this, layer.key, false);
        SkRuntimeShaderBuilder builder(effect);
        if (effect->findUniform("rect"))
            builder.uniform("rect") = SkV4 {device.left(), device.top(), device.right(), device.bottom()};
        if (effect->findUniform("unit"))
            builder.uniform("unit") = unit;
        if (effect->findUniform("origin"))
            builder.uniform("origin") = SkV2 {origin.x(), origin.y()};
        if (effect->findUniform("opacity"))
            builder.uniform("opacity") = float(std::min(strength, 1.0));
        std::vector<float> values;
        for (const UnderlayUniform& uniform : layer.uniforms) {
            if (!effect->findUniform(uniform.name.c_str()))
                continue;
            values = uniform.values;
            if (uniform.kind == UnderlayUniform::Kind::Lengths) {
                for (float& value : values)
                    value *= unit;
            } else if (uniform.kind == UnderlayUniform::Kind::Points) {
                for (size_t i = 0; i + 1 < values.size(); i += 2) {
                    const SkPoint at = toDevice(values[i], values[i + 1]);
                    values[i] = at.x();
                    values[i + 1] = at.y();
                }
            }
            builder.uniform(uniform.name.c_str()).set(values.data(), int(values.size()));
        }
        std::vector<std::string_view> children = {"backdrop"};
        std::vector<sk_sp<SkImageFilter>> inputs = {nullptr};
        for (const UnderlayInput& input : layer.inputs) {
            const float sigma = float(input.blur) * unit / 2;
            children.push_back(input.name);
            if (sigma <= 0) {
                inputs.push_back(nullptr);
                continue;
            }
            const float power = float(std::max(input.power, 0.01));
            inputs.push_back(SkImageFilters::Blur(sigma, sigma, SkTileMode::kClamp,
                                                  power != 1 ? raisedBy(power) : nullptr));
        }
        const float reach = float(layer.reach) * unit;
        const sk_sp<SkImageFilter> filter = SkImageFilters::RuntimeShader(builder, reach, children.data(),
                                                                          inputs.data(), int(children.size()));
        const SkRect bounds = device.makeOutset(reach, reach);
        // The shader gives what lies outside the layer back as it is, so its
        // result replaces what it read.
        SkPaint replace;
        replace.setBlendMode(SkBlendMode::kSrc);
        canvas->save();
        canvas->resetMatrix();
        canvas->clipRect(bounds);
        // The filter reads what the window has composited under the layer.
        canvas->saveLayer(SkCanvas::SaveLayerRec(&bounds, &replace, filter.get(), 0));
        canvas->restore();
        canvas->restore();
        told(canvas, this, layer.key, true);
    }
}

void View::growForUnderlays(SkRegion& region, const Window* window, double scale)
{
    const auto& all = underlaysOfViews();
    if (all.empty() || region.isEmpty())
        return;
    // A layer reads what lies under it, so any change there paints all of it.
    for (const auto& [view, underlays] : all) {
        if (view->window() != window || !view->isVisibleInTree())
            continue;
        const PointF position = positionInWindow(view);
        for (const ViewUnderlay& layer : underlays) {
            const RectF reach = reachRect(layer);
            const SkIRect rect = SkRect::MakeXYWH(SkScalar((position.x + reach.x) * scale),
                                                  SkScalar((position.y + reach.y) * scale),
                                                  SkScalar(reach.width * scale), SkScalar(reach.height * scale))
                                         .roundOut();
            if (region.intersects(rect))
                region.op(rect, SkRegion::kUnion_Op);
        }
    }
}

} // namespace cutefish::ui
