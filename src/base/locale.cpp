// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/locale.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <langinfo.h>
#include <locale.h>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <unicode/uchar.h>
#include <unicode/ucal.h>
#include <unicode/ucol.h>
#include <unicode/udat.h>
#include <unicode/udata.h>
#include <unicode/uloc.h>
#include <unicode/ulocdata.h>
#include <unicode/measunit.h>
#include <unicode/numberformatter.h>
#include <unicode/unum.h>
#include <unicode/ures.h>
#include <unicode/ustring.h>

#include "base/file.h"
#include "base/paths.h"
#include "base/settings.h"
#include "base/strings.h"

// Exported by ICU's common library but declared only in its internal
// uresimp.h: a nested lookup that inherits from parent locales key by key, as
// CLDR does.
U_CAPI UResourceBundle* U_EXPORT2 ures_getByKeyWithFallback(const UResourceBundle* resourceBundle, const char* key,
                                                            UResourceBundle* fillIn, UErrorCode* status);

namespace cutefish::base {

struct Locale::Data
{
    // Language and territory, such as "en_US", or "C".
    std::string name = "C";
    std::string language = "C";
    std::string territory;
    std::string icuId = "en_US_POSIX";
    bool isC = true;

    // Format strings as Locale::toString() takes them, long and short.
    std::array<std::string, 2> dateFormats = {"dddd, d MMMM yyyy", "d MMM yyyy"};
    std::array<std::string, 2> timeFormats = {"HH:mm:ss t", "HH:mm:ss"};
    // By Locale::FormatType, then the same three standalone.
    std::array<std::array<std::string, 12>, 6> months;
    // Monday to Sunday.
    std::array<std::array<std::string, 7>, 6> days;
    std::string am = "AM";
    std::string pm = "PM";
    int firstDayOfWeek = 1;
    std::vector<int> weekdays = {1, 2, 3, 4, 5};

    std::string decimal = ".";
    std::string group = ",";
    std::string minus = "-";
    std::string plus = "+";
    char32_t zero = U'0';
    // Digits after the last separator, between separators, and the fewest
    // before the first one.
    int groupLeast = 3;
    int groupHigher = 3;
    int groupTop = 1;

    MeasurementSystem measurement = MeasurementSystem::Metric;
    TemperatureUnit temperature = TemperatureUnit::Celsius;

    std::string byteUnit = "bytes";
    std::array<std::string, 6> siUnits = {"kB", "MB", "GB", "TB", "PB", "EB"};
    std::array<std::string, 6> iecUnits = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
};

static std::string toUtf8(const UChar* text, int32_t length)
{
    if (!text || length <= 0)
        return {};
    UErrorCode status = U_ZERO_ERROR;
    int32_t size = 0;
    u_strToUTF8(nullptr, 0, &size, text, length, &status);
    std::string result(size_t(size), '\0');
    status = U_ZERO_ERROR;
    u_strToUTF8(result.data(), size, nullptr, text, length, &status);
    return U_FAILURE(status) ? std::string() : result;
}

static std::u16string toUtf16(std::string_view text)
{
    UErrorCode status = U_ZERO_ERROR;
    int32_t size = 0;
    u_strFromUTF8(nullptr, 0, &size, text.data(), int32_t(text.size()), &status);
    std::u16string result(size_t(size), u'\0');
    status = U_ZERO_ERROR;
    u_strFromUTF8(result.data(), size, nullptr, text.data(), int32_t(text.size()), &status);
    return U_FAILURE(status) ? std::u16string() : result;
}

static std::string fromCodePoint(char32_t c)
{
    UChar units[2];
    int32_t length = 0;
    UBool error = false;
    U16_APPEND(units, length, 2, c, error);
    return error ? std::string() : toUtf8(units, length);
}

static std::string caseMapped(const std::string& text, bool upper)
{
    const std::u16string source = toUtf16(text);
    std::u16string result(source.size() * 3 + 1, u'\0');
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = upper ? u_strToUpper(result.data(), int32_t(result.size()), source.data(),
                                                int32_t(source.size()), "", &status)
                                 : u_strToLower(result.data(), int32_t(result.size()), source.data(),
                                                int32_t(source.size()), "", &status);
    return U_FAILURE(status) ? text : toUtf8(result.data(), length);
}

// "en_SG.UTF-8@euro" or "en-SG" to "en_SG"; empty for the C locale.
static std::string localeId(std::string_view name)
{
    std::string id(name.substr(0, name.find_first_of(".@")));
    std::replace(id.begin(), id.end(), '-', '_');
    if (id.empty() || id == "C" || id == "POSIX")
        return {};
    return id;
}

// Converts a CLDR date or time pattern into a format string as
// Locale::toString() takes it.
static std::string formatFromCldrPattern(std::string_view text)
{
    std::string result;
    std::string separator;
    const auto countFirst = [&text] {
        size_t n = 1;
        while (n < text.size() && text[n] == text[0])
            ++n;
        return n;
    };
    while (!text.empty()) {
        const char c = text[0];
        if (c == '\'') {
            const size_t close = text.find('\'', 1);
            const size_t length = close == std::string_view::npos ? text.size() : close + 1;
            separator += text.substr(0, length);
            text.remove_prefix(length);
            continue;
        }
        const size_t n = countFirst();
        std::string form;
        switch (c) {
        case 'a':
        case 'b':
        case 'B':
            form = "Ap";
            break;
        case 'c':
        case 'e':
        case 'E':
            form = n == 4 ? "dddd" : "ddd";
            break;
        case 'd':
        case 'h':
        case 'H':
        case 'm':
        case 's':
            form.assign(n, c);
            break;
        case 'k':
            form.assign(n, 'H');
            break;
        case 'K':
            form.assign(n, 'h');
            break;
        case 'L':
        case 'M':
            form = n > 4 ? std::string("MMM") : std::string(n, 'M');
            break;
        case 'O':
            form = "t";
            break;
        case 'S':
            form = n < 3 ? "z" : "zzz";
            break;
        case 'u':
            form = "yyyy";
            break;
        case 'v':
        case 'V':
        case 'z':
            form = "tttt";
            break;
        case 'x':
        case 'X':
            form = n > 1 && (n & 1) ? "ttt" : "tt";
            break;
        case 'y':
            form = n == 2 ? "yy" : "yyyy";
            break;
        case 'Z':
            form = n < 4 ? "tt" : n > 4 ? "ttt" : "t";
            break;
        default:
            break;
        }
        if (!form.empty()) {
            result += separator;
            separator.clear();
            result += form;
            text.remove_prefix(n);
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            // A field the format strings have no form for, with the separator
            // only it needed.
            text.remove_prefix(n);
            while (!separator.empty() && (separator.back() == ' ' || separator.back() == '-'))
                separator.pop_back();
        } else {
            separator += c;
            text.remove_prefix(1);
        }
    }
    return result + separator;
}

namespace {

struct DateFormatDeleter
{
    void operator()(UDateFormat* format) const { udat_close(format); }
};

struct ResourceDeleter
{
    void operator()(UResourceBundle* bundle) const { ures_close(bundle); }
};

using Resource = std::unique_ptr<UResourceBundle, ResourceDeleter>;

} // namespace

static Resource openResource(const char* package, const std::string& id)
{
    UErrorCode status = U_ZERO_ERROR;
    Resource bundle(ures_open(package, id.c_str(), &status));
    return U_FAILURE(status) ? nullptr : std::move(bundle);
}

// A nested resource by its keys, inherited from parent locales as needed.
static Resource findResource(const UResourceBundle* bundle, std::initializer_list<const char*> keys)
{
    Resource current;
    const UResourceBundle* parent = bundle;
    for (const char* key : keys) {
        if (!parent)
            return nullptr;
        UErrorCode status = U_ZERO_ERROR;
        Resource child(ures_getByKeyWithFallback(parent, key, nullptr, &status));
        if (U_FAILURE(status))
            return nullptr;
        current = std::move(child);
        parent = current.get();
    }
    return current;
}

static std::string resourceString(const UResourceBundle* resource)
{
    if (!resource || ures_getType(resource) != URES_STRING)
        return {};
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    const UChar* text = ures_getString(resource, &length, &status);
    return U_FAILURE(status) ? std::string() : toUtf8(text, length);
}

static std::string resourceString(const UResourceBundle* bundle, std::initializer_list<const char*> keys)
{
    return resourceString(findResource(bundle, keys).get());
}

static std::string withKeyword(const std::string& id, const char* keyword, const char* value)
{
    char buffer[ULOC_FULLNAME_CAPACITY + ULOC_KEYWORD_AND_VALUES_CAPACITY];
    std::snprintf(buffer, sizeof(buffer), "%s", id.c_str());
    UErrorCode status = U_ZERO_ERROR;
    uloc_setKeywordValue(keyword, value, buffer, sizeof(buffer), &status);
    return U_FAILURE(status) ? id : std::string(buffer);
}

// Names and formats are always the Gregorian calendar's, also where another
// one is the default (fa_IR, th_TH).
static std::string gregorian(const std::string& id)
{
    return withKeyword(id, "calendar", "gregorian");
}

static std::string keywordValue(const std::string& id, const char* keyword)
{
    char value[ULOC_KEYWORDS_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = uloc_getKeywordValue(id.c_str(), keyword, value, sizeof(value), &status);
    return U_FAILURE(status) || length <= 0 ? std::string() : std::string(value, size_t(length));
}

static std::string baseName(const std::string& id)
{
    char name[ULOC_FULLNAME_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    uloc_getBaseName(id.c_str(), name, sizeof(name), &status);
    return U_FAILURE(status) ? id : std::string(name);
}

static std::string datePattern(const std::string& id, UDateFormatStyle time, UDateFormatStyle date)
{
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<UDateFormat, DateFormatDeleter> format(
            udat_open(time, date, gregorian(id).c_str(), u"UTC", -1, nullptr, -1, &status));
    if (U_FAILURE(status))
        return {};
    UChar buffer[256];
    const int32_t length = udat_toPattern(format.get(), false, buffer, 256, &status);
    return U_FAILURE(status) ? std::string() : toUtf8(buffer, length);
}

static std::string dateSymbol(const UDateFormat* format, UDateFormatSymbolType type, int index)
{
    UChar buffer[128];
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = udat_getSymbols(format, type, index, buffer, 128, &status);
    return U_FAILURE(status) ? std::string() : toUtf8(buffer, length);
}

static std::string numberSymbol(const UNumberFormat* format, UNumberFormatSymbol symbol, const std::string& fallback)
{
    UChar buffer[32];
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = unum_getSymbol(format, symbol, buffer, 32, &status);
    return U_FAILURE(status) || length <= 0 ? fallback : toUtf8(buffer, length);
}

// Without leading white space, Unicode's included (U+00A0, U+202F).
static std::string withoutLeadingSpace(const std::string& text)
{
    size_t i = 0;
    while (i < text.size()) {
        UChar32 c = 0;
        int32_t next = int32_t(i);
        U8_NEXT(text.data(), next, int32_t(text.size()), c);
        if (c < 0 || !u_isUWhiteSpace(c))
            break;
        i = size_t(next);
    }
    return text.substr(i);
}

// How a unit is written after a number: its short form, else its narrow one.
static std::string unitText(const UResourceBundle* units, const std::string& unit)
{
    for (const char* length : {"unitsShort", "unitsNarrow"}) {
        Resource table = findResource(units, {length, "digital", unit.c_str()});
        if (!table)
            continue;
        for (const char* count : {"many", "few", "two", "other", "zero", "one"}) {
            std::string text = resourceString(table.get(), {count});
            if (text.starts_with("{0}"))
                text = withoutLeadingSpace(text.substr(3));
            if (!text.empty())
                return text;
        }
        std::string name = resourceString(table.get(), {"dnam"});
        if (!name.empty())
            return name;
    }
    return {};
}

static void readUnits(Locale::Data& data)
{
    const std::string package = std::string(U_ICUDATA_NAME) + U_TREE_SEPARATOR_STRING + "unit";
    Resource units = openResource(package.c_str(), baseName(data.icuId));
    if (!units)
        return;
    // CLDR's root makes long units an alias of the short ones, which ICU's
    // lookup does not follow from a locale with long units of its own.
    std::string byte = resourceString(units.get(), {"units", "digital", "byte", "dnam"});
    if (byte.empty())
        byte = resourceString(units.get(), {"unitsShort", "digital", "byte", "dnam"});
    data.byteUnit = byte.empty() ? "bytes" : byte;

    // The SI units as CLDR has them, extrapolated past what it has from the
    // localized byte, which also gives the IEC units.
    static const char* const quantifiers[] = {"kilo", "mega", "giga", "tera", "peta", "exa"};
    std::vector<std::string> cache;
    std::string tail = "B";
    for (size_t i = 0; i < 6; ++i) {
        const std::string q = quantifiers[i];
        std::string text = unitText(units.get(), q + "byte");
        const std::string prefix = q == "kilo" ? "k" : std::string(1, char(std::toupper(q[0])));
        if (text.empty()) {
            text = prefix + tail;
        } else if (text.starts_with(prefix)) {
            const std::string rest = text.substr(1);
            tail = std::all_of(cache.begin(), cache.end(), [&rest](const std::string& k) { return k == rest; })
                    ? rest : std::string("B");
            cache.push_back(rest);
        }
        data.siUnits[i] = text;
    }
    std::string suffix = "iB";
    if (!cache.empty()) {
        const std::string byteText = cache.back();
        cache.pop_back();
        if (std::all_of(cache.begin(), cache.end(), [&byteText](const std::string& k) { return k == byteText; }))
            suffix = "i" + byteText;
    }
    for (size_t i = 0; i < 6; ++i)
        data.iecUnits[i] = std::string(1, char(std::toupper(quantifiers[i][0]))) + suffix;
}

// Digit grouping from the default numbering system's decimal pattern.
static void readGrouping(Locale::Data& data, const UResourceBundle* bundle)
{
    std::string system = resourceString(bundle, {"NumberElements", "default"});
    if (system.empty())
        system = "latn";
    std::string pattern = resourceString(bundle, {"NumberElements", system.c_str(), "patterns", "decimalFormat"});
    if (pattern.empty())
        pattern = resourceString(bundle, {"NumberElements", "latn", "patterns", "decimalFormat"});
    const std::string top = resourceString(bundle, {"NumberElements", "minimumGroupingDigits"});
    data.groupTop = top.empty() ? 1 : std::atoi(top.c_str());

    std::vector<std::string> groups;
    const std::string integer = pattern.substr(0, pattern.find('.'));
    size_t start = 0;
    while (true) {
        const size_t comma = integer.find(',', start);
        groups.push_back(integer.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    if (groups.size() > 3)
        groups.erase(groups.begin(), groups.end() - 3);
    if (groups.size() > 2) {
        data.groupLeast = int(groups.back().size());
        data.groupHigher = int(groups[groups.size() - 2].size());
    } else {
        const int size = groups.size() == 2 ? int(groups.back().size()) : 3;
        data.groupLeast = size;
        data.groupHigher = size;
    }
}

static std::shared_ptr<const Locale::Data> createData(const std::string& id)
{
    auto data = std::make_shared<Locale::Data>();
    if (id.empty()) {
        static const char* const longMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                                                 "August", "September", "October", "November", "December"};
        static const char* const longDays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
                                               "Sunday"};
        for (int i = 0; i < 12; ++i) {
            const std::string name = longMonths[i];
            data->months[0][i] = data->months[3][i] = name;
            data->months[1][i] = data->months[4][i] = name.substr(0, 3);
            data->months[2][i] = std::to_string(i + 1);
            data->months[5][i] = name.substr(0, 1);
        }
        for (int i = 0; i < 7; ++i) {
            const std::string name = longDays[i];
            data->days[0][i] = data->days[3][i] = name;
            data->days[1][i] = data->days[4][i] = name.substr(0, 3);
            data->days[2][i] = std::to_string(i + 1);
            data->days[5][i] = name.substr(0, 1);
        }
        return data;
    }

    data->isC = false;
    data->icuId = id;
    const std::string base = baseName(id);
    char likely[ULOC_FULLNAME_CAPACITY];
    UErrorCode status = U_ZERO_ERROR;
    uloc_addLikelySubtags(base.c_str(), likely, ULOC_FULLNAME_CAPACITY, &status);
    if (U_FAILURE(status))
        std::snprintf(likely, sizeof(likely), "%s", base.c_str());
    char language[ULOC_LANG_CAPACITY] = {};
    char territory[ULOC_COUNTRY_CAPACITY] = {};
    status = U_ZERO_ERROR;
    uloc_getLanguage(likely, language, ULOC_LANG_CAPACITY, &status);
    status = U_ZERO_ERROR;
    uloc_getCountry(likely, territory, ULOC_COUNTRY_CAPACITY, &status);
    data->language = language;
    data->territory = territory;
    data->name = territory[0] ? data->language + "_" + territory : data->language;

    const char* icuId = data->icuId.c_str();
    data->dateFormats = {formatFromCldrPattern(datePattern(id, UDAT_NONE, UDAT_FULL)),
                         formatFromCldrPattern(datePattern(id, UDAT_NONE, UDAT_SHORT))};
    data->timeFormats = {formatFromCldrPattern(datePattern(id, UDAT_FULL, UDAT_NONE)),
                         formatFromCldrPattern(datePattern(id, UDAT_SHORT, UDAT_NONE))};

    status = U_ZERO_ERROR;
    std::unique_ptr<UDateFormat, DateFormatDeleter> symbols(
            udat_open(UDAT_NONE, UDAT_FULL, gregorian(id).c_str(), u"UTC", -1, nullptr, -1, &status));
    if (U_SUCCESS(status)) {
        static const UDateFormatSymbolType monthTypes[] = {
            UDAT_MONTHS, UDAT_SHORT_MONTHS, UDAT_NARROW_MONTHS,
            UDAT_STANDALONE_MONTHS, UDAT_STANDALONE_SHORT_MONTHS, UDAT_STANDALONE_NARROW_MONTHS};
        static const UDateFormatSymbolType dayTypes[] = {
            UDAT_WEEKDAYS, UDAT_SHORT_WEEKDAYS, UDAT_NARROW_WEEKDAYS,
            UDAT_STANDALONE_WEEKDAYS, UDAT_STANDALONE_SHORT_WEEKDAYS, UDAT_STANDALONE_NARROW_WEEKDAYS};
        for (int form = 0; form < 6; ++form) {
            for (int month = 0; month < 12; ++month)
                data->months[form][month] = dateSymbol(symbols.get(), monthTypes[form], month);
            // ICU counts from Sunday = 1.
            for (int day = 0; day < 7; ++day)
                data->days[form][day] = dateSymbol(symbols.get(), dayTypes[form], (day + 1) % 7 + 1);
        }
        data->am = dateSymbol(symbols.get(), UDAT_AM_PMS, 0);
        data->pm = dateSymbol(symbols.get(), UDAT_AM_PMS, 1);
    }

    Resource bundle = openResource(nullptr, base);
    // AM and PM are the wide format day periods.
    if (Resource markers = findResource(bundle.get(), {"calendar", "gregorian", "AmPmMarkers"})) {
        UErrorCode markerStatus = U_ZERO_ERROR;
        int32_t length = 0;
        const UChar* am = ures_getStringByIndex(markers.get(), 0, &length, &markerStatus);
        if (U_SUCCESS(markerStatus))
            data->am = toUtf8(am, length);
        markerStatus = U_ZERO_ERROR;
        const UChar* pm = ures_getStringByIndex(markers.get(), 1, &length, &markerStatus);
        if (U_SUCCESS(markerStatus))
            data->pm = toUtf8(pm, length);
    }

    status = U_ZERO_ERROR;
    UCalendar* calendar = ucal_open(nullptr, 0, icuId, UCAL_GREGORIAN, &status);
    if (U_SUCCESS(status)) {
        const int first = ucal_getAttribute(calendar, UCAL_FIRST_DAY_OF_WEEK);
        data->firstDayOfWeek = first == UCAL_SUNDAY ? 7 : first - 1;
        data->weekdays.clear();
        for (int day = 1; day <= 7; ++day) {
            UErrorCode typeStatus = U_ZERO_ERROR;
            const UCalendarWeekdayType type =
                    ucal_getDayOfWeekType(calendar, UCalendarDaysOfWeek(day % 7 + 1), &typeStatus);
            if (U_FAILURE(typeStatus) || type == UCAL_WEEKDAY)
                data->weekdays.push_back(day);
        }
        ucal_close(calendar);
    }

    status = U_ZERO_ERROR;
    UNumberFormat* number = unum_open(UNUM_DECIMAL, nullptr, 0, icuId, nullptr, &status);
    if (U_SUCCESS(status)) {
        data->decimal = numberSymbol(number, UNUM_DECIMAL_SEPARATOR_SYMBOL, ".");
        data->group = numberSymbol(number, UNUM_GROUPING_SEPARATOR_SYMBOL, ",");
        data->minus = numberSymbol(number, UNUM_MINUS_SIGN_SYMBOL, "-");
        data->plus = numberSymbol(number, UNUM_PLUS_SIGN_SYMBOL, "+");
        const std::u16string zero = toUtf16(numberSymbol(number, UNUM_ZERO_DIGIT_SYMBOL, "0"));
        UChar32 c = U'0';
        if (!zero.empty())
            U16_GET(zero.data(), 0, 0, int32_t(zero.size()), c);
        data->zero = char32_t(c);
        unum_close(number);
    }
    if (bundle)
        readGrouping(*data, bundle.get());
    readUnits(*data);

    // ICU answers the measurement system from the territory alone.
    const std::string measure = keywordValue(id, "measure");
    if (measure == "metric") {
        data->measurement = MeasurementSystem::Metric;
    } else if (measure == "ussystem") {
        data->measurement = MeasurementSystem::US;
    } else if (measure == "uksystem") {
        data->measurement = MeasurementSystem::UK;
    } else {
        status = U_ZERO_ERROR;
        const UMeasurementSystem system = ulocdata_getMeasurementSystem(base.c_str(), &status);
        if (U_SUCCESS(status))
            data->measurement = system == UMS_US ? MeasurementSystem::US
                    : system == UMS_UK ? MeasurementSystem::UK : MeasurementSystem::Metric;
    }

    status = U_ZERO_ERROR;
    const icu::number::FormattedNumber weather = icu::number::NumberFormatter::withLocale(icu::Locale(id.c_str()))
                                                         .unit(icu::MeasureUnit::getCelsius())
                                                         .usage("weather")
                                                         .formatDouble(20, status);
    const icu::MeasureUnit unit = weather.getOutputUnit(status);
    if (U_SUCCESS(status) && std::string_view(unit.getIdentifier()) == "fahrenheit")
        data->temperature = TemperatureUnit::Fahrenheit;
    return data;
}

// By ICU id, keywords included.
static std::shared_ptr<const Locale::Data> cachedData(const std::string& id)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<const Locale::Data>> cache;
    std::lock_guard locker(mutex);
    auto it = cache.find(id);
    if (it == cache.end())
        it = cache.emplace(id, createData(id)).first;
    return it->second;
}

static std::shared_ptr<const Locale::Data> dataFor(std::string_view name)
{
    return cachedData(localeId(name));
}

static std::string likelySubtags(const std::string& id)
{
    char likely[ULOC_FULLNAME_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    uloc_addLikelySubtags(id.c_str(), likely, sizeof(likely), &status);
    return U_FAILURE(status) ? id : std::string(likely);
}

static std::string scriptOf(const std::string& id)
{
    char script[ULOC_SCRIPT_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    uloc_getScript(id.c_str(), script, sizeof(script), &status);
    return script;
}

static std::string territoryOf(const std::string& id)
{
    char territory[ULOC_COUNTRY_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    uloc_getCountry(id.c_str(), territory, sizeof(territory), &status);
    return territory;
}

static const char* const kWeekdayKeywords[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

// The language of `language` (an id without codeset) in the region, with a
// script where the region writes the language in another one by default
// ("zh_Hant_SG" for Taiwanese Chinese in Singapore), and the choices as
// keywords.
static std::string regionalId(const std::string& language, const std::string& region,
                              const RegionalPreferences& preferences)
{
    const std::string full = likelySubtags(language.empty() ? std::string("en") : language);
    char code[ULOC_LANG_CAPACITY] = {};
    UErrorCode status = U_ZERO_ERROR;
    uloc_getLanguage(full.c_str(), code, sizeof(code), &status);
    std::string id = *code ? code : "en";
    const std::string territory = region.empty() ? territoryOf(full) : region;
    const std::string script = scriptOf(full);
    if (!territory.empty()) {
        if (!script.empty() && scriptOf(likelySubtags(id + '_' + territory)) != script)
            id += '_' + script;
        id += '_' + territory;
    }

    if (preferences.firstDayOfWeek >= 1 && preferences.firstDayOfWeek <= 7)
        id = withKeyword(id, "fw", kWeekdayKeywords[preferences.firstDayOfWeek - 1]);
    if (preferences.twentyFourHour)
        id = withKeyword(id, "hours", *preferences.twentyFourHour ? "h23" : "h12");
    if (preferences.measurementSystem) {
        static const char* const systems[] = {"metric", "ussystem", "uksystem"};
        id = withKeyword(id, "measure", systems[int(*preferences.measurementSystem)]);
    }
    if (preferences.temperatureUnit)
        id = withKeyword(id, "mu", *preferences.temperatureUnit == TemperatureUnit::Fahrenheit ? "fahrenhe" : "celsius");
    return id;
}

// What the C library's locale setup names for the category: LC_ALL, the
// category's own variable, then LANG.
static std::string categoryName(int mask, int category)
{
    locale_t locale = newlocale(mask, "", locale_t(nullptr));
    if (!locale)
        return "C";
    const char* name = nl_langinfo_l(_NL_LOCALE_NAME(category), locale);
    std::string result = name && *name ? name : "C";
    freelocale(locale);
    return result;
}

static int64_t daysFromCivil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yearOfEra = year - era * 400;
    const int dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return int64_t(era) * 146097 + dayOfEra - 719468;
}

bool CalendarDate::isValid() const
{
    if (month < 1 || month > 12 || day < 1)
        return false;
    static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return day <= lengths[month - 1] + (month == 2 && leap ? 1 : 0);
}

int CalendarDate::dayOfWeek() const
{
    // 1970-01-01 was a Thursday.
    const int64_t days = daysFromCivil(year, month, day);
    return int(((days % 7) + 7 + 3) % 7) + 1;
}

bool ClockTime::isValid() const
{
    return hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60
           && millisecond >= 0 && millisecond < 1000;
}

static std::string systemZoneId()
{
    static const std::string id = [] {
        UChar buffer[128];
        UErrorCode status = U_ZERO_ERROR;
        const int32_t length = ucal_getDefaultTimeZone(buffer, 128, &status);
        return U_FAILURE(status) ? std::string() : toUtf8(buffer, length);
    }();
    return id;
}

DateTime DateTime::fromSecondsSinceEpoch(int64_t seconds)
{
    const std::time_t time = std::time_t(seconds);
    std::tm tm{};
    localtime_r(&time, &tm);
    DateTime result;
    result.date = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
    result.time = {tm.tm_hour, tm.tm_min, tm.tm_sec, 0};
    result.utcOffset = int(tm.tm_gmtoff);
    result.zoneAbbreviation = tm.tm_zone ? tm.tm_zone : "";
    result.zoneId = systemZoneId();
    return result;
}

DateTime DateTime::utc(CalendarDate date, ClockTime time)
{
    return {date, time, 0, "UTC", "UTC"};
}

// Never freed: a reload leaves the previous locale to whoever holds it.
static std::atomic<const Locale*> s_system{nullptr};

static std::mutex s_preferencesMutex;
static std::optional<RegionalPreferences> s_preferences;

const Locale* Locale::readSystem()
{
    std::optional<RegionalPreferences> preferences;
    {
        std::lock_guard locker(s_preferencesMutex);
        preferences = s_preferences;
    }
    auto* system = new Locale;
    const std::string messages = categoryName(LC_MESSAGES_MASK, LC_MESSAGES);
    system->m_messages = dataFor(messages);
    std::string region;
    if (preferences)
        region = preferences->region.empty() ? territoryOf(localeId(categoryName(LC_TIME_MASK, LC_TIME)))
                                             : preferences->region;
    // Formats of the C locale stay as they are without a region.
    if (!region.empty()) {
        system->m_time = cachedData(regionalId(localeId(messages), region, *preferences));
        system->m_numeric = system->m_time;
    } else {
        system->m_time = dataFor(categoryName(LC_TIME_MASK, LC_TIME));
        system->m_numeric = dataFor(categoryName(LC_NUMERIC_MASK, LC_NUMERIC));
    }
    system->m_collation = localeId(categoryName(LC_COLLATE_MASK, LC_COLLATE));
    return system;
}

void Locale::setSystemPreferences(std::optional<RegionalPreferences> preferences)
{
    std::lock_guard locker(s_preferencesMutex);
    s_preferences = std::move(preferences);
}

Locale Locale::regional(std::string_view language, const RegionalPreferences& preferences)
{
    Locale locale(language);
    locale.m_time = cachedData(regionalId(localeId(language), preferences.region, preferences));
    locale.m_numeric = locale.m_time;
    locale.m_groupDigits = true;
    return locale;
}

const Locale& Locale::system()
{
    const Locale* locale = s_system.load(std::memory_order_acquire);
    if (!locale) {
        const Locale* read = readSystem();
        if (s_system.compare_exchange_strong(locale, read, std::memory_order_acq_rel))
            locale = read;
        else
            delete read;
    }
    return *locale;
}

void Locale::reloadSystem()
{
    s_system.store(readSystem(), std::memory_order_release);
}

void Locale::setProcessLocale()
{
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
}

// A variable of /etc/default/locale or /etc/locale.conf without its codeset,
// such as "en_SG" or "sr_RS@latin"; empty for none, C and POSIX.
static std::string systemLocaleVariable(std::string_view name)
{
    const std::string prefix = std::string(name) + '=';
    for (const char* path : {"/etc/default/locale", "/etc/locale.conf"}) {
        const std::optional<std::string> contents = file::read(path);
        if (!contents)
            continue;
        for (const std::string& line : strings::split(*contents, '\n')) {
            std::string value(strings::trimmed(line));
            if (!value.starts_with(prefix))
                continue;
            value.erase(0, prefix.size());
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            const size_t codeset = value.find('.');
            if (codeset != std::string::npos)
                value.erase(codeset, value.find('@', codeset) - codeset);
            if (!value.empty() && value != "C" && value != "POSIX")
                return value;
        }
    }
    return {};
}

std::string Locale::systemLanguage()
{
    const std::string language = systemLocaleVariable("LANG");
    return language.empty() ? "en_US" : language;
}

std::string Locale::systemRegion()
{
    for (const char* name : {"LC_TIME", "LANG"}) {
        const std::string territory = territoryOf(localeId(systemLocaleVariable(name)));
        if (!territory.empty())
            return territory;
    }
    return {};
}

Locale::Locale(std::string_view name)
    : m_messages(dataFor(name))
    , m_time(m_messages)
    , m_numeric(m_messages)
    , m_collation(localeId(name))
    , m_groupDigits(!m_messages->isC)
{
}

Locale::~Locale() = default;
Locale::Locale(const Locale&) = default;
Locale& Locale::operator=(const Locale&) = default;

std::string Locale::name() const
{
    return m_messages->name;
}

std::string Locale::language() const
{
    return m_messages->language;
}

std::string Locale::territory() const
{
    return m_time->territory;
}

std::string Locale::formatsId() const
{
    return m_time->icuId;
}

bool Locale::usesTwentyFourHourTime() const
{
    bool quoted = false;
    for (const char c : m_time->timeFormats[1]) {
        if (c == '\'')
            quoted = !quoted;
        else if (!quoted && (c == 'h' || c == 'H'))
            return c == 'H';
    }
    return true;
}

MeasurementSystem Locale::measurementSystem() const
{
    return m_time->measurement;
}

TemperatureUnit Locale::temperatureUnit() const
{
    return m_time->temperature;
}

RegionalPreferences RegionalPreferences::read(const std::string& path)
{
    const Settings settings(path);
    RegionalPreferences preferences;
    preferences.region = strings::toAsciiUpper(settings.stringValue("Region/region"));
    const int64_t day = settings.intValue("Region/firstWeekday");
    preferences.firstDayOfWeek = day >= 1 && day <= 7 ? int(day) : 0;
    if (settings.contains("Region/twentyFourHour"))
        preferences.twentyFourHour = settings.boolValue("Region/twentyFourHour");
    const std::string measurement = settings.stringValue("Region/measurement");
    if (measurement == "metric")
        preferences.measurementSystem = MeasurementSystem::Metric;
    else if (measurement == "us")
        preferences.measurementSystem = MeasurementSystem::US;
    else if (measurement == "uk")
        preferences.measurementSystem = MeasurementSystem::UK;
    const std::string temperature = settings.stringValue("Region/temperature");
    if (temperature == "celsius")
        preferences.temperatureUnit = TemperatureUnit::Celsius;
    else if (temperature == "fahrenheit")
        preferences.temperatureUnit = TemperatureUnit::Fahrenheit;
    return preferences;
}

std::string RegionalPreferences::sessionPath()
{
    return paths::cutefishConfig("services");
}

std::string Locale::dateFormat(FormatType type) const
{
    return m_time->dateFormats[type == FormatType::Long ? 0 : 1];
}

std::string Locale::timeFormat(FormatType type) const
{
    return m_time->timeFormats[type == FormatType::Long ? 0 : 1];
}

std::string Locale::dateTimeFormat(FormatType type) const
{
    return dateFormat(type) + " " + timeFormat(type);
}

static std::string nameAt(const std::array<std::array<std::string, 12>, 6>& names, int index, Locale::FormatType type,
                          bool standalone)
{
    if (index < 1 || index > 12)
        return {};
    return names[size_t(int(type) + (standalone ? 3 : 0))][size_t(index - 1)];
}

static std::string nameAt(const std::array<std::array<std::string, 7>, 6>& names, int index, Locale::FormatType type,
                          bool standalone)
{
    if (index < 1 || index > 7)
        return {};
    return names[size_t(int(type) + (standalone ? 3 : 0))][size_t(index - 1)];
}

std::string Locale::monthName(int month, FormatType type) const
{
    return nameAt(m_time->months, month, type, false);
}

std::string Locale::standaloneMonthName(int month, FormatType type) const
{
    return nameAt(m_time->months, month, type, true);
}

std::string Locale::dayName(int day, FormatType type) const
{
    return nameAt(m_time->days, day, type, false);
}

std::string Locale::standaloneDayName(int day, FormatType type) const
{
    return nameAt(m_time->days, day, type, true);
}

std::string Locale::amText() const
{
    return m_time->am;
}

std::string Locale::pmText() const
{
    return m_time->pm;
}

int Locale::firstDayOfWeek() const
{
    return m_time->firstDayOfWeek;
}

std::vector<int> Locale::weekdays() const
{
    return m_messages->weekdays;
}

// The ASCII digits in the locale's digits.
static std::string localizedDigits(std::string_view digits, char32_t zero)
{
    if (zero == U'0')
        return std::string(digits);
    std::string result;
    for (char c : digits)
        result += c >= '0' && c <= '9' ? fromCodePoint(zero + char32_t(c - '0')) : std::string(1, c);
    return result;
}

static std::string integerText(int64_t value, int width, const Locale::Data& digits)
{
    const bool negative = value < 0;
    std::string text = std::to_string(negative ? -uint64_t(value) : uint64_t(value));
    const int used = int(text.size()) + (negative ? 1 : 0);
    if (used < width)
        text.insert(0, size_t(width - used), '0');
    return (negative ? digits.minus : std::string()) + localizedDigits(text, digits.zero);
}

// Copies quoted text, with '' as a literal quote; i is at the opening
// quote.
static std::string readQuoted(std::string_view format, size_t& i)
{
    ++i;
    if (i == format.size())
        return {};
    if (format[i] == '\'') {
        ++i;
        return "'";
    }
    std::string result;
    while (i < format.size()) {
        if (format[i] == '\'') {
            if (i + 1 < format.size() && format[i + 1] == '\'') {
                result += '\'';
                i += 2;
            } else {
                break;
            }
        } else {
            result += format[i++];
        }
    }
    if (i < format.size())
        ++i;
    return result;
}

static bool containsAmPm(std::string_view format)
{
    size_t i = 0;
    while (i < format.size()) {
        if (format[i] == '\'') {
            readQuoted(format, i);
            continue;
        }
        if (format[i] == 'a' || format[i] == 'A')
            return true;
        ++i;
    }
    return false;
}

static std::string offsetText(int seconds, bool colon)
{
    const int minutes = std::abs(seconds) / 60;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), colon ? "%c%02d:%02d" : "%c%02d%02d", seconds < 0 ? '-' : '+',
                  minutes / 60, minutes % 60);
    return buffer;
}

static std::string zoneName(const DateTime& when, const Locale::Data& names)
{
    if (when.zoneId.empty())
        return when.zoneAbbreviation;
    const std::u16string id = toUtf16(when.zoneId);
    UErrorCode status = U_ZERO_ERROR;
    UCalendar* calendar = ucal_open(id.data(), int32_t(id.size()), names.icuId.c_str(), UCAL_GREGORIAN, &status);
    if (U_FAILURE(status))
        return when.zoneAbbreviation;
    ucal_setDateTime(calendar, when.date.year, when.date.month - 1, when.date.day, when.time.hour, when.time.minute,
                     when.time.second, &status);
    const bool daylight = ucal_inDaylightTime(calendar, &status);
    UChar buffer[128];
    status = U_ZERO_ERROR;
    const int32_t length = ucal_getTimeZoneDisplayName(calendar, daylight ? UCAL_DST : UCAL_STANDARD,
                                                       names.icuId.c_str(), buffer, 128, &status);
    ucal_close(calendar);
    return U_FAILURE(status) ? when.zoneAbbreviation : toUtf8(buffer, length);
}

// Writes a date and time by a format string: names from one locale, digits
// from another, as the system locale does with LC_TIME and LC_NUMERIC.
static std::string formatDateTime(std::string_view format, const CalendarDate* date, const ClockTime* time,
                                  const DateTime* when, const Locale::Data& names, const Locale::Data& digits)
{
    const bool formatDate = date && date->isValid();
    const bool formatTime = time && time->isValid();
    if (!formatDate && !formatTime)
        return {};

    std::string result;
    const auto number = [&](int value, int repeat) {
        result += integerText(value, repeat > 1 ? repeat : 0, digits);
    };
    size_t i = 0;
    while (i < format.size()) {
        if (format[i] == '\'') {
            result += readQuoted(format, i);
            continue;
        }
        const char c = format[i];
        int repeat = 1;
        while (i + size_t(repeat) < format.size() && format[i + size_t(repeat)] == c)
            ++repeat;
        bool used = false;
        if (formatDate) {
            switch (c) {
            case 'y':
                used = true;
                repeat = repeat >= 4 ? 4 : repeat >= 2 ? 2 : 1;
                if (repeat == 4)
                    number(date->year, date->year < 0 ? 5 : 4);
                else if (repeat == 2)
                    number(date->year % 100, 2);
                else
                    result += c;
                break;
            case 'M':
                used = true;
                repeat = std::min(repeat, 4);
                if (repeat <= 2)
                    number(date->month, repeat);
                else
                    result += nameAt(names.months, date->month,
                                     repeat == 3 ? Locale::FormatType::Short : Locale::FormatType::Long, false);
                break;
            case 'd':
                used = true;
                repeat = std::min(repeat, 4);
                if (repeat <= 2)
                    number(date->day, repeat);
                else
                    result += nameAt(names.days, date->dayOfWeek(),
                                     repeat == 3 ? Locale::FormatType::Short : Locale::FormatType::Long, false);
                break;
            default:
                break;
            }
        }
        if (!used && formatTime) {
            switch (c) {
            case 'h': {
                used = true;
                repeat = std::min(repeat, 2);
                int hour = time->hour;
                if (containsAmPm(format)) {
                    if (hour > 12)
                        hour -= 12;
                    else if (hour == 0)
                        hour = 12;
                }
                number(hour, repeat);
                break;
            }
            case 'H':
                used = true;
                repeat = std::min(repeat, 2);
                number(time->hour, repeat);
                break;
            case 'm':
                used = true;
                repeat = std::min(repeat, 2);
                number(time->minute, repeat);
                break;
            case 's':
                used = true;
                repeat = std::min(repeat, 2);
                number(time->second, repeat);
                break;
            case 'A':
            case 'a': {
                used = true;
                std::string text = time->hour < 12 ? names.am : names.pm;
                repeat = 1;
                const char next = i + 1 < format.size() ? format[i + 1] : '\0';
                if (next == 'p' || next == 'P')
                    ++repeat;
                if (c == 'A' && (repeat == 1 || next == 'P'))
                    text = caseMapped(text, true);
                else if (c == 'a' && (repeat == 1 || next == 'p'))
                    text = caseMapped(text, false);
                // "Ap" and "aP" keep CLDR's own case.
                result += text;
                break;
            }
            case 'z': {
                used = true;
                repeat = std::min(repeat, 3);
                std::string text = integerText(time->millisecond, 3, digits);
                if (repeat != 3) {
                    const std::string zero = fromCodePoint(digits.zero);
                    for (int k = 0; k < 2 && text.ends_with(zero); ++k)
                        text.resize(text.size() - zero.size());
                }
                result += text;
                break;
            }
            case 't': {
                used = true;
                repeat = std::min(repeat, 4);
                const DateTime now = when ? *when : DateTime::fromSecondsSinceEpoch(std::time(nullptr));
                if (repeat == 4) {
                    result += zoneName(now, names);
                } else if (repeat >= 2) {
                    result += offsetText(now.utcOffset, repeat == 3);
                } else {
                    std::string text = now.zoneAbbreviation;
                    if (text.starts_with("UTC") && text.size() == 6)
                        text += ":00";
                    result += text;
                }
                break;
            }
            default:
                break;
            }
        }
        if (!used)
            result.append(size_t(repeat), c);
        i += size_t(repeat);
    }
    return result;
}

std::string Locale::toString(const CalendarDate& date, FormatType type) const
{
    return formatDateTime(dateFormat(type), &date, nullptr, nullptr, *m_time, *m_time);
}

std::string Locale::toString(const ClockTime& time, FormatType type) const
{
    return formatDateTime(timeFormat(type), nullptr, &time, nullptr, *m_time, *m_time);
}

std::string Locale::toString(const DateTime& dateTime, FormatType type) const
{
    return formatDateTime(dateTimeFormat(type), &dateTime.date, &dateTime.time, &dateTime, *m_time, *m_time);
}

std::string Locale::toString(const CalendarDate& date, std::string_view format) const
{
    return formatDateTime(format, &date, nullptr, nullptr, *m_time, *m_numeric);
}

std::string Locale::toString(const ClockTime& time, std::string_view format) const
{
    return formatDateTime(format, nullptr, &time, nullptr, *m_time, *m_numeric);
}

std::string Locale::toString(const DateTime& dateTime, std::string_view format) const
{
    return formatDateTime(format, &dateTime.date, &dateTime.time, &dateTime, *m_time, *m_numeric);
}

std::string Locale::decimalPoint() const
{
    return m_numeric->decimal;
}

std::string Locale::groupSeparator() const
{
    return m_numeric->group;
}

std::string Locale::negativeSign() const
{
    return m_numeric->minus;
}

std::string Locale::positiveSign() const
{
    return m_numeric->plus;
}

std::string Locale::zeroDigit() const
{
    return fromCodePoint(m_numeric->zero);
}

// Marks where separators go in a run of ASCII integer digits.
static std::string grouped(const std::string& integer, const Locale::Data& sizes)
{
    std::string result = integer;
    int64_t i = int64_t(integer.size()) - sizes.groupLeast;
    if (i >= sizes.groupTop) {
        result.insert(size_t(i), 1, ',');
        while ((i -= sizes.groupHigher) > 0)
            result.insert(size_t(i), 1, ',');
    }
    return result;
}

// ASCII digits and ',' marks in the locale's digits and separator.
static std::string localizedNumber(std::string_view text, const Locale::Data& numeric)
{
    std::string result;
    for (char c : text) {
        if (c == ',')
            result += numeric.group;
        else if (numeric.zero != U'0' && c >= '0' && c <= '9')
            result += fromCodePoint(numeric.zero + char32_t(c - '0'));
        else
            result += c;
    }
    return result;
}

std::string Locale::toString(double value, int decimals) const
{
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? m_numeric->minus + "inf" : "inf";
    decimals = std::max(0, decimals);

    // The exact decimal expansion of the double, rounded half away from
    // zero.
    std::vector<char> buffer(size_t(400 + decimals));
    std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals + 40, std::fabs(value));
    const std::string digits = buffer.data();
    const size_t point = digits.find('.');
    std::string integer = digits.substr(0, point);
    std::string fraction = digits.substr(point + 1);
    const bool roundUp = fraction[size_t(decimals)] >= '5';
    fraction.resize(size_t(decimals));
    if (roundUp) {
        std::string whole = integer + fraction;
        int i = int(whole.size()) - 1;
        while (i >= 0 && whole[size_t(i)] == '9')
            whole[size_t(i--)] = '0';
        if (i < 0)
            whole.insert(whole.begin(), '1');
        else
            ++whole[size_t(i)];
        integer = whole.substr(0, whole.size() - size_t(decimals));
        fraction = whole.substr(whole.size() - size_t(decimals));
    }
    // The system locale groups as its messages locale does.
    std::string result = value < 0 ? m_numeric->minus : std::string();
    result += localizedNumber(m_groupDigits ? grouped(integer, *m_messages) : integer, *m_numeric);
    if (decimals > 0)
        result += m_numeric->decimal + localizedNumber(fraction, *m_numeric);
    return result;
}

std::string Locale::toString(int64_t value) const
{
    const std::string integer = std::to_string(value < 0 ? -uint64_t(value) : uint64_t(value));
    std::string result = value < 0 ? m_numeric->minus : std::string();
    result += localizedNumber(m_groupDigits ? grouped(integer, *m_messages) : integer, *m_numeric);
    return result;
}

std::string Locale::formattedDataSize(int64_t bytes, int precision, DataSizeFormat format) const
{
    const uint64_t magnitude = bytes < 0 ? -uint64_t(bytes) : uint64_t(bytes);
    int power = 0;
    int base = 1000;
    if (bytes != 0) {
        if (format == DataSizeFormat::Si) {
            power = int(std::log10(double(magnitude)) / 3);
        } else {
            power = (63 - __builtin_clzll(magnitude)) / 10;
            base = 1024;
        }
    }
    const std::string number = power ? toString(double(bytes) / std::pow(double(base), power),
                                                std::min(precision, 3 * power))
                                     : toString(bytes);
    std::string unit;
    if (power > 0)
        unit = format == DataSizeFormat::Iec ? m_messages->iecUnits[size_t(power - 1)]
                                             : m_messages->siUnits[size_t(power - 1)];
    else
        unit = m_messages->byteUnit;
    return number + " " + unit;
}

namespace {

struct CollatorDeleter
{
    void operator()(UCollator* collator) const { ucol_close(collator); }
};

} // namespace

int Locale::compare(std::string_view a, std::string_view b) const
{
    if (m_collation.empty()) {
        const int order = a.compare(b);
        return order < 0 ? -1 : order > 0 ? 1 : 0;
    }
    thread_local std::unordered_map<std::string, std::unique_ptr<UCollator, CollatorDeleter>> collators;
    auto it = collators.find(m_collation);
    if (it == collators.end()) {
        UErrorCode status = U_ZERO_ERROR;
        UCollator* collator = ucol_open(m_collation.c_str(), &status);
        it = collators.emplace(m_collation, U_FAILURE(status) ? nullptr : collator).first;
    }
    if (!it->second) {
        const int order = a.compare(b);
        return order < 0 ? -1 : order > 0 ? 1 : 0;
    }
    UErrorCode status = U_ZERO_ERROR;
    const UCollationResult order = ucol_strcollUTF8(it->second.get(), a.data(), int32_t(a.size()), b.data(),
                                                    int32_t(b.size()), &status);
    return order == UCOL_LESS ? -1 : order == UCOL_GREATER ? 1 : 0;
}

std::string Locale::territoryName(std::string_view code)
{
    const std::string id = "_" + std::string(code);
    UChar buffer[128];
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = uloc_getDisplayCountry(id.c_str(), "en", buffer, 128, &status);
    return U_FAILURE(status) ? std::string() : toUtf8(buffer, length);
}

} // namespace cutefish::base
