// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "base/locale.h"

using namespace cutefish::base;

namespace {

struct Expected
{
    const char* locale;
    const char* key;
    const char* value;
};

} // namespace

// Reference results for these locales, keyed as the measuring tool printed
// them: "<function>.<form>.<argument>".
static const Expected kReference[] = {
    {"C", "name", "C"},
    {"C", "dateFormat.long", "dddd, d MMMM yyyy"},
    {"C", "dateFormat.short", "d MMM yyyy"},
    {"C", "timeFormat.long", "HH:mm:ss t"},
    {"C", "timeFormat.short", "HH:mm:ss"},
    {"C", "monthName.long.10", "October"},
    {"C", "monthName.short.10", "Oct"},
    {"C", "standaloneMonthName.long.10", "October"},
    {"C", "standaloneMonthName.narrow.10", "O"},
    {"C", "dayName.long.3", "Wednesday"},
    {"C", "dayName.short.3", "Wed"},
    {"C", "standaloneDayName.narrow.3", "W"},
    {"C", "amText", "AM"},
    {"C", "pmText", "PM"},
    {"C", "firstDayOfWeek", "1"},
    {"C", "weekdays", "1,2,3,4,5"},
    {"C", "decimalPoint", "."},
    {"C", "groupSeparator", ","},
    {"C", "zeroDigit", "0"},
    {"C", "double.1234567.8910000001.2", "1234567.89"},
    {"C", "double.-0.040000000000000001.1", "-0.0"},
    {"C", "double.2.5.0", "3"},
    {"C", "double.0.125.2", "0.13"},
    {"C", "double.999.95000000000005.1", "1000.0"},
    {"C", "int.1234567", "1234567"},
    {"C", "int.-7", "-7"},
    {"C", "dataSize.iec.0", "0 bytes"},
    {"C", "dataSize.iec.1536", "1.50 KiB"},
    {"C", "dataSize.si.123456789", "123.46 MB"},
    {"C", "dataSize.trad.1048575", "1024.00 kB"},
    {"C", "date.long.2026-10-14", "Wednesday, 14 October 2026"},
    {"C", "date.short.2026-01-03", "3 Jan 2026"},
    {"C", "time.short.16:00:00.000", "16:00:00"},
    {"C", "time.short.00:00:00.000", "00:00:00"},
    {"C", "dateTime.short.2026-10-03T14:21:00Z", "3 Oct 2026 14:21:00"},
    {"C", "dateFmt.ddd.2026-10-14", "Wed"},
    {"C", "dateFmt.dddd, d MMMM yyyy.2026-10-14", "Wednesday, 14 October 2026"},
    {"C", "timeFmt.h:mm AP.13:05:07.042", "1:05 PM"},
    {"C", "timeFmt.h:mm ap.13:05:07.042", "1:05 pm"},
    {"C", "timeFmt.h:mm aP.00:05:09.000", "12:05 AM"},
    {"C", "timeFmt.hap.12:00:00.000", "12pm"},
    {"C", "timeFmt.h:mm:ss z.13:05:07.042", "13:05:07 042"},
    {"C", "timeFmt.h:mm:ss z.00:00:00.000", "0:00:00 0"},
    {"C", "timeFmt.hh:mm:ss.zzz.23:59:59.999", "23:59:59.999"},
    {"C", "timeFmt.HH 'h' mm.09:30:00.000", "09 h 30"},
    {"C", "dateFmt.'It''s' d.2026-10-14", "It's 14"},
    {"C", "dateFmt.y yyy yyyyy.1999-12-31", "y 99y 1999y"},
    {"C", "dateFmt.yy-M-d.2000-02-29", "00-2-29"},
    {"C", "dateFmt.dd.MM.yyyy.0005-06-07", "07.06.0005"},
    {"en_US", "name", "en_US"},
    {"en_US", "dateFormat.long", "dddd, MMMM d, yyyy"},
    {"en_US", "dateFormat.short", "M/d/yy"},
    {"en_US", "timeFormat.long", "h:mm:ss\u202fAp tttt"},
    {"en_US", "timeFormat.short", "h:mm\u202fAp"},
    {"en_US", "monthName.long.10", "October"},
    {"en_US", "monthName.short.10", "Oct"},
    {"en_US", "standaloneMonthName.long.10", "October"},
    {"en_US", "standaloneMonthName.narrow.10", "O"},
    {"en_US", "dayName.long.3", "Wednesday"},
    {"en_US", "dayName.short.3", "Wed"},
    {"en_US", "standaloneDayName.narrow.3", "W"},
    {"en_US", "amText", "AM"},
    {"en_US", "pmText", "PM"},
    {"en_US", "firstDayOfWeek", "7"},
    {"en_US", "weekdays", "1,2,3,4,5"},
    {"en_US", "decimalPoint", "."},
    {"en_US", "groupSeparator", ","},
    {"en_US", "zeroDigit", "0"},
    {"en_US", "double.1234567.8910000001.2", "1,234,567.89"},
    {"en_US", "double.-0.040000000000000001.1", "-0.0"},
    {"en_US", "double.2.5.0", "3"},
    {"en_US", "double.0.125.2", "0.13"},
    {"en_US", "double.999.95000000000005.1", "1,000.0"},
    {"en_US", "int.1234567", "1,234,567"},
    {"en_US", "int.-7", "-7"},
    {"en_US", "dataSize.iec.0", "0 bytes"},
    {"en_US", "dataSize.iec.1536", "1.50 KiB"},
    {"en_US", "dataSize.si.123456789", "123.46 MB"},
    {"en_US", "dataSize.trad.1048575", "1,024.00 kB"},
    {"en_US", "date.long.2026-10-14", "Wednesday, October 14, 2026"},
    {"en_US", "date.short.2026-01-03", "1/3/26"},
    {"en_US", "time.short.16:00:00.000", "4:00\u202fPM"},
    {"en_US", "time.short.00:00:00.000", "12:00\u202fAM"},
    {"en_US", "dateTime.short.2026-10-03T14:21:00Z", "10/3/26 2:21\u202fPM"},
    {"en_US", "dateFmt.ddd.2026-10-14", "Wed"},
    {"en_US", "dateFmt.dddd, d MMMM yyyy.2026-10-14", "Wednesday, 14 October 2026"},
    {"en_SG", "name", "en_SG"},
    {"en_SG", "dateFormat.long", "dddd, d MMMM yyyy"},
    {"en_SG", "dateFormat.short", "d/M/yy"},
    {"en_SG", "timeFormat.long", "h:mm:ss\u202fAp tttt"},
    {"en_SG", "timeFormat.short", "h:mm\u202fAp"},
    {"en_SG", "monthName.long.10", "October"},
    {"en_SG", "monthName.short.10", "Oct"},
    {"en_SG", "standaloneMonthName.long.10", "October"},
    {"en_SG", "standaloneMonthName.narrow.10", "O"},
    {"en_SG", "dayName.long.3", "Wednesday"},
    {"en_SG", "dayName.short.3", "Wed"},
    {"en_SG", "standaloneDayName.narrow.3", "W"},
    {"en_SG", "amText", "am"},
    {"en_SG", "pmText", "pm"},
    {"en_SG", "firstDayOfWeek", "7"},
    {"en_SG", "weekdays", "1,2,3,4,5"},
    {"en_SG", "decimalPoint", "."},
    {"en_SG", "groupSeparator", ","},
    {"en_SG", "zeroDigit", "0"},
    {"en_SG", "double.1234567.8910000001.2", "1,234,567.89"},
    {"en_SG", "double.-0.040000000000000001.1", "-0.0"},
    {"en_SG", "double.2.5.0", "3"},
    {"en_SG", "double.0.125.2", "0.13"},
    {"en_SG", "double.999.95000000000005.1", "1,000.0"},
    {"en_SG", "int.1234567", "1,234,567"},
    {"en_SG", "int.-7", "-7"},
    {"en_SG", "dataSize.iec.0", "0 bytes"},
    {"en_SG", "dataSize.iec.1536", "1.50 KiB"},
    {"en_SG", "dataSize.si.123456789", "123.46 MB"},
    {"en_SG", "dataSize.trad.1048575", "1,024.00 kB"},
    {"en_SG", "date.long.2026-10-14", "Wednesday, 14 October 2026"},
    {"en_SG", "date.short.2026-01-03", "3/1/26"},
    {"en_SG", "time.short.16:00:00.000", "4:00\u202fpm"},
    {"en_SG", "time.short.00:00:00.000", "12:00\u202fam"},
    {"en_SG", "dateTime.short.2026-10-03T14:21:00Z", "3/10/26 2:21\u202fpm"},
    {"en_SG", "dateFmt.ddd.2026-10-14", "Wed"},
    {"en_SG", "dateFmt.dddd, d MMMM yyyy.2026-10-14", "Wednesday, 14 October 2026"},
    {"en_SG", "timeFmt.h:mm AP.13:05:07.042", "1:05 PM"},
    {"en_SG", "timeFmt.h:mm ap.13:05:07.042", "1:05 pm"},
    {"en_SG", "timeFmt.h:mm aP.00:05:09.000", "12:05 am"},
    {"en_SG", "timeFmt.hap.12:00:00.000", "12pm"},
    {"en_SG", "timeFmt.h:mm:ss z.13:05:07.042", "13:05:07 042"},
    {"en_SG", "timeFmt.h:mm:ss z.00:00:00.000", "0:00:00 0"},
    {"en_SG", "timeFmt.hh:mm:ss.zzz.23:59:59.999", "23:59:59.999"},
    {"en_SG", "timeFmt.HH 'h' mm.09:30:00.000", "09 h 30"},
    {"en_SG", "dateFmt.'It''s' d.2026-10-14", "It's 14"},
    {"en_SG", "dateFmt.y yyy yyyyy.1999-12-31", "y 99y 1999y"},
    {"en_SG", "dateFmt.yy-M-d.2000-02-29", "00-2-29"},
    {"en_SG", "dateFmt.dd.MM.yyyy.0005-06-07", "07.06.0005"},
    {"de_DE", "name", "de_DE"},
    {"de_DE", "dateFormat.long", "dddd, d. MMMM yyyy"},
    {"de_DE", "dateFormat.short", "dd.MM.yy"},
    {"de_DE", "timeFormat.long", "HH:mm:ss tttt"},
    {"de_DE", "timeFormat.short", "HH:mm"},
    {"de_DE", "monthName.long.10", "Oktober"},
    {"de_DE", "monthName.short.10", "Okt."},
    {"de_DE", "standaloneMonthName.long.10", "Oktober"},
    {"de_DE", "standaloneMonthName.narrow.10", "O"},
    {"de_DE", "dayName.long.3", "Mittwoch"},
    {"de_DE", "dayName.short.3", "Mi."},
    {"de_DE", "standaloneDayName.narrow.3", "M"},
    {"de_DE", "amText", "AM"},
    {"de_DE", "pmText", "PM"},
    {"de_DE", "firstDayOfWeek", "1"},
    {"de_DE", "weekdays", "1,2,3,4,5"},
    {"de_DE", "decimalPoint", ","},
    {"de_DE", "groupSeparator", "."},
    {"de_DE", "zeroDigit", "0"},
    {"de_DE", "double.1234567.8910000001.2", "1.234.567,89"},
    {"de_DE", "double.-0.040000000000000001.1", "-0,0"},
    {"de_DE", "double.2.5.0", "3"},
    {"de_DE", "double.0.125.2", "0,13"},
    {"de_DE", "double.999.95000000000005.1", "1.000,0"},
    {"de_DE", "int.1234567", "1.234.567"},
    {"de_DE", "int.-7", "-7"},
    {"de_DE", "dataSize.iec.0", "0 Bytes"},
    {"de_DE", "dataSize.iec.1536", "1,50 KiB"},
    {"de_DE", "dataSize.si.123456789", "123,46 MB"},
    {"de_DE", "dataSize.trad.1048575", "1.024,00 kB"},
    {"de_DE", "date.long.2026-10-14", "Mittwoch, 14. Oktober 2026"},
    {"de_DE", "date.short.2026-01-03", "03.01.26"},
    {"de_DE", "time.short.16:00:00.000", "16:00"},
    {"de_DE", "time.short.00:00:00.000", "00:00"},
    {"de_DE", "dateTime.short.2026-10-03T14:21:00Z", "03.10.26 14:21"},
    {"de_DE", "dateFmt.ddd.2026-10-14", "Mi."},
    {"de_DE", "dateFmt.dddd, d MMMM yyyy.2026-10-14", "Mittwoch, 14 Oktober 2026"},
    {"fr_FR", "name", "fr_FR"},
    {"fr_FR", "dateFormat.long", "dddd d MMMM yyyy"},
    {"fr_FR", "dateFormat.short", "dd/MM/yyyy"},
    {"fr_FR", "timeFormat.long", "HH:mm:ss tttt"},
    {"fr_FR", "timeFormat.short", "HH:mm"},
    {"fr_FR", "monthName.long.10", "octobre"},
    {"fr_FR", "monthName.short.10", "oct."},
    {"fr_FR", "standaloneMonthName.long.10", "octobre"},
    {"fr_FR", "standaloneMonthName.narrow.10", "O"},
    {"fr_FR", "dayName.long.3", "mercredi"},
    {"fr_FR", "dayName.short.3", "mer."},
    {"fr_FR", "standaloneDayName.narrow.3", "M"},
    {"fr_FR", "amText", "AM"},
    {"fr_FR", "pmText", "PM"},
    {"fr_FR", "firstDayOfWeek", "1"},
    {"fr_FR", "weekdays", "1,2,3,4,5"},
    {"fr_FR", "decimalPoint", ","},
    {"fr_FR", "groupSeparator", "\u202f"},
    {"fr_FR", "zeroDigit", "0"},
    {"fr_FR", "double.1234567.8910000001.2", "1\u202f234\u202f567,89"},
    {"fr_FR", "double.-0.040000000000000001.1", "-0,0"},
    {"fr_FR", "double.2.5.0", "3"},
    {"fr_FR", "double.0.125.2", "0,13"},
    {"fr_FR", "double.999.95000000000005.1", "1\u202f000,0"},
    {"fr_FR", "int.1234567", "1\u202f234\u202f567"},
    {"fr_FR", "int.-7", "-7"},
    {"fr_FR", "dataSize.iec.0", "0 octets"},
    {"fr_FR", "dataSize.iec.1536", "1,50 Kio"},
    {"fr_FR", "dataSize.si.123456789", "123,46 Mo"},
    {"fr_FR", "dataSize.trad.1048575", "1\u202f024,00 ko"},
    {"fr_FR", "date.long.2026-10-14", "mercredi 14 octobre 2026"},
    {"fr_FR", "date.short.2026-01-03", "03/01/2026"},
    {"fr_FR", "time.short.16:00:00.000", "16:00"},
    {"fr_FR", "time.short.00:00:00.000", "00:00"},
    {"fr_FR", "dateTime.short.2026-10-03T14:21:00Z", "03/10/2026 14:21"},
    {"fr_FR", "dateFmt.ddd.2026-10-14", "mer."},
    {"fr_FR", "dateFmt.dddd, d MMMM yyyy.2026-10-14", "mercredi, 14 octobre 2026"},
};

static Locale::FormatType formatType(std::string_view name)
{
    return name == "long" ? Locale::FormatType::Long
           : name == "short" ? Locale::FormatType::Short : Locale::FormatType::Narrow;
}

static CalendarDate isoDate(std::string_view text)
{
    return {std::stoi(std::string(text.substr(0, 4))), std::stoi(std::string(text.substr(5, 2))),
            std::stoi(std::string(text.substr(8, 2)))};
}

static ClockTime isoTime(std::string_view text)
{
    return {std::stoi(std::string(text.substr(0, 2))), std::stoi(std::string(text.substr(3, 2))),
            std::stoi(std::string(text.substr(6, 2))), std::stoi(std::string(text.substr(9, 3)))};
}

// The function the key names, applied to the locale.
static std::string evaluate(const Locale& locale, std::string_view key)
{
    const size_t dot = key.find('.');
    const std::string_view function = key.substr(0, dot);
    const std::string_view rest = dot == std::string_view::npos ? std::string_view() : key.substr(dot + 1);
    const auto split = [](std::string_view text) {
        const size_t at = text.find('.');
        return std::pair(text.substr(0, at), text.substr(at + 1));
    };

    if (function == "name")
        return locale.name();
    if (function == "dateFormat")
        return locale.dateFormat(formatType(rest));
    if (function == "timeFormat")
        return locale.timeFormat(formatType(rest));
    if (function == "amText")
        return locale.amText();
    if (function == "pmText")
        return locale.pmText();
    if (function == "firstDayOfWeek")
        return std::to_string(locale.firstDayOfWeek());
    if (function == "weekdays") {
        std::string text;
        for (int day : locale.weekdays())
            text += (text.empty() ? "" : ",") + std::to_string(day);
        return text;
    }
    if (function == "decimalPoint")
        return locale.decimalPoint();
    if (function == "groupSeparator")
        return locale.groupSeparator();
    if (function == "zeroDigit")
        return locale.zeroDigit();
    if (function == "monthName" || function == "standaloneMonthName" || function == "dayName"
        || function == "standaloneDayName") {
        const auto [form, index] = split(rest);
        const int number = std::stoi(std::string(index));
        if (function == "monthName")
            return locale.monthName(number, formatType(form));
        if (function == "standaloneMonthName")
            return locale.standaloneMonthName(number, formatType(form));
        if (function == "dayName")
            return locale.dayName(number, formatType(form));
        return locale.standaloneDayName(number, formatType(form));
    }
    if (function == "double") {
        const size_t last = rest.rfind('.');
        return locale.toString(std::stod(std::string(rest.substr(0, last))),
                               std::stoi(std::string(rest.substr(last + 1))));
    }
    if (function == "int")
        return locale.toString(int64_t(std::stoll(std::string(rest))));
    if (function == "dataSize") {
        const auto [form, bytes] = split(rest);
        const int64_t value = std::stoll(std::string(bytes));
        if (form == "si")
            return locale.formattedDataSize(value, 2, Locale::DataSizeFormat::Si);
        if (form == "trad")
            return locale.formattedDataSize(value, 2, Locale::DataSizeFormat::Traditional);
        return locale.formattedDataSize(value);
    }
    if (function == "date") {
        const auto [form, date] = split(rest);
        return locale.toString(isoDate(date), formatType(form));
    }
    if (function == "time") {
        const auto [form, time] = split(rest);
        return locale.toString(isoTime(time), formatType(form));
    }
    if (function == "dateTime") {
        const auto [form, value] = split(rest);
        return locale.toString(DateTime::utc(isoDate(value), isoTime(std::string(value.substr(11, 8)) + ".000")),
                               formatType(form));
    }
    // The format may hold dots itself; the date or time is the last field.
    if (function == "dateFmt") {
        const size_t last = rest.rfind('.');
        return locale.toString(isoDate(rest.substr(last + 1)), rest.substr(0, last));
    }
    if (function == "timeFmt") {
        const size_t last = rest.rfind('.', rest.size() - 5);
        return locale.toString(isoTime(rest.substr(last + 1)), rest.substr(0, last));
    }
    return "<unknown key>";
}

TEST(LocaleTest, MatchesReferenceFormats)
{
    for (const Expected& expected : kReference) {
        const Locale locale(expected.locale);
        EXPECT_EQ(evaluate(locale, expected.key), expected.value) << expected.locale << " " << expected.key;
    }
}

TEST(LocaleTest, NamesWithCodesetAndModifier)
{
    EXPECT_EQ(Locale("en_SG.UTF-8").name(), "en_SG");
    EXPECT_EQ(Locale("de_DE.UTF-8@euro").name(), "de_DE");
    EXPECT_EQ(Locale("de").name(), "de_DE");
    EXPECT_EQ(Locale("C.UTF-8").name(), "C");
    EXPECT_EQ(Locale("POSIX").language(), "C");
}

TEST(LocaleTest, DayOfWeek)
{
    EXPECT_EQ((CalendarDate{2026, 10, 14}.dayOfWeek()), 3);
    EXPECT_EQ((CalendarDate{1970, 1, 1}.dayOfWeek()), 4);
    EXPECT_EQ((CalendarDate{2000, 2, 29}.dayOfWeek()), 2);
    EXPECT_EQ((CalendarDate{1969, 12, 28}.dayOfWeek()), 7);
    EXPECT_FALSE((CalendarDate{2026, 2, 29}.isValid()));
    EXPECT_TRUE((CalendarDate{2000, 2, 29}.isValid()));
}

TEST(LocaleTest, ExplicitCLocaleOmitsSeparators)
{
    EXPECT_EQ(Locale("C").toString(int64_t(1234567)), "1234567");
    EXPECT_EQ(Locale("en_US").toString(int64_t(1234567)), "1,234,567");
}

TEST(LocaleTest, Compare)
{
    const Locale english("en_US");
    EXPECT_LT(english.compare("apple", "Banana"), 0);
    EXPECT_LT(english.compare("item10", "item2"), 0);
    EXPECT_EQ(english.compare("same", "same"), 0);
    // Code point order without a collation.
    EXPECT_GT(Locale("C").compare("apple", "Banana"), 0);
}

TEST(LocaleTest, TerritoryName)
{
    EXPECT_EQ(Locale::territoryName("SG"), "Singapore");
    EXPECT_EQ(Locale::territoryName("US"), "United States");
    EXPECT_EQ(Locale::territoryName("CI"), "Côte d’Ivoire");
}

TEST(LocaleTest, RegionalFormatsPairLanguageAndRegion)
{
    const Locale japanese = Locale::regional("ja_JP", {.region = "SG"});
    EXPECT_EQ(japanese.formatsId(), "ja_SG");
    EXPECT_EQ(japanese.territory(), "SG");
    EXPECT_EQ(japanese.name(), "ja_JP");
    EXPECT_EQ(japanese.monthName(10), "10\u6708");
    // Singapore's week starts on Sunday, Japan's too, Germany's on Monday.
    EXPECT_EQ(japanese.firstDayOfWeek(), 7);
    EXPECT_EQ(Locale::regional("en_US", {.region = "DE"}).firstDayOfWeek(), 1);

    EXPECT_EQ(Locale::regional("zh_TW", {.region = "SG"}).formatsId(), "zh_Hant_SG");
    EXPECT_EQ(Locale::regional("zh_CN", {.region = "SG"}).formatsId(), "zh_SG");
    // The language's own territory without a region.
    EXPECT_EQ(Locale::regional("de_DE", {}).formatsId(), "de_DE");
    EXPECT_EQ(Locale::regional("en_US", {.region = "GB"}).toString(CalendarDate{2026, 10, 14},
                                                                    Locale::FormatType::Short),
              "14/10/2026");
}

TEST(LocaleTest, RegionalChoicesOverrideTheRegion)
{
    const Locale us = Locale::regional("en_US", {.region = "US"});
    EXPECT_FALSE(us.usesTwentyFourHourTime());
    EXPECT_EQ(us.measurementSystem(), MeasurementSystem::US);
    EXPECT_EQ(us.temperatureUnit(), TemperatureUnit::Fahrenheit);
    EXPECT_EQ(us.firstDayOfWeek(), 7);

    const Locale chosen = Locale::regional("en_US", {.region = "US",
                                                     .firstDayOfWeek = 1,
                                                     .twentyFourHour = true,
                                                     .measurementSystem = MeasurementSystem::Metric,
                                                     .temperatureUnit = TemperatureUnit::Celsius});
    EXPECT_EQ(chosen.formatsId(), "en_US@fw=mon;hours=h23;measure=metric;mu=celsius");
    EXPECT_EQ(chosen.firstDayOfWeek(), 1);
    EXPECT_TRUE(chosen.usesTwentyFourHourTime());
    EXPECT_EQ(chosen.toString(ClockTime{16, 5, 0, 0}, Locale::FormatType::Short), "16:05");
    EXPECT_EQ(chosen.measurementSystem(), MeasurementSystem::Metric);
    EXPECT_EQ(chosen.temperatureUnit(), TemperatureUnit::Celsius);
    // Names and order stay the region's.
    EXPECT_EQ(chosen.toString(CalendarDate{2026, 10, 14}, Locale::FormatType::Short), "10/14/26");

    const Locale twelve = Locale::regional("de_DE", {.twentyFourHour = false});
    EXPECT_FALSE(twelve.usesTwentyFourHourTime());
    EXPECT_EQ(Locale::regional("en_GB", {}).measurementSystem(), MeasurementSystem::UK);
    EXPECT_EQ(Locale::regional("en_GB", {.measurementSystem = MeasurementSystem::US}).measurementSystem(),
              MeasurementSystem::US);
}

TEST(LocaleTest, SystemFollowsThePreferences)
{
    const std::string before = Locale::system().formatsId();
    Locale::setSystemPreferences(RegionalPreferences{.region = "SG", .firstDayOfWeek = 3});
    Locale::reloadSystem();
    EXPECT_EQ(Locale::system().territory(), "SG");
    EXPECT_EQ(Locale::system().firstDayOfWeek(), 3);

    Locale::setSystemPreferences(std::nullopt);
    Locale::reloadSystem();
    EXPECT_EQ(Locale::system().formatsId(), before);
}

TEST(LocaleTest, ReadsPreferencesFromTheServicesSettings)
{
    const std::string path = ::testing::TempDir() + "region-test-services.conf";
    std::FILE* file = std::fopen(path.c_str(), "w");
    ASSERT_NE(file, nullptr);
    std::fputs("[Region]\nregion=sg\nfirstWeekday=1\ntwentyFourHour=true\nmeasurement=uk\ntemperature=fahrenheit\n",
               file);
    std::fclose(file);
    const RegionalPreferences preferences = RegionalPreferences::read(path);
    EXPECT_EQ(preferences.region, "SG");
    EXPECT_EQ(preferences.firstDayOfWeek, 1);
    EXPECT_EQ(preferences.twentyFourHour, true);
    EXPECT_EQ(preferences.measurementSystem, MeasurementSystem::UK);
    EXPECT_EQ(preferences.temperatureUnit, TemperatureUnit::Fahrenheit);
    std::remove(path.c_str());

    EXPECT_EQ(RegionalPreferences::read(path), RegionalPreferences{});
}
