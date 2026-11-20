// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <vector>

#include "ui/geometry.h"

namespace cutefish::ui {

// A pane of glass a view stands on, drawn when the window composites, under
// the view's own drawing and over what lies under the view: a rounded rect
// shaded per device pixel from its signed distance to the outline, which
// also anti-aliases the outline, so its edge stays as thin and crisp at any
// scale. What lies under it is blurred, saturated and, within the bezel of
// the edge, read from further in, so it shows magnified there, the colours
// bent apart by the dispersion; then come the tint, `multiply`, white light
// added by the height, a light around the pointer and a colour over all of
// that. A line just outside the outline turns from `edge` along the top
// (`bottomEdge` along the bottom) to `sideEdge` along the sides, by the
// square of the outline's horizontal normal, and a shadow falls outside it.
// Colours are straight RGBA, 0 to 1; lengths are the view's units unless
// they say device pixels.
//
// Panes with the same key in successive View::setGlass() calls move from
// what shows to the new values over the new pane's duration, on the
// window's frames: every value but the light stops, which change at once
// unless both have as many. A pane with a key the last call did not have
// fades in over its duration, and one the call leaves out fades out over
// its own.
struct ViewGlass
{
    uint64_t key = 0;
    RectF rect;
    double radius = 0;
    // The whole pane's strength, shadow included.
    double opacity = 1;

    double tint[4] = {1, 1, 1, 0};
    // Points down from the top of the rect and the light added there, in
    // order; at most 24. They follow the outline in from the top and the
    // bottom, fading out towards the sides.
    std::vector<PointF> light;
    double over[4] = {0, 0, 0, 0};
    double edge[4] = {0, 0, 0, 0};
    double sideEdge[4] = {0, 0, 0, 0};
    double edgeDevicePixels = 1;

    double blur = 0;
    double saturation = 1;
    double bezel = 0;
    // The bezel along the sides, where the outline's normal is horizontal;
    // it turns into `bezel` towards the top and bottom by the square of the
    // normal's vertical part. Negative: `bezel` all round.
    double sideBezel = -1;
    double bend = 0;
    double profile = 2;
    // The share of the bend by which red is bent less and blue more.
    double dispersion = 0;
    // What lies under the middle shows magnified by this.
    double zoom = 1;
    // The colour after the tint is raised to this power, channel by channel.
    double exponent = 1;

    // Laid over the glass within `glowRadius` of the pointer while it is in
    // the window, strongest under it; the edge line brightens by
    // `glowEdge` of the glow there too.
    double glow[4] = {1, 1, 1, 0};
    double glowRadius = 0;
    double glowEdge = 0;
    // Seconds the light takes to catch up with the pointer, easing out.
    double glowLag = 0;

    // What lies under the pane is shown less blurred towards its top and
    // bottom over these lengths, unblurred at the edge.
    double fadeTop = 0;
    double fadeBottom = 0;

    double shadow[4] = {0, 0, 0, 0};
    double shadowBlur = 0;
    double shadowOffset = 0;

    // Seconds to reach these values, along the cubic Bezier curve from (0,
    // 0) to (1, 1) through these two control points; the rect, radius and
    // shadow take `shapeDuration` and `shapeCurve` instead where it is not
    // negative.
    double duration = 0;
    double curve[4] = {0.25, 0.1, 0.25, 1};
    double shapeDuration = -1;
    double shapeCurve[4] = {0.25, 0.1, 0.25, 1};

    // The line along the bottom, where `edge` gives the top's; a negative
    // alpha takes `edge` there too.
    double bottomEdge[4] = {0, 0, 0, -1};
    // The line keeps to whole device pixels: it covers the pixel the outline
    // falls in, so an outline between pixels does not spread it over two.
    bool edgeOnPixels = false;
    // The colour after the tint is multiplied by this, by its alpha.
    double multiply[4] = {1, 1, 1, 0};
    // What lies under the pane is blurred, averaged over blocks of two by two
    // device pixels and scaled back up, so a fine pattern shows wider and
    // shallower than one blur gives it.
    bool blurAtHalfResolution = false;
    // How far inside the outline the bend is largest, easing in from nothing
    // at the outline over that depth; 0: largest at the outline.
    double bendPeak = 0;
    // How much the dispersion keeps to where the outline's normal is
    // horizontal, 0 to 1: 0 all round, 1 along the sides only.
    double dispersionSides = 0;
    // The shadow eases in over this length outside the outline, so it is
    // darkest that far out rather than at the outline.
    double shadowGap = 0;
    // How much the bend reads across by the depth from the sides and up or
    // down by the depth from the top or bottom, as through a rectangular
    // slab, rather than along the outline's normal, 0 to 1.
    double bendAlongAxes = 0;
    // The side line's red reaches this many device pixels further in than
    // its other colours, most where the outline faces sideways.
    double edgeSplit = 0;
    // How far inside the sides the bend along the axes is largest; negative:
    // `bendPeak`.
    double sideBendPeak = -1;
    // Past what black and white under the pane come to after the tint and
    // `multiply`, the power's curve goes on as straight lines of these
    // slopes, below and above; 0 leaves the curve as it is there.
    double exponentSlopes[2] = {0, 0};
    // What lies under the pane is blurred raised to this power and the blur
    // raised back, so where light and dark are averaged a power above 1
    // leans to the light.
    double blurPower = 1;
    // With a profile, the blur within fadeTop and fadeBottom follows it
    // rather than fading out evenly: points of the share of the way from the
    // edge (0) to where the fade ends (1) and the blur shown there, in
    // order, at most 8. It is shown as none, `fadeBlur` and `blur` mixed.
    double fadeBlur = 0;
    std::vector<PointF> fadeProfile;

    bool operator==(const ViewGlass&) const = default;
};

} // namespace cutefish::ui
