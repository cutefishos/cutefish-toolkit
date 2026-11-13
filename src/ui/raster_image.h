// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class SkImage;

namespace cutefish::ui {

// Pixels drawn frame after frame, such as rendered pages: taken over once,
// so drawing neither copies nor compares them and the GPU keeps its upload.
// Made on any thread; copies share the pixels.
class RasterImage
{
public:
    RasterImage() = default;
    // 0xAARRGGBB words, row by row, premultiplied or not.
    RasterImage(int width, int height, std::vector<uint32_t> pixels, bool premultiplied);

    bool isNull() const { return !m_image; }
    int width() const;
    int height() const;
    size_t sizeInBytes() const { return size_t(width()) * size_t(height()) * 4; }

    // For Painter::drawImage.
    const SkImage* skImage() const;

private:
    std::shared_ptr<const SkImage> m_image;
};

} // namespace cutefish::ui
