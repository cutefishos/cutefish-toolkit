// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>

namespace cutefish::ui {

// A fontconfig file read after the system's configuration in place of the
// user's, by the text contexts made afterwards: text that renders the same
// whoever's home it runs in. Call it before the application is made.
void setTextConfigurationFile(const std::string& path);

} // namespace cutefish::ui
