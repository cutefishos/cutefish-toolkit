// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Property lists, the XML and binary (bplist00) files application bundles
// describe themselves in.
namespace cutefish::base::plist {

// A value as far as bundles need one: strings, booleans, arrays and
// dictionaries, with every other type standing in as Other.
struct Value
{
    enum class Type { Other, String, Boolean, Array, Dictionary };

    Type type = Type::Other;
    std::string text;
    bool boolean = false;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> members;

    // The first member under key; null when there is none or this is no
    // dictionary.
    const Value* find(std::string_view key) const;
    // The member's string; empty when it has none.
    std::string string(std::string_view key) const;
    // Whether the member is true; Info.plist keys may also be the string YES
    // or 1.
    bool flag(std::string_view key) const;
    // The string, or the strings of an array: Info.plist keys take either.
    std::vector<std::string> strings() const;
};

// The whole list. Nothing when the data is neither form, or nests deeper
// than 64 levels or holds over a million values, as a binary list whose
// objects refer back to themselves would; a binary object that cannot be
// read is Other.
std::optional<Value> read(std::string_view data);

// The string values of the root dictionary, as UTF-8; values of other types
// are left out. Nothing when the data is neither form or its root is not a
// dictionary.
std::optional<std::map<std::string, std::string>> rootStrings(std::string_view data);

} // namespace cutefish::base::plist
