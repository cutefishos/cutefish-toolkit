// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <string>

namespace cutefish::ui {

// Files built into the library, such as the glyph icons under data/icons
// and the runtime shaders under data/shaders.
struct EmbeddedFile
{
    const char* name;
    const unsigned char* data;
    size_t size;
};

const EmbeddedFile* findEmbeddedFile(const char* name);

// A built-in text file, such as a runtime shader's source under data/shaders;
// empty when there is none.
inline std::string embeddedText(const char* name)
{
    const EmbeddedFile* file = findEmbeddedFile(name);
    return file ? std::string(reinterpret_cast<const char*>(file->data), file->size) : std::string();
}

} // namespace cutefish::ui
