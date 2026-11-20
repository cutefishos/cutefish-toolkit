// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <string_view>

#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"

namespace cutefish::ui {

enum class ImageScaling {
    // The box exactly.
    Stretch,
    // Inside the box, the aspect kept.
    Fit,
    // Covering the box, the aspect kept.
    Cover,
};

struct ImageRequest
{
    std::string path;
    // The file's bytes instead of the path, which may still name the type by
    // its extension. Must outlive the call.
    std::string_view data;
    // The box in device pixels; 0 x 0 keeps the image's own size.
    int width = 0;
    int height = 0;
    ImageScaling scaling = ImageScaling::Fit;
    // Small images grow to the box as well, as thumbnails do.
    bool enlarge = false;
    // Turns the pixels as the file's EXIF orientation says; HEIF and JPEG XL
    // are always turned, as their decoders do.
    bool autoTransform = false;
};

struct DecodedImage
{
    sk_sp<SkImage> image;
    // As the file has it, after turning.
    int originalWidth = 0;
    int originalHeight = 0;
};

// Reads PNG, JPEG, BMP, ICO and WBMP through Skia, WebP, TIFF, HEIF, AVIF and
// JPEG XL through their libraries, GIF (its first frame), Netpbm, XBM, TGA
// and ICNS through the toolkit's own readers, and SVG; on any thread. The image is null
// when the file cannot be read or holds more than 256 MiB of pixels.
DecodedImage decodeImage(const ImageRequest& request);

// The size scaled into the box: Fit keeps the aspect inside it, Cover
// keeps the aspect covering it, truncated to whole pixels.
void scaledSize(int width, int height, int boxWidth, int boxHeight, ImageScaling scaling, int* outWidth,
                int* outHeight);

} // namespace cutefish::ui
