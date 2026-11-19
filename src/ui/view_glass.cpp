// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/view_glass.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "include/core/SkCanvas.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkRegion.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#include "ui/embedded_files.h"
#include "ui/frame_client.h"
#include "ui/painter.h"
#include "ui/view.h"
#include "ui/window.h"

namespace cutefish::ui {

// A pane as it moves from one set of values to the next.
struct GlassTrack
{
    ViewGlass from;
    ViewGlass to;
    ViewGlass shown;
    // The frame the move began on; 0 until the next frame.
    uint64_t start = 0;
    bool moving = false;
    bool leaving = false;
};

struct GlassOfView
{
    std::vector<GlassTrack> tracks;
};

// Kept beside the views, which keep their layout.
static std::unordered_map<const View*, GlassOfView>& glassOfViews()
{
    static std::unordered_map<const View*, GlassOfView> glass;
    return glass;
}

// Runs the moving panes of one window on its frames.
class GlassClock : public FrameClient
{
public:
    explicit GlassClock(Window* window)
        : FrameClient(window)
    {
    }

    void run() { startTicking(); }

protected:
    void tick(uint64_t frameTimeNs) override
    {
        if (!View::tickGlass(window(), frameTimeNs))
            stopTicking();
    }
};

static std::unordered_map<const Window*, std::unique_ptr<GlassClock>>& glassClocks()
{
    static std::unordered_map<const Window*, std::unique_ptr<GlassClock>> clocks;
    return clocks;
}

// Where the light of panes that lag behind the pointer is, in the window.
struct GlassPointer
{
    PointF shown;
    bool placed = false;
    uint64_t frame = 0;
};

static std::unordered_map<const Window*, GlassPointer>& glassPointers()
{
    static std::unordered_map<const Window*, GlassPointer> pointers;
    return pointers;
}

static bool glows(const ViewGlass& pane)
{
    return pane.glow[3] > 0 && pane.glowRadius > 0 && pane.opacity > 0;
}

// How far past its rect a pane draws or reads, in its units.
static double reachOf(const ViewGlass& glass)
{
    return std::max(glass.shadowBlur + std::abs(glass.shadowOffset) + glass.shadowGap,
               std::abs(glass.bend) + glass.blur * 2) + 1;
}

static RectF reachRect(const ViewGlass& glass)
{
    const double reach = reachOf(glass);
    return {glass.rect.x - reach, glass.rect.y - reach, glass.rect.width + 2 * reach,
            glass.rect.height + 2 * reach};
}

// The view's untransformed position in the window.
static PointF positionInWindow(const View* view)
{
    PointF position = view->geometry().topLeft();
    for (const View* ancestor = view->parent(); ancestor; ancestor = ancestor->parent())
        position += ancestor->geometry().topLeft();
    return position;
}

// Where the cubic Bezier curve from (0, 0) to (1, 1) through the control
// points is at x.
static double eased(const double* curve, double x)
{
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    const auto at = [](double a, double b, double t) {
        const double u = 1 - t;
        return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
    };
    const auto slope = [](double a, double b, double t) {
        const double u = 1 - t;
        return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
    };
    double t = x;
    for (int i = 0; i < 8; ++i) {
        const double error = at(curve[0], curve[2], t) - x;
        const double derivative = slope(curve[0], curve[2], t);
        if (std::abs(error) < 1e-6 || std::abs(derivative) < 1e-6)
            break;
        t = std::clamp(t - error / derivative, 0.0, 1.0);
    }
    // Bisect where Newton's steps stalled.
    if (std::abs(at(curve[0], curve[2], t) - x) > 1e-4) {
        double low = 0, high = 1;
        for (int i = 0; i < 40; ++i) {
            t = (low + high) / 2;
            if (at(curve[0], curve[2], t) < x)
                low = t;
            else
                high = t;
        }
    }
    return at(curve[1], curve[3], t);
}

static double mixed(double a, double b, double t)
{
    return a + (b - a) * t;
}

// Premultiplied, so a colour fading out keeps its hue on the way and the
// pane passes no shade darker or lighter than both ends.
static void mixColour(double* into, const double* a, const double* b, double t)
{
    const double alpha = mixed(a[3], b[3], t);
    for (int i = 0; i < 3; ++i) {
        const double premultiplied = mixed(a[i] * a[3], b[i] * b[3], t);
        into[i] = alpha > 0 ? premultiplied / alpha : mixed(a[i], b[i], t);
    }
    into[3] = alpha;
}

static const double* bottomEdgeOf(const ViewGlass& glass)
{
    return glass.bottomEdge[3] < 0 ? glass.edge : glass.bottomEdge;
}

// The same pane, however it was asked to get there.
static bool sameLook(const ViewGlass& a, const ViewGlass& b)
{
    ViewGlass c = b;
    c.duration = a.duration;
    c.shapeDuration = a.shapeDuration;
    std::copy(a.shapeCurve, a.shapeCurve + 4, c.shapeCurve);
    std::copy(a.curve, a.curve + 4, c.curve);
    return a == c;
}

// `t` along the way for the colours and light, `s` for the shape.
static ViewGlass between(const ViewGlass& a, const ViewGlass& b, double t, double s)
{
    ViewGlass glass = b;
    glass.rect = {mixed(a.rect.x, b.rect.x, s), mixed(a.rect.y, b.rect.y, s), mixed(a.rect.width, b.rect.width, s),
                  mixed(a.rect.height, b.rect.height, s)};
    glass.radius = mixed(a.radius, b.radius, s);
    glass.opacity = mixed(a.opacity, b.opacity, t);
    mixColour(glass.tint, a.tint, b.tint, t);
    if (a.light.size() == b.light.size()) {
        for (size_t i = 0; i < b.light.size(); ++i)
            glass.light[i] = {mixed(a.light[i].x, b.light[i].x, t), mixed(a.light[i].y, b.light[i].y, t)};
    }
    mixColour(glass.over, a.over, b.over, t);
    mixColour(glass.edge, a.edge, b.edge, t);
    mixColour(glass.bottomEdge, bottomEdgeOf(a), bottomEdgeOf(b), t);
    mixColour(glass.sideEdge, a.sideEdge, b.sideEdge, t);
    glass.edgeDevicePixels = mixed(a.edgeDevicePixels, b.edgeDevicePixels, t);
    glass.blur = mixed(a.blur, b.blur, t);
    glass.saturation = mixed(a.saturation, b.saturation, t);
    glass.bezel = mixed(a.bezel, b.bezel, t);
    glass.sideBezel = mixed(a.sideBezel, b.sideBezel, t);
    glass.bend = mixed(a.bend, b.bend, t);
    glass.profile = mixed(a.profile, b.profile, t);
    glass.dispersion = mixed(a.dispersion, b.dispersion, t);
    glass.zoom = mixed(a.zoom, b.zoom, t);
    glass.exponent = mixed(a.exponent, b.exponent, t);
    mixColour(glass.multiply, a.multiply, b.multiply, t);
    glass.bendPeak = mixed(a.bendPeak, b.bendPeak, t);
    glass.dispersionSides = mixed(a.dispersionSides, b.dispersionSides, t);
    glass.shadowGap = mixed(a.shadowGap, b.shadowGap, s);
    glass.bendAlongAxes = mixed(a.bendAlongAxes, b.bendAlongAxes, t);
    glass.edgeSplit = mixed(a.edgeSplit, b.edgeSplit, t);
    glass.sideBendPeak = mixed(a.sideBendPeak, b.sideBendPeak, t);
    mixColour(glass.glow, a.glow, b.glow, t);
    glass.glowRadius = mixed(a.glowRadius, b.glowRadius, t);
    glass.glowEdge = mixed(a.glowEdge, b.glowEdge, t);
    glass.glowLag = mixed(a.glowLag, b.glowLag, t);
    glass.fadeTop = mixed(a.fadeTop, b.fadeTop, t);
    glass.fadeBottom = mixed(a.fadeBottom, b.fadeBottom, t);
    mixColour(glass.shadow, a.shadow, b.shadow, t);
    glass.shadowBlur = mixed(a.shadowBlur, b.shadowBlur, s);
    glass.shadowOffset = mixed(a.shadowOffset, b.shadowOffset, s);
    return glass;
}

// Where the track is at the frame time, from its start.
static void advance(GlassTrack& track, uint64_t frameTimeNs)
{
    const double elapsed = double(frameTimeNs - std::min(frameTimeNs, track.start)) / 1e9;
    const auto progress = [elapsed](double duration) {
        return duration > 0 ? std::min(elapsed / duration, 1.0) : 1.0;
    };
    const double t = progress(track.to.duration);
    const double s = track.to.shapeDuration < 0 ? t : progress(track.to.shapeDuration);
    if (t >= 1 && s >= 1) {
        track.shown = track.to;
        track.moving = false;
        return;
    }
    const double* shapeCurve = track.to.shapeDuration < 0 ? track.to.curve : track.to.shapeCurve;
    track.shown = between(track.from, track.to, eased(track.to.curve, t), eased(shapeCurve, s));
}

void View::setGlass(std::vector<ViewGlass> glass)
{
    auto& all = glassOfViews();
    auto known = all.find(this);
    if (known == all.end()) {
        if (glass.empty())
            return;
        known = all.emplace(this, GlassOfView {}).first;
    }
    GlassOfView& state = known->second;
    Window* window = this->window();
    const auto damagePane = [window](const View* view, const ViewGlass& pane) {
        if (window && pane.opacity > 0)
            window->damageRect(view, reachRect(pane));
    };

    std::vector<GlassTrack> tracks;
    bool moves = false;
    for (const ViewGlass& pane : glass) {
        GlassTrack track;
        track.to = pane;
        const auto previous = std::find_if(state.tracks.begin(), state.tracks.end(), [&](const GlassTrack& old) {
            return old.to.key == pane.key && !old.leaving;
        });
        const bool animates = (pane.duration > 0 || pane.shapeDuration > 0) && window;
        if (previous != state.tracks.end()) {
            if (sameLook(previous->to, pane)) {
                tracks.push_back(*previous);
                moves = moves || previous->moving;
                continue;
            }
            track.from = previous->shown;
            damagePane(this, previous->shown);
        } else {
            track.from = pane;
            track.from.opacity = 0;
        }
        if (animates) {
            track.shown = track.from;
            track.moving = true;
            moves = true;
            // Declared while a frame is drawn, the move shows its first
            // refresh in that frame.
            if (window->m_rendering && window->m_frameTimeNs > window->m_refreshNs) {
                track.start = window->m_frameTimeNs - window->m_refreshNs;
                advance(track, window->m_frameTimeNs);
            }
        } else {
            track.shown = pane;
        }
        damagePane(this, track.shown);
        tracks.push_back(std::move(track));
    }
    for (const GlassTrack& old : state.tracks) {
        const bool kept = std::any_of(glass.begin(), glass.end(), [&](const ViewGlass& pane) {
            return pane.key == old.to.key;
        });
        if (kept && !old.leaving)
            continue;
        damagePane(this, old.shown);
        if ((old.to.duration <= 0 && old.to.shapeDuration <= 0) || !window || (old.leaving && !old.moving))
            continue;
        GlassTrack leaving = old;
        if (!old.leaving) {
            leaving.from = old.shown;
            leaving.to = old.to;
            leaving.to.opacity = 0;
            leaving.start = 0;
            leaving.leaving = true;
        }
        leaving.moving = true;
        moves = true;
        tracks.push_back(std::move(leaving));
    }
    state.tracks = std::move(tracks);
    if (state.tracks.empty()) {
        all.erase(known);
        return;
    }
    if (moves && window) {
        auto& clocks = glassClocks();
        auto& clock = clocks[window];
        if (!clock)
            clock = std::make_unique<GlassClock>(window);
        clock->run();
    }
}

void View::forgetGlass(const View* view)
{
    auto& all = glassOfViews();
    if (!all.empty())
        all.erase(view);
}

void View::forgetGlassOf(const Window* window)
{
    auto& clocks = glassClocks();
    if (!clocks.empty())
        clocks.erase(window);
    auto& pointers = glassPointers();
    if (!pointers.empty())
        pointers.erase(window);
}

bool View::tickGlass(Window* window, uint64_t frameTimeNs)
{
    const auto damagePane = [window](const View* view, const ViewGlass& pane) {
        if (pane.opacity > 0)
            window->damageRect(view, reachRect(pane));
    };
    bool moving = false;
    for (auto it = glassOfViews().begin(); it != glassOfViews().end();) {
        GlassOfView& state = it->second;
        const View* view = it->first;
        if (view->window() != window) {
            ++it;
            continue;
        }
        for (GlassTrack& track : state.tracks) {
            if (!track.moving)
                continue;
            // The first frame that shows a move shows it one refresh along.
            if (!track.start)
                track.start = frameTimeNs - std::min(frameTimeNs, window->m_refreshNs);
            damagePane(view, track.shown);
            advance(track, frameTimeNs);
            moving = moving || track.moving;
            damagePane(view, track.shown);
        }
        std::erase_if(state.tracks, [](const GlassTrack& track) { return track.leaving && !track.moving; });
        if (state.tracks.empty())
            it = glassOfViews().erase(it);
        else
            ++it;
    }

    // The light that lags eases towards the pointer, about 95 % of the way
    // in its lag.
    double lag = 0;
    for (const auto& [view, state] : glassOfViews()) {
        if (view->window() != window)
            continue;
        for (const GlassTrack& track : state.tracks) {
            if (glows(track.shown))
                lag = std::max(lag, track.shown.glowLag);
        }
    }
    const auto found = glassPointers().find(window);
    if (found != glassPointers().end() && lag > 0 && window->m_pointerInside) {
        GlassPointer& pointer = found->second;
        const PointF target = window->m_pointer;
        if (!pointer.placed) {
            pointer.shown = target;
            pointer.placed = true;
        } else {
            const double elapsed = double(frameTimeNs - std::min(pointer.frame, frameTimeNs)) / 1e9;
            const double share = 1 - std::exp(-elapsed * 3 / lag);
            pointer.shown = {mixed(pointer.shown.x, target.x, share), mixed(pointer.shown.y, target.y, share)};
        }
        pointer.frame = frameTimeNs;
        if (std::abs(pointer.shown.x - target.x) > 0.05 || std::abs(pointer.shown.y - target.y) > 0.05)
            moving = true;
        else
            pointer.shown = target;
        for (const auto& [view, state] : glassOfViews()) {
            if (view->window() != window)
                continue;
            for (const GlassTrack& track : state.tracks) {
                if (glows(track.shown) && track.shown.glowLag > 0)
                    window->damageRect(view, reachRect(track.shown));
            }
        }
    }
    return moving;
}

void View::glassPointerMoved(Window* window, PointF from, bool wasInside)
{
    const auto& all = glassOfViews();
    if (all.empty())
        return;
    bool lags = false;
    for (const auto& [view, state] : all) {
        if (view->window() != window)
            continue;
        const PointF position = positionInWindow(view);
        for (const GlassTrack& track : state.tracks) {
            const ViewGlass& pane = track.shown;
            if (!glows(pane))
                continue;
            const RectF near {position.x + pane.rect.x - pane.glowRadius, position.y + pane.rect.y - pane.glowRadius,
                              pane.rect.width + 2 * pane.glowRadius, pane.rect.height + 2 * pane.glowRadius};
            if ((wasInside && near.contains(from)) || (window->m_pointerInside && near.contains(window->m_pointer))) {
                window->damageRect(view, reachRect(pane));
                lags = lags || pane.glowLag > 0;
            }
        }
    }
    GlassPointer& pointer = glassPointers()[window];
    if (!window->m_pointerInside) {
        pointer.placed = false;
        return;
    }
    if (!lags)
        return;
    if (!pointer.placed) {
        pointer.shown = window->m_pointer;
        pointer.placed = true;
    }
    auto& clock = glassClocks()[window];
    if (!clock)
        clock = std::make_unique<GlassClock>(window);
    clock->run();
}

bool View::hasGlass() const
{
    const auto& all = glassOfViews();
    return !all.empty() && all.count(this);
}

void View::paintGlass(Painter& painter, double opacity) const
{
    const auto& all = glassOfViews();
    if (all.empty())
        return;
    const auto known = all.find(this);
    if (known == all.end())
        return;
    static const sk_sp<SkRuntimeEffect> effect = [] {
        // data/shaders/glass.sksl, in device pixels; `backdrop` is what lies
        // under the pane, `blurred` the same blurred.
        SkRuntimeEffect::Result result =
            SkRuntimeEffect::MakeForShader(SkString(embeddedText("glass.sksl").c_str()));
        if (!result.effect)
            fprintf(stderr, "cutefish-toolkit: %s\n", result.errorText.c_str());
        return result.effect;
    }();
    SkCanvas* canvas = painter.canvas();
    const SkMatrix total = canvas->getTotalMatrix();
    if (!effect || !total.isScaleTranslate())
        return;
    const double scale = painter.scale();
    const PointF offset = painter.offset();
    const Window* window = this->window();
    // The pointer, and where the light lagging behind it is, in the view's
    // coordinates.
    std::optional<PointF> pointer;
    std::optional<PointF> lagging;
    if (window && window->m_pointerInside) {
        const PointF position = positionInWindow(this);
        pointer = PointF {window->m_pointer.x - position.x, window->m_pointer.y - position.y};
        lagging = pointer;
        const auto found = glassPointers().find(window);
        if (found != glassPointers().end() && found->second.placed)
            lagging = PointF {found->second.shown.x - position.x, found->second.shown.y - position.y};
    }
    const auto toDevice = [&](double x, double y) {
        return total.mapPoint({SkScalar((offset.x + x) * scale), SkScalar((offset.y + y) * scale)});
    };
    for (const GlassTrack& track : known->second.tracks) {
        const ViewGlass& glass = track.shown;
        const double strength = glass.opacity * opacity;
        if (strength <= 0)
            continue;
        const SkPoint topLeft = toDevice(glass.rect.x, glass.rect.y);
        const SkPoint bottomRight = toDevice(glass.rect.right(), glass.rect.bottom());
        const SkRect device = SkRect::MakeLTRB(topLeft.x(), topLeft.y(), bottomRight.x(), bottomRight.y());
        const float unit = float(scale * std::abs(total.getScaleY()));
        SkRuntimeShaderBuilder builder(effect);
        builder.uniform("rect") = SkV4 {device.left(), device.top(), device.right(), device.bottom()};
        builder.uniform("radius") = float(glass.radius) * unit;
        builder.uniform("opacity") = float(std::min(strength, 1.0));
        const auto colour = [](const double* c) {
            return SkV4 {float(c[0]), float(c[1]), float(c[2]), float(c[3])};
        };
        builder.uniform("tint") = colour(glass.tint);
        // Unused stops repeat the last.
        float light[48];
        const size_t count = std::min<size_t>(glass.light.size(), 24);
        for (size_t i = 0; i < 24; ++i) {
            const PointF stop = count ? glass.light[std::min(i, count - 1)] : PointF {0, 0};
            light[2 * i] = float(stop.x) * unit;
            light[2 * i + 1] = float(stop.y);
        }
        builder.uniform("light").set(light, 48);
        builder.uniform("over") = colour(glass.over);
        builder.uniform("edge") = colour(glass.edge);
        builder.uniform("bottomEdge") = colour(bottomEdgeOf(glass));
        builder.uniform("sideEdge") = colour(glass.sideEdge);
        builder.uniform("edgeWidth") = float(glass.edgeDevicePixels);
        builder.uniform("edgeOnPixels") = glass.edgeOnPixels ? 1.0f : 0.0f;
        builder.uniform("multiply") = colour(glass.multiply);
        builder.uniform("halfResolution") = glass.blurAtHalfResolution ? 1.0f : 0.0f;
        builder.uniform("saturation") = float(glass.saturation);
        builder.uniform("bezel") = float(glass.bezel) * unit;
        builder.uniform("sideBezel") = glass.sideBezel < 0 ? -1.0f : float(glass.sideBezel) * unit;
        builder.uniform("bend") = float(glass.bend) * unit;
        builder.uniform("profile") = float(std::max(glass.profile, 0.01));
        builder.uniform("dispersion") = float(glass.dispersion);
        builder.uniform("zoom") = float(std::max(glass.zoom, 0.01));
        builder.uniform("exponent") = float(std::max(glass.exponent, 0.01));
        builder.uniform("glow") = colour(glass.glow);
        if (pointer && glass.glowRadius > 0) {
            const PointF light = glass.glowLag > 0 ? *lagging : *pointer;
            const SkPoint at = toDevice(light.x, light.y);
            builder.uniform("glowAt") = SkV3 {at.x(), at.y(), float(glass.glowRadius) * unit};
        } else {
            builder.uniform("glowAt") = SkV3 {0, 0, 0};
        }
        builder.uniform("glowEdge") = float(glass.glowEdge);
        builder.uniform("fade") = SkV2 {float(glass.fadeTop) * unit, float(glass.fadeBottom) * unit};
        builder.uniform("shadow") = colour(glass.shadow);
        builder.uniform("shadowBlur") = std::max(float(glass.shadowBlur) * unit, 0.5f);
        builder.uniform("shadowOffset") = float(glass.shadowOffset) * unit;
        builder.uniform("bendPeak") = float(glass.bendPeak) * unit;
        builder.uniform("dispersionSides") = float(std::clamp(glass.dispersionSides, 0.0, 1.0));
        builder.uniform("shadowGap") = float(glass.shadowGap) * unit;
        builder.uniform("bendAlongAxes") = float(std::clamp(glass.bendAlongAxes, 0.0, 1.0));
        builder.uniform("edgeSplit") = float(glass.edgeSplit);
        builder.uniform("sideBendPeak") = glass.sideBendPeak < 0 ? -1.0f : float(glass.sideBendPeak) * unit;

        const float sigma = float(glass.blur) * unit / 2;
        std::string_view children[] = {"backdrop", "blurred"};
        const sk_sp<SkImageFilter> inputs[] = {
            nullptr, sigma > 0 ? SkImageFilters::Blur(sigma, sigma, SkTileMode::kClamp, nullptr) : nullptr};
        const float reach = float(reachOf(glass)) * unit;
        const sk_sp<SkImageFilter> filter = SkImageFilters::RuntimeShader(builder, reach, children, inputs, 2);
        const SkRect bounds = device.makeOutset(reach, reach);
        // The shader gives what lies outside the pane back as it is, so its
        // result replaces what it read.
        SkPaint replace;
        replace.setBlendMode(SkBlendMode::kSrc);
        canvas->save();
        canvas->resetMatrix();
        canvas->clipRect(bounds);
        // The filter reads what the window has composited under the pane.
        canvas->saveLayer(SkCanvas::SaveLayerRec(&bounds, &replace, filter.get(), 0));
        canvas->restore();
        canvas->restore();
    }
}

void View::growForGlass(SkRegion& region, const Window* window, double scale)
{
    const auto& all = glassOfViews();
    if (all.empty() || region.isEmpty())
        return;
    // A pane reads what lies under it, so any change there paints all of it.
    for (const auto& [view, state] : all) {
        if (view->window() != window || !view->isVisibleInTree())
            continue;
        const PointF position = positionInWindow(view);
        for (const GlassTrack& track : state.tracks) {
            const RectF reach = reachRect(track.shown);
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
