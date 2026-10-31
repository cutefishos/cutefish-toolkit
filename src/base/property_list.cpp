// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/property_list.h"

#include <cstdint>
#include <cstdlib>

namespace cutefish::base::plist {

using Strings = std::map<std::string, std::string>;

static constexpr int kMaximumDepth = 64;
static constexpr size_t kMaximumValues = 1 << 20;

static void appendUtf8(std::string* out, uint32_t code)
{
    if (code < 0x80) {
        *out += char(code);
    } else if (code < 0x800) {
        *out += char(0xc0 | (code >> 6));
        *out += char(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        *out += char(0xe0 | (code >> 12));
        *out += char(0x80 | ((code >> 6) & 0x3f));
        *out += char(0x80 | (code & 0x3f));
    } else {
        *out += char(0xf0 | (code >> 18));
        *out += char(0x80 | ((code >> 12) & 0x3f));
        *out += char(0x80 | ((code >> 6) & 0x3f));
        *out += char(0x80 | (code & 0x3f));
    }
}

class XmlReader
{
public:
    explicit XmlReader(std::string_view text)
        : m_text(text)
    {
    }

    struct Tag
    {
        std::string name;
        bool closing = false;
        bool empty = false;
    };

    // The next element tag, past text, comments, declarations and
    // processing instructions; nothing at the end or on malformed markup.
    std::optional<Tag> nextTag()
    {
        while (true) {
            const size_t open = m_text.find('<', m_position);
            if (open == std::string_view::npos)
                return std::nullopt;
            m_position = open;
            if (skipped("<!--", "-->") || skipped("<?", "?>") || skipped("<![CDATA[", "]]>"))
                continue;
            if (m_text.substr(m_position).starts_with("<!")) {
                if (!skipDeclaration())
                    return std::nullopt;
                continue;
            }
            const size_t close = m_text.find('>', m_position);
            if (close == std::string_view::npos)
                return std::nullopt;
            std::string_view inside = m_text.substr(m_position + 1, close - m_position - 1);
            m_position = close + 1;

            Tag tag;
            if (inside.starts_with('/')) {
                tag.closing = true;
                inside.remove_prefix(1);
            }
            if (inside.ends_with('/')) {
                tag.empty = true;
                inside.remove_suffix(1);
            }
            const size_t end = inside.find_first_of(" \t\r\n");
            tag.name = std::string(inside.substr(0, end));
            if (tag.name.empty())
                return std::nullopt;
            return tag;
        }
    }

    // The text up to the closing tag of the element just opened.
    std::optional<std::string> text(const std::string& name)
    {
        const std::string closing = "</" + name;
        std::string result;
        while (true) {
            const size_t next = m_text.find('<', m_position);
            if (next == std::string_view::npos)
                return std::nullopt;
            if (!decode(m_text.substr(m_position, next - m_position), &result))
                return std::nullopt;
            m_position = next;
            if (m_text.substr(m_position).starts_with("<![CDATA[")) {
                const size_t end = m_text.find("]]>", m_position);
                if (end == std::string_view::npos)
                    return std::nullopt;
                result += m_text.substr(m_position + 9, end - m_position - 9);
                m_position = end + 3;
                continue;
            }
            if (skipped("<!--", "-->"))
                continue;
            if (!m_text.substr(m_position).starts_with(closing))
                return std::nullopt;
            const std::optional<Tag> tag = nextTag();
            if (!tag || !tag->closing || tag->name != name)
                return std::nullopt;
            return result;
        }
    }

    // Past the end of the element just opened.
    bool skipElement(const Tag& opened)
    {
        if (opened.empty)
            return true;
        int depth = 1;
        while (depth > 0) {
            const std::optional<Tag> tag = nextTag();
            if (!tag)
                return false;
            if (tag->closing)
                --depth;
            else if (!tag->empty)
                ++depth;
        }
        return true;
    }

private:
    bool skipped(std::string_view start, std::string_view end)
    {
        if (!m_text.substr(m_position).starts_with(start))
            return false;
        const size_t found = m_text.find(end, m_position + start.size());
        m_position = found == std::string_view::npos ? m_text.size() : found + end.size();
        return true;
    }

    // <!DOCTYPE ...>, whose internal subset in brackets may hold '>'.
    bool skipDeclaration()
    {
        int brackets = 0;
        for (size_t i = m_position + 2; i < m_text.size(); ++i) {
            if (m_text[i] == '[') {
                ++brackets;
            } else if (m_text[i] == ']') {
                --brackets;
            } else if (m_text[i] == '>' && brackets <= 0) {
                m_position = i + 1;
                return true;
            }
        }
        return false;
    }

    static bool decode(std::string_view text, std::string* out)
    {
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '&') {
                *out += text[i];
                continue;
            }
            const size_t end = text.find(';', i);
            if (end == std::string_view::npos)
                return false;
            const std::string_view entity = text.substr(i + 1, end - i - 1);
            if (entity == "amp") {
                *out += '&';
            } else if (entity == "lt") {
                *out += '<';
            } else if (entity == "gt") {
                *out += '>';
            } else if (entity == "quot") {
                *out += '"';
            } else if (entity == "apos") {
                *out += '\'';
            } else if (entity.starts_with('#') && entity.size() > 1) {
                const bool hex = entity[1] == 'x' || entity[1] == 'X';
                const std::string digits(entity.substr(hex ? 2 : 1));
                char* parsed = nullptr;
                const unsigned long code = std::strtoul(digits.c_str(), &parsed, hex ? 16 : 10);
                if (digits.empty() || *parsed || code == 0 || code > 0x10ffff || (code >= 0xd800 && code < 0xe000))
                    return false;
                appendUtf8(out, uint32_t(code));
            } else {
                return false;
            }
            i = end;
        }
        return true;
    }

    std::string_view m_text;
    size_t m_position = 0;
};

// The element the tag opened, which the reader is just past.
static std::optional<Value> readXmlValue(XmlReader& reader, const XmlReader::Tag& opened, int depth, size_t* budget)
{
    if (depth > kMaximumDepth || *budget == 0)
        return std::nullopt;
    --*budget;

    Value value;
    if (opened.name == "string") {
        std::optional<std::string> text = opened.empty ? std::string() : reader.text("string");
        if (!text)
            return std::nullopt;
        value.type = Value::Type::String;
        value.text = std::move(*text);
        return value;
    }

    if (opened.name == "array") {
        value.type = Value::Type::Array;
        if (opened.empty)
            return value;
        while (true) {
            const std::optional<XmlReader::Tag> tag = reader.nextTag();
            if (!tag)
                return std::nullopt;
            if (tag->closing)
                return tag->name == "array" ? std::optional<Value>(std::move(value)) : std::nullopt;
            std::optional<Value> element = readXmlValue(reader, *tag, depth + 1, budget);
            if (!element)
                return std::nullopt;
            value.array.push_back(std::move(*element));
        }
    }

    if (opened.name == "dict") {
        value.type = Value::Type::Dictionary;
        if (opened.empty)
            return value;
        while (true) {
            const std::optional<XmlReader::Tag> key = reader.nextTag();
            if (!key)
                return std::nullopt;
            if (key->closing && key->name == "dict")
                return value;
            if (key->closing || key->name != "key")
                return std::nullopt;
            std::optional<std::string> name = key->empty ? std::string() : reader.text("key");
            if (!name)
                return std::nullopt;

            const std::optional<XmlReader::Tag> tag = reader.nextTag();
            if (!tag || tag->closing)
                return std::nullopt;
            std::optional<Value> member = readXmlValue(reader, *tag, depth + 1, budget);
            if (!member)
                return std::nullopt;
            value.members.emplace_back(std::move(*name), std::move(*member));
        }
    }

    if (opened.name == "true" || opened.name == "false") {
        value.type = Value::Type::Boolean;
        value.boolean = opened.name == "true";
    }

    if (!reader.skipElement(opened))
        return std::nullopt;
    return value;
}

static std::optional<Value> readXml(std::string_view data)
{
    XmlReader reader(data);
    std::optional<XmlReader::Tag> root = reader.nextTag();
    if (root && root->name == "plist" && !root->closing && !root->empty)
        root = reader.nextTag();
    if (!root || root->closing)
        return std::nullopt;
    size_t budget = kMaximumValues;
    return readXmlValue(reader, *root, 0, &budget);
}

class BinaryReader
{
public:
    explicit BinaryReader(std::string_view data)
        : m_data(data)
    {
    }

    std::optional<Value> read()
    {
        if (m_data.size() < 8 + 32)
            return std::nullopt;
        const size_t trailer = m_data.size() - 32;
        m_offsetSize = uint8_t(m_data[trailer + 6]);
        m_referenceSize = uint8_t(m_data[trailer + 7]);
        m_objectCount = number(trailer + 8, 8);
        const uint64_t top = number(trailer + 16, 8);
        m_offsetTable = number(trailer + 24, 8);
        if (m_offsetSize < 1 || m_offsetSize > 8 || m_referenceSize < 1 || m_referenceSize > 8
            || m_offsetTable >= trailer || m_objectCount > (trailer - m_offsetTable) / m_offsetSize)
            return std::nullopt;

        std::optional<Value> root = value(top, 0);
        if (!root || m_exhausted)
            return std::nullopt;
        return root;
    }

private:
    // Nothing for an object that cannot be read.
    std::optional<Value> value(uint64_t object, int depth)
    {
        if (depth > kMaximumDepth || m_budget == 0) {
            m_exhausted = true;
            return std::nullopt;
        }
        --m_budget;

        const std::optional<size_t> offset = objectOffset(object);
        if (!offset)
            return std::nullopt;
        const uint8_t marker = uint8_t(m_data[*offset]);
        const uint8_t type = marker >> 4;
        Value result;
        if (marker == 0x08 || marker == 0x09) {
            result.type = Value::Type::Boolean;
            result.boolean = marker == 0x09;
            return result;
        }
        if (type == 0x5 || type == 0x6) {
            std::optional<std::string> text = string(object);
            if (!text)
                return std::nullopt;
            result.type = Value::Type::String;
            result.text = std::move(*text);
            return result;
        }
        if (type != 0xa && type != 0xd)
            return result;

        size_t start = 0;
        const std::optional<uint64_t> count = length(*offset, &start);
        const uint64_t references = type == 0xd ? 2 : 1;
        if (!count || *count > (m_offsetTable - start) / (references * m_referenceSize))
            return std::nullopt;

        if (type == 0xa) {
            result.type = Value::Type::Array;
            for (uint64_t i = 0; i < *count && !m_exhausted; ++i) {
                std::optional<Value> element = value(number(start + i * m_referenceSize, m_referenceSize), depth + 1);
                result.array.push_back(element ? std::move(*element) : Value());
            }
            return result;
        }

        result.type = Value::Type::Dictionary;
        for (uint64_t i = 0; i < *count && !m_exhausted; ++i) {
            std::optional<std::string> key = string(number(start + i * m_referenceSize, m_referenceSize));
            if (!key)
                return std::nullopt;
            const uint64_t reference = number(start + (*count + i) * m_referenceSize, m_referenceSize);
            std::optional<Value> member = value(reference, depth + 1);
            result.members.emplace_back(std::move(*key), member ? std::move(*member) : Value());
        }
        return result;
    }

    uint64_t number(size_t position, size_t size) const
    {
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i)
            value = value << 8 | uint8_t(m_data[position + i]);
        return value;
    }

    std::optional<size_t> objectOffset(uint64_t object) const
    {
        if (object >= m_objectCount)
            return std::nullopt;
        const uint64_t offset = number(size_t(m_offsetTable + object * m_offsetSize), m_offsetSize);
        if (offset < 8 || offset >= m_offsetTable)
            return std::nullopt;
        return size_t(offset);
    }

    // The element count in an object's marker, or in the integer after it;
    // start is where the elements begin.
    std::optional<uint64_t> length(size_t offset, size_t* start) const
    {
        const uint8_t info = uint8_t(m_data[offset]) & 0xf;
        if (info != 0xf) {
            *start = offset + 1;
            return info;
        }
        if (offset + 2 > m_offsetTable)
            return std::nullopt;
        const uint8_t marker = uint8_t(m_data[offset + 1]);
        if ((marker >> 4) != 0x1 || (marker & 0xf) > 3)
            return std::nullopt;
        const size_t size = size_t(1) << (marker & 0xf);
        if (offset + 2 + size > m_offsetTable)
            return std::nullopt;
        *start = offset + 2 + size;
        return number(offset + 2, size);
    }

    std::optional<std::string> string(uint64_t object) const
    {
        const std::optional<size_t> offset = objectOffset(object);
        if (!offset)
            return std::nullopt;
        const uint8_t type = uint8_t(m_data[*offset]) >> 4;
        if (type != 0x5 && type != 0x6)
            return std::nullopt;
        size_t start = 0;
        const std::optional<uint64_t> count = length(*offset, &start);
        if (!count)
            return std::nullopt;
        const uint64_t available = m_offsetTable - start;

        if (type == 0x5) {
            if (*count > available)
                return std::nullopt;
            std::string result;
            for (uint64_t i = 0; i < *count; ++i)
                appendUtf8(&result, uint8_t(m_data[start + i]));
            return result;
        }

        if (*count > available / 2)
            return std::nullopt;
        std::string result;
        for (uint64_t i = 0; i < *count; ++i) {
            uint32_t unit = uint32_t(number(start + i * 2, 2));
            if (unit >= 0xd800 && unit < 0xdc00 && i + 1 < *count) {
                const uint32_t low = uint32_t(number(start + (i + 1) * 2, 2));
                if (low >= 0xdc00 && low < 0xe000) {
                    unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                    ++i;
                }
            }
            if (unit >= 0xd800 && unit < 0xe000)
                unit = 0xfffd;
            appendUtf8(&result, unit);
        }
        return result;
    }

    std::string_view m_data;
    size_t m_offsetSize = 0;
    size_t m_referenceSize = 0;
    uint64_t m_objectCount = 0;
    uint64_t m_offsetTable = 0;
    size_t m_budget = kMaximumValues;
    bool m_exhausted = false;
};

const Value* Value::find(std::string_view key) const
{
    for (const auto& [name, member] : members) {
        if (name == key)
            return &member;
    }
    return nullptr;
}

std::string Value::string(std::string_view key) const
{
    const Value* member = find(key);
    return member && member->type == Type::String ? member->text : std::string();
}

bool Value::flag(std::string_view key) const
{
    const Value* member = find(key);
    if (!member)
        return false;
    if (member->type == Type::Boolean)
        return member->boolean;
    return member->type == Type::String && (member->text == "YES" || member->text == "1");
}

std::vector<std::string> Value::strings() const
{
    if (type == Type::String)
        return {text};
    std::vector<std::string> result;
    for (const Value& element : array) {
        if (element.type == Type::String)
            result.push_back(element.text);
    }
    return result;
}

std::optional<Value> read(std::string_view data)
{
    if (data.starts_with("bplist00"))
        return BinaryReader(data).read();
    return readXml(data);
}

std::optional<Strings> rootStrings(std::string_view data)
{
    const std::optional<Value> root = read(data);
    if (!root || root->type != Value::Type::Dictionary)
        return std::nullopt;
    Strings result;
    for (const auto& [key, member] : root->members) {
        if (member.type == Value::Type::String)
            result.emplace(key, member.text);
    }
    return result;
}

} // namespace cutefish::base::plist
