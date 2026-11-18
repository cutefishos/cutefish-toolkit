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
// the edge, bent in from further out, the colours bent apart by the
// dispersion; then come the tint, white light whose strength follows the
// height, a light around the pointer, a colour over all of that, and a line
// just inside the outline whose colour turns from `edge` along the top and
// bottom to `sideEdge` along the sides, by the square of the outline's
// horizontal normal. A shadow falls outside the outline. Colours are
// straight RGBA, 0 to 1; lengths are the view's units unless they say
// device pixels.
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
    // Points down from the top of the rect and the light's alpha there, in
    // order; at most 24.
    std::vector<PointF> light;
    double over[4] = {0, 0, 0, 0};
    double edge[4] = {0, 0, 0, 0};
    double sideEdge[4] = {0, 0, 0, 0};
    double edgeDevicePixels = 1;

    double blur = 0;
    double saturation = 1;
    double bezel = 0;
    double bend = 0;
    double profile = 2;
    // The share of the bend by which red is bent less and blue more.
    double dispersion = 0;
    // What lies under the middle shows magnified by this.
    double zoom = 1;

    // Laid over the glass within `glowRadius` of the pointer while it is in
    // the window, strongest under it; the edge line brightens by
    // `glowEdge` of the glow there too.
    double glow[4] = {1, 1, 1, 0};
    double glowRadius = 0;
    double glowEdge = 0;
    // Seconds the light takes to catch up with the pointer, easing out.
    double glowLag = 0;

    // The pane fades out towards its top and bottom over these lengths,
    // what lies under it shown less blurred there too.
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

    bool operator==(const ViewGlass&) const = default;
};

} // namespace cutefish::ui
