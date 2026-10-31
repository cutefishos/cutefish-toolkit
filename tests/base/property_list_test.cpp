// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/property_list.h"

using namespace cutefish::base;

static void putNumber(std::string* out, uint64_t value, int size)
{
    for (int i = size - 1; i >= 0; --i)
        *out += char((value >> (8 * i)) & 0xff);
}

// A binary property list of one dictionary; a value that is an integer is
// written as one, a value of more than one byte per unit as UTF-16.
struct Value
{
    std::string text;
    std::u16string wide;
    int number = -1;
};

static std::string binaryPlist(const std::vector<std::pair<std::string, Value>>& members)
{
    std::vector<std::string> objects;
    std::string dictionary(1, char(0xd0 | members.size()));
    for (size_t i = 0; i < members.size(); ++i)
        dictionary += char(1 + i);
    for (size_t i = 0; i < members.size(); ++i)
        dictionary += char(1 + members.size() + i);
    objects.push_back(dictionary);

    const auto ascii = [](const std::string& text) {
        std::string object;
        if (text.size() < 15) {
            object += char(0x50 | text.size());
        } else {
            object += char(0x5f);
            object += char(0x10);
            object += char(text.size());
        }
        return object + text;
    };
    for (const auto& [key, value] : members)
        objects.push_back(ascii(key));
    for (const auto& [key, value] : members) {
        if (value.number >= 0) {
            objects.push_back(std::string(1, char(0x10)) + char(value.number));
        } else if (!value.wide.empty()) {
            std::string object(1, char(0x60 | value.wide.size()));
            for (const char16_t unit : value.wide)
                putNumber(&object, unit, 2);
            objects.push_back(object);
        } else {
            objects.push_back(ascii(value.text));
        }
    }

    std::string data = "bplist00";
    std::vector<size_t> offsets;
    for (const std::string& object : objects) {
        offsets.push_back(data.size());
        data += object;
    }
    const size_t table = data.size();
    for (const size_t offset : offsets)
        data += char(offset);
    data += std::string(6, '\0');
    data += char(1);
    data += char(1);
    putNumber(&data, objects.size(), 8);
    putNumber(&data, 0, 8);
    putNumber(&data, table, 8);
    return data;
}

TEST(PropertyListTest, ReadsTheRootStringsOfXml)
{
    const auto strings = plist::rootStrings(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<!-- A comment -->
	<key>CFBundleName</key>
	<string>Tom &amp; Jerry &#x263A;</string>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>CFBundleTypeName</key>
			<string>Nested</string>
		</dict>
	</array>
	<key>LSRequiresNativeExecution</key>
	<true/>
	<key>CFBundleIconFile</key>
	<string/>
	<key>CFBundleIdentifier</key>
	<string><![CDATA[org.example.<App>]]></string>
</dict>
</plist>
)");
    ASSERT_TRUE(strings);
    EXPECT_EQ(strings->size(), 3u);
    EXPECT_EQ(strings->at("CFBundleName"), "Tom & Jerry ☺");
    EXPECT_EQ(strings->at("CFBundleIconFile"), "");
    EXPECT_EQ(strings->at("CFBundleIdentifier"), "org.example.<App>");
    EXPECT_FALSE(strings->contains("CFBundleTypeName"));
}

TEST(PropertyListTest, ReadsTheRootStringsOfABinaryList)
{
    const std::string data = binaryPlist({
            {"CFBundleName", {.text = "Editor"}},
            {"CFBundleDisplayName", {.wide = u"Café \U0001F600"}},
            {"CFBundleExecutableTooLong", {.text = "a name longer than fifteen"}},
            {"Count", {.number = 5}},
    });
    const auto strings = plist::rootStrings(data);
    ASSERT_TRUE(strings);
    EXPECT_EQ(strings->size(), 3u);
    EXPECT_EQ(strings->at("CFBundleName"), "Editor");
    EXPECT_EQ(strings->at("CFBundleDisplayName"), "Café \U0001F600");
    EXPECT_EQ(strings->at("CFBundleExecutableTooLong"), "a name longer than fifteen");
}

TEST(PropertyListTest, RefusesWhatIsNoDictionary)
{
    EXPECT_FALSE(plist::rootStrings(""));
    EXPECT_FALSE(plist::rootStrings("<plist><array><string>a</string></array></plist>"));
    EXPECT_FALSE(plist::rootStrings("<plist><dict><key>a</key><string>b</dict></plist>"));
    EXPECT_FALSE(plist::rootStrings("<plist><dict><key>a</key><string>&bogus;</string></dict></plist>"));

    std::string data = binaryPlist({{"CFBundleName", {.text = "Editor"}}});
    EXPECT_TRUE(plist::rootStrings(data));
    // Every truncation, and an offset table that points past the objects.
    for (size_t size = 0; size < data.size(); ++size)
        EXPECT_FALSE(plist::rootStrings(data.substr(0, size))) << size;
    data[data.size() - 1] = char(0xff);
    EXPECT_FALSE(plist::rootStrings(data));
}

// A binary property list of the objects given whole, one-byte references,
// the first object the root.
static std::string binaryObjects(const std::vector<std::string>& objects)
{
    std::string data = "bplist00";
    std::vector<size_t> offsets;
    for (const std::string& object : objects) {
        offsets.push_back(data.size());
        data += object;
    }
    const size_t table = data.size();
    for (const size_t offset : offsets)
        putNumber(&data, offset, 2);
    data += std::string(6, '\0');
    data += char(2);
    data += char(1);
    putNumber(&data, objects.size(), 8);
    putNumber(&data, 0, 8);
    putNumber(&data, table, 8);
    return data;
}

TEST(PropertyListTest, ReadsNestedValuesOfXml)
{
    const auto info = plist::read(R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>CFBundleTypeRole</key>
			<string>Editor</string>
			<key>LSItemContentTypes</key>
			<array>
				<string>public.plain-text</string>
				<integer>4</integer>
				<string>public.html</string>
			</array>
			<key>CFBundleTypeExtensions</key>
			<string>txt</string>
		</dict>
		<array/>
	</array>
	<key>LSRequiresNativeExecution</key>
	<true/>
	<key>LSBackgroundOnly</key>
	<false/>
	<key>LSUIElement</key>
	<string>1</string>
</dict>
</plist>
)");
    ASSERT_TRUE(info);
    ASSERT_EQ(info->type, plist::Value::Type::Dictionary);
    const plist::Value* documents = info->find("CFBundleDocumentTypes");
    ASSERT_TRUE(documents);
    ASSERT_EQ(documents->array.size(), 2u);
    const plist::Value& document = documents->array[0];
    EXPECT_EQ(document.string("CFBundleTypeRole"), "Editor");
    EXPECT_EQ(document.find("LSItemContentTypes")->strings(),
              (std::vector<std::string>{"public.plain-text", "public.html"}));
    EXPECT_EQ(document.find("CFBundleTypeExtensions")->strings(), std::vector<std::string>{"txt"});
    EXPECT_EQ(documents->array[1].type, plist::Value::Type::Array);
    EXPECT_EQ(info->find("LSRequiresNativeExecution")->type, plist::Value::Type::Boolean);
    EXPECT_EQ(info->string("LSRequiresNativeExecution"), "");
    EXPECT_TRUE(info->flag("LSRequiresNativeExecution"));
    EXPECT_FALSE(info->flag("LSBackgroundOnly"));
    EXPECT_TRUE(info->flag("LSUIElement"));
    EXPECT_FALSE(info->flag("Missing"));
    EXPECT_FALSE(document.flag("CFBundleTypeRole"));
    EXPECT_FALSE(info->find("Missing"));
    EXPECT_FALSE(documents->find("CFBundleTypeRole"));

    EXPECT_FALSE(plist::read("<plist><dict><key>a</key><array><string>b</string></dict></plist>"));
}

TEST(PropertyListTest, ReadsNestedValuesOfABinaryList)
{
    const auto info = plist::read(binaryObjects({
            std::string("\xd2\x01\x08\x02\x09", 5),
            "\x55Types",
            std::string("\xa3\x03\x04\x07", 4),
            "\x5bpublic.text",
            std::string("\x10\x05", 2),
            "\x51x",
            "\x51y",
            std::string("\xd1\x05\x06", 3),
            "\x5bLSUIElement",
            "\x09",
    }));
    ASSERT_TRUE(info);
    const plist::Value* types = info->find("Types");
    ASSERT_TRUE(types);
    ASSERT_EQ(types->array.size(), 3u);
    EXPECT_EQ(types->strings(), std::vector<std::string>{"public.text"});
    EXPECT_EQ(types->array[1].type, plist::Value::Type::Other);
    EXPECT_EQ(types->array[2].string("x"), "y");
    EXPECT_EQ(info->find("LSUIElement")->type, plist::Value::Type::Boolean);
    EXPECT_TRUE(info->flag("LSUIElement"));

    // A reference past the objects is Other; the list still reads.
    const auto dangling = plist::read(binaryObjects({std::string("\xd1\x01\x09", 3), "\x51x"}));
    ASSERT_TRUE(dangling);
    EXPECT_EQ(dangling->find("x")->type, plist::Value::Type::Other);
}

TEST(PropertyListTest, RefusesBinaryListsWithoutEnd)
{
    // A dictionary that holds itself.
    EXPECT_FALSE(plist::read(binaryObjects({std::string("\xd1\x01\x00", 3), "\x51x"})));
    EXPECT_FALSE(plist::rootStrings(binaryObjects({std::string("\xd1\x01\x00", 3), "\x51x"})));

    // Forty arrays, each holding the next twice: 2^40 values, though only
    // forty objects and forty levels deep.
    std::vector<std::string> objects;
    for (int i = 0; i < 40; ++i)
        objects.push_back(std::string{char(0xa2), char(i + 1), char(i + 1)});
    objects.push_back("\x51x");
    EXPECT_FALSE(plist::read(binaryObjects(objects)));
}
