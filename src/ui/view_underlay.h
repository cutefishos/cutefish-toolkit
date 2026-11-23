// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/geometry.h"

namespace cutefish::ui {

class View;

// What lies under an underlay, as one of its shader's children: blurred by
// `blur` (the view's units; the Gaussian's sigma is half of it, 0 leaves it
// sharp), its colour raised to `power` first.
struct UnderlayInput
{
    std::string name;
    double blur = 0;
    double power = 1;

    bool operator==(const UnderlayInput&) const = default;
};

// A float, vector or array uniform of an underlay's shader, by name, with
// its values as they are, as lengths in the view's units (each times the
// device pixels per unit), or as points in the view's coordinates (pairs,
// each in device pixels).
struct UnderlayUniform
{
    enum class Kind
    {
        Plain,
        Lengths,
        Points,
    };

    std::string name;
    std::vector<float> values;
    Kind kind = Kind::Plain;

    bool operator==(const UnderlayUniform&) const = default;
};

// A layer a view stands on, drawn when the window composites, under the
// view's drawing and over what lies under the view. Within `rect` grown by
// `reach` (the view's coordinates), `shader` gives the pixels, and they
// replace what lay there. The shader is SkSL for a shader effect, its main
// taking the device position; it reads what lies under it as the child
// `backdrop` and as each input, takes `uniforms`, and the toolkit gives
// these where it declares them: `rect` (float4: left, top, right, bottom in
// device pixels), `unit` (device pixels per unit of the view), `origin`
// (float2: the view's top left in device pixels) and `opacity` (the view's
// opacity times the layer's, at most 1). A layer at opacity 0 is not drawn.
struct ViewUnderlay
{
    uint64_t key = 0;
    RectF rect;
    double reach = 0;
    double opacity = 1;
    std::string shader;
    std::vector<UnderlayInput> inputs;
    std::vector<UnderlayUniform> uniforms;

    bool operator==(const ViewUnderlay&) const = default;
};

// A layer a frame is about to draw (`after` false) or has drawn. While a
// handler is set, the work the frame issued so far is submitted to the GPU
// before each call, with the frame's context current, so a host can time
// the layers; it costs a submission per call.
struct UnderlayDrawn
{
    const View* view = nullptr;
    uint64_t key = 0;
    bool after = false;
};
using UnderlayDrawnHandler = void (*)(const UnderlayDrawn& layer);
void setUnderlayDrawnHandler(UnderlayDrawnHandler handler);

} // namespace cutefish::ui
