// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

namespace cutefish::ui {

// What a viewer needs of an image before decoding it, from its header: the
// pixel size after the EXIF orientation, whether it moves, whether it is
// drawn from vectors.
struct ImageInfo
{
    // The file is an image the toolkit decodes.
    bool readable = false;
    // 0 when the header does not say, or says something impossible.
    int width = 0;
    int height = 0;
    bool animated = false;
    bool vector = false;
};

// The types readImageInfo() takes, those the toolkit decodes, by their
// canonical names.
const std::vector<std::string>& imageMimeTypes();
ImageInfo readImageInfo(const std::string& path, const std::string& mimeType);

} // namespace cutefish::ui
