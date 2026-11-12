// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "base/transform.h"
#include "ui/path.h"

namespace cutefish::ui {

struct PathAccess
{
    // Points mapped in double precision before they go to single precision.
    static SkPath toSkPath(const Path& path);
    static SkPath toSkPath(const Path& path, double scale, PointF offset);
    static SkPath toSkPath(const Path& path, const base::AffineTransform& transform);
    static Path fromSkPath(const SkPath& path);

private:
    template<typename Map>
    static SkPath convert(const Path& path, Map map);
    template<typename Map>
    static bool roundedRect(const Path& path, Map map, SkRRect* rrect, SkPathDirection* direction,
                            unsigned* start);
};

} // namespace cutefish::ui
