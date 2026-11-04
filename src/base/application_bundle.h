// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/desktop_entry.h"

// Application bundles: folders named <name>.app that hold Contents/Info.plist
// and run as one program, Contents/MacOS/<CFBundleExecutable>. A desktop
// entry whose MimeType lists kType runs them.
namespace cutefish::base::bundle {

inline constexpr char kType[] = "application/x-apple-app";

// Where installed bundles live: ~/Applications, then /Applications.
std::vector<std::string> applicationsDirectories();

// The path absolute and without a trailing slash when its name ends in .app;
// empty otherwise. Reads nothing.
std::string bundlePath(const std::string& path);
// The bundle whose Contents/MacOS the program is in, by the path alone;
// empty for any other program.
std::string bundleOfProgram(const std::string& program);

// The bundle at path as an application that handler starts with the path,
// the documents to open after it: named by CFBundleDisplayName, CFBundleName
// or the folder's own name, its icon Contents/Resources/<CFBundleIconFile>,
// its MIME types those of its document types, "group/*" standing for a
// whole group. Nothing when the folder holds no Info.plist that can be read.
std::optional<DesktopEntry> read(const std::string& path, const DesktopEntry& handler);
// Whether a bundle's entry opens files of the type: the type is one of its
// MIME types or descends from one, or is in a group it opens.
bool opensType(const DesktopEntry& entry, const std::string& type);

} // namespace cutefish::base::bundle
