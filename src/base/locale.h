// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::base {

// A day of the proleptic Gregorian calendar.
struct CalendarDate
{
    int year = 0;
    // 1 to 12.
    int month = 0;
    int day = 0;

    bool isValid() const;
    // 1 for Monday to 7 for Sunday.
    int dayOfWeek() const;
    bool operator==(const CalendarDate&) const = default;
};

struct ClockTime
{
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;

    bool isValid() const;
    bool operator==(const ClockTime&) const = default;
};

// A wall-clock date and time with the zone it was read in, which the "t"
// format letters show.
struct DateTime
{
    CalendarDate date;
    ClockTime time;
    // Seconds east of UTC.
    int utcOffset = 0;
    // As the system's time zone database abbreviates it, e.g. "+08" or "CEST".
    std::string zoneAbbreviation;
    // The IANA id, such as "Asia/Singapore", for the zone's long name.
    std::string zoneId;

    // The local date and time at that moment, in the system's time zone.
    static DateTime fromSecondsSinceEpoch(int64_t seconds);
    // The same wall clock in UTC.
    static DateTime utc(CalendarDate date, ClockTime time);
};

enum class MeasurementSystem {
    Metric,
    US,
    // Metric, with miles and pints.
    UK,
};

enum class TemperatureUnit {
    Celsius,
    Fahrenheit,
};

// The region and the format choices of Settings' Language & Region page, as
// cutefish-services keeps them in services.conf [Region]; what is not set
// follows the region.
struct RegionalPreferences
{
    // ISO 3166, such as "SG"; empty for the territory the formats have
    // otherwise.
    std::string region;
    // 1 for Monday to 7 for Sunday; 0 for the region's.
    int firstDayOfWeek = 0;
    std::optional<bool> twentyFourHour;
    std::optional<MeasurementSystem> measurementSystem;
    std::optional<TemperatureUnit> temperatureUnit;

    // From [Region] of the settings file at the path.
    static RegionalPreferences read(const std::string& path);
    // cutefish-services' settings file.
    static std::string sessionPath();

    bool operator==(const RegionalPreferences&) const = default;
};

// Dates, times and numbers written by format strings and CLDR's data through
// ICU. The system locale is read per category: dates, times, day and month
// names and the first day of the week come from LC_TIME, number symbols from
// LC_NUMERIC, sorting from LC_COLLATE, and the name, the weekend and the digit
// grouping from LC_MESSAGES. The categories are read through the C library's
// locale setup, so a locale that is not installed counts as "C".
//
// A process that follows the session (setSystemPreferences) formats dates,
// times and numbers in the language of LC_MESSAGES and the session's region
// instead, with its choices.
class Locale
{
public:
    enum class FormatType {
        Long,
        Short,
        Narrow,
    };

    enum class DataSizeFormat {
        // 1024-based with KiB, MiB, ...
        Iec,
        // 1024-based with kB, MB, ...
        Traditional,
        // 1000-based with kB, MB, ...
        Si,
    };

    // The session's locale, as the process's locale categories name it.
    static const Locale& system();
    // Reads system() again after setlocale(); references to the previous
    // one stay valid.
    static void reloadSystem();
    // setlocale(LC_ALL, "") with LC_NUMERIC kept at "C", as strtod() and the
    // SVG parser expect a '.' decimal point; system() reads LC_NUMERIC itself.
    static void setProcessLocale();
    // The system's default language for sessions, such as "en_SG" or
    // "sr_RS@latin", from LANG in /etc/default/locale or /etc/locale.conf;
    // "en_US" without one.
    static std::string systemLanguage();
    // The system's default region, such as "SG": the territory of LC_TIME,
    // else of LANG, in the same files; empty without one.
    static std::string systemRegion();
    // Locale::system() formats in the region with these choices from now on,
    // until nullopt goes back to the categories alone; then reloadSystem().
    static void setSystemPreferences(std::optional<RegionalPreferences> preferences);
    // The formats of a language, such as "zh_CN", in the preferences' region
    // (else the language's own) with their choices: the names in the
    // language, order and conventions of the region, also for pairs the C
    // library has no locale for ("ja_SG"). Text and sorting follow the
    // language.
    static Locale regional(std::string_view language, const RegionalPreferences& preferences);
    // Every category from one locale, such as "en_SG", "zh_CN" or "C"; a
    // codeset or modifier ("en_SG.UTF-8@euro") is ignored.
    explicit Locale(std::string_view name);
    ~Locale();

    Locale(const Locale&);
    Locale& operator=(const Locale&);

    // Language and territory, such as "en_US", or "C".
    std::string name() const;
    // The language's code, such as "en"; "C" for the C locale.
    std::string language() const;
    // The territory of the dates and numbers, such as "SG"; empty for C.
    std::string territory() const;
    // ICU's id for the dates and numbers, with the choices as keywords, such
    // as "zh_Hans_SG@fw=mon;hours=h23".
    std::string formatsId() const;

    bool usesTwentyFourHourTime() const;
    MeasurementSystem measurementSystem() const;
    // For the weather, as CLDR prefers it in the region.
    TemperatureUnit temperatureUnit() const;

    // Format strings, such as "dddd, d MMMM yyyy" or "h:mm Ap".
    std::string dateFormat(FormatType type = FormatType::Long) const;
    std::string timeFormat(FormatType type = FormatType::Long) const;
    std::string dateTimeFormat(FormatType type = FormatType::Long) const;

    // Months 1 to 12; days 1 for Monday to 7 for Sunday. The standalone
    // forms name a month or day on its own, the others within a date.
    std::string monthName(int month, FormatType type = FormatType::Long) const;
    std::string standaloneMonthName(int month, FormatType type = FormatType::Long) const;
    std::string dayName(int day, FormatType type = FormatType::Long) const;
    std::string standaloneDayName(int day, FormatType type = FormatType::Long) const;
    std::string amText() const;
    std::string pmText() const;
    // 1 for Monday to 7 for Sunday.
    int firstDayOfWeek() const;
    // The working days, 1 for Monday to 7 for Sunday, in that order.
    std::vector<int> weekdays() const;

    std::string toString(const CalendarDate& date, FormatType type = FormatType::Long) const;
    std::string toString(const ClockTime& time, FormatType type = FormatType::Long) const;
    std::string toString(const DateTime& dateTime, FormatType type = FormatType::Long) const;
    // With a format string: d dd ddd dddd, M MM MMM MMMM, yy yyyy, h hh
    // H HH, m mm, s ss, z zzz, AP ap Ap aP, t tt ttt tttt and 'quoted text'.
    std::string toString(const CalendarDate& date, std::string_view format) const;
    std::string toString(const ClockTime& time, std::string_view format) const;
    std::string toString(const DateTime& dateTime, std::string_view format) const;

    std::string decimalPoint() const;
    std::string groupSeparator() const;
    std::string negativeSign() const;
    std::string positiveSign() const;
    std::string zeroDigit() const;

    // Fixed-point with exactly that many decimals, halfway values rounded away
    // from zero, digits grouped.
    std::string toString(double value, int decimals) const;
    std::string toString(int64_t value) const;
    std::string toString(int value) const { return toString(int64_t(value)); }
    // A byte count in the largest unit it reaches, at most precision decimals,
    // e.g. "1.50 KiB" or "999 bytes".
    std::string formattedDataSize(int64_t bytes, int precision = 2,
                                  DataSizeFormat format = DataSizeFormat::Iec) const;

    // By LC_COLLATE: < 0, 0 or > 0.
    int compare(std::string_view a, std::string_view b) const;

    // The English name of an ISO 3166 code, such as "SG" for "Singapore".
    static std::string territoryName(std::string_view code);

    struct Data;

private:
    friend class Collator;

    Locale() = default;
    static const Locale* readSystem();

    std::shared_ptr<const Data> m_messages;
    std::shared_ptr<const Data> m_time;
    std::shared_ptr<const Data> m_numeric;
    std::string m_collation;
    // Off for an explicit C locale, which writes numbers without separators.
    bool m_groupDigits = true;
};

} // namespace cutefish::base
