#pragma once

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>

// Civil-date helpers for the ISO-8601 date dialect.
//
// A date is a STRING Value holding 'YYYY-MM-DD'. There is no TypeId::DATE, and
// deliberately so: ISO-8601 sorts lexicographically, so STRING dates already
// get zone-map chunk pruning, scanColumn<std::string>'s tight comparison loop,
// and BETWEEN — none of which a new type would inherit for free.
//
// The conversions are hand-rolled because the project builds as C++17 and
// std::chrono::year_month_day is C++20. daysFromCivil/civilFromDays are Howard
// Hinnant's public-domain algorithms, valid for the proleptic Gregorian
// calendar over the full int range.

// Representable range. formatIsoDate writes exactly four year digits, so a year
// outside [0, 9999] cannot be rendered — it wrapped modulo 10000 instead, and
// silently: `date '1994-01-01' + interval '100000' year` printed "1994-01-01",
// the input date unchanged, with no error. A negative year was worse — year -6
// made '0' + (-6) the byte '*', so `- interval '2000' year` produced the string
// "000*-01-01", which then flowed into comparisons and zone maps as an ordinary
// STRING. Matches SQLite's date() range.
constexpr int MIN_DATE_YEAR = 0;
constexpr int MAX_DATE_YEAR = 9999;

// Widest interval magnitude that could still land inside that range. Checked
// BEFORE any addition, which is what makes the arithmetic below overflow-free:
// years 0000-9999 span ~3.65M days and 120k months, so anything past these
// bounds is out of range regardless of the starting date. Deliberately loose —
// they exist to make the arithmetic safe; the year check afterwards decides the
// actual answer. This mirrors checked_arith.h's stance that an analytical
// engine must not answer with a silently wrong number.
constexpr int64_t MAX_INTERVAL_DAYS   = 4000000;
constexpr int64_t MAX_INTERVAL_MONTHS = 200000;
constexpr int64_t MAX_INTERVAL_YEARS  = 20000;

[[noreturn]] inline void throwDateOutOfRange() {
    throw std::runtime_error(
        "date arithmetic result is outside the supported range "
        "0000-01-01 .. 9999-12-31");
}

inline bool isLeapYear(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

inline int daysInMonth(int y, int m) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && isLeapYear(y)) return 29;
    return kDays[m - 1];
}

// Strict YYYY-MM-DD with a real calendar check. '1998-02-30' is rejected here,
// at parse time, rather than becoming a string no row can ever equal.
inline bool isIsoDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    const int digits[8] = {0, 1, 2, 3, 5, 6, 8, 9};
    for (int i : digits) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    int y = std::stoi(s.substr(0, 4));
    int m = std::stoi(s.substr(5, 2));
    int d = std::stoi(s.substr(8, 2));
    return m >= 1 && m <= 12 && d >= 1 && d <= daysInMonth(y, m);
}

// Days since 1970-01-01. Caller guarantees a valid civil date (isIsoDate).
inline int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Inverse of daysFromCivil.
inline void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);          // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t  yr  = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);          // [0, 365]
    const unsigned mp  = (5 * doy + 2) / 153;                              // [0, 11]
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);                    // [1, 31]
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);                       // [1, 12]
    y = static_cast<int>(yr + (m <= 2));
}

inline std::string formatIsoDate(int y, int m, int d) {
    std::string s(10, '0');
    s[0] = static_cast<char>('0' + (y / 1000) % 10);
    s[1] = static_cast<char>('0' + (y / 100) % 10);
    s[2] = static_cast<char>('0' + (y / 10) % 10);
    s[3] = static_cast<char>('0' + y % 10);
    s[4] = '-';
    s[5] = static_cast<char>('0' + m / 10);
    s[6] = static_cast<char>('0' + m % 10);
    s[7] = '-';
    s[8] = static_cast<char>('0' + d / 10);
    s[9] = static_cast<char>('0' + d % 10);
    return s;
}

// date + n days. Signed n; the caller negates for subtraction.
inline std::string addDays(const std::string& iso, int64_t n) {
    // bound first: with |n| capped, daysFromCivil's ±3.7M can never overflow
    if (n > MAX_INTERVAL_DAYS || n < -MAX_INTERVAL_DAYS) throwDateOutOfRange();
    int y = std::stoi(iso.substr(0, 4));
    int m = std::stoi(iso.substr(5, 2));
    int d = std::stoi(iso.substr(8, 2));
    int64_t z = daysFromCivil(y, m, d) + n;
    civilFromDays(z, y, m, d);
    if (y < MIN_DATE_YEAR || y > MAX_DATE_YEAR) throwDateOutOfRange();
    return formatIsoDate(y, m, d);
}

// date + n months, clamping the day to the target month's length:
// 2024-01-31 + 1 month = 2024-02-29, matching SQL interval semantics.
// Years are months * 12, so both units share this one path.
inline std::string addMonths(const std::string& iso, int64_t n) {
    // bound first: y*12 is at most 119988, so a capped n cannot overflow total
    if (n > MAX_INTERVAL_MONTHS || n < -MAX_INTERVAL_MONTHS) throwDateOutOfRange();
    int y = std::stoi(iso.substr(0, 4));
    int m = std::stoi(iso.substr(5, 2));
    int d = std::stoi(iso.substr(8, 2));

    int64_t total = static_cast<int64_t>(y) * 12 + (m - 1) + n;
    // floor division: -1 / 12 must be -1, not 0, so negative years land right
    int64_t ny = total >= 0 ? total / 12 : -(((-total) + 11) / 12);
    int nm = static_cast<int>(total - ny * 12) + 1;

    if (ny < MIN_DATE_YEAR || ny > MAX_DATE_YEAR) throwDateOutOfRange();
    y = static_cast<int>(ny);
    m = nm;
    int last = daysInMonth(y, m);
    if (d > last) d = last;
    return formatIsoDate(y, m, d);
}

// date + n years. Separate from addMonths so the n*12 conversion is bounded
// before it happens — multiplying an unchecked interval count by 12 is its own
// overflow.
inline std::string addYears(const std::string& iso, int64_t n) {
    if (n > MAX_INTERVAL_YEARS || n < -MAX_INTERVAL_YEARS) throwDateOutOfRange();
    return addMonths(iso, n * 12);
}
