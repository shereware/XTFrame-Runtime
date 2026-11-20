#include <locale.h>  // _configthreadlocale (Windows), newlocale / uselocale (POSIX)

#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

#include "binding_runtime_internal.h"  // Code/Runtime/core/src: toNum() / parseDouble(), exercised directly.
#include "doctest.h"
#if defined(__APPLE__)
#include <xlocale.h>
#endif

/// T-797 / T-798 (Docs/design/23-host-integration.md §23.ai rule 7): the binding runtime's two number
/// readers -- toNum() of a string value and parseDouble() of a port text -- behave case for case as the
/// strtod() they used in the "C" locale, except hexadecimal, which is not a number since T-798, and a
/// comma-decimal LC_NUMERIC on the calling thread does not change what they read.

using namespace xt;
using namespace xt::binding_runtime_detail;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// One row: the text, what toNum() gives, whether parseDouble() accepts it and the value it gives.
struct Row {
    const char* text;
    double num;
    bool parses;
    double parsed;
    /// Differs from strtod (T-798 decision: hexadecimal is rejected; toNum reads the "0" before the 'x').
    bool differsFromStrtod = false;
};

// The behaviour of the strtod() implementation in the "C" locale (MSVC UCRT, T-797 before the switch),
// except the rows marked as differing from strtod.
const Row kRows[] = {
    {"", 0.0, false, 0.0},
    {"12.5", 12.5, true, 12.5},
    {"0", 0.0, true, 0.0},
    {"00012", 12.0, true, 12.0},
    {"-0", -0.0, true, -0.0},
    // Leading whitespace: every isspace() character of the "C" locale.
    {"  12.5", 12.5, true, 12.5},
    {"\t\n\v\f\r7", 7.0, true, 7.0},
    {"   ", 0.0, false, 0.0},
    // Trailing characters: parseDouble() skips trailing spaces only.
    {"12.5 ", 12.5, true, 12.5},
    {"12.5   ", 12.5, true, 12.5},
    {"12.5\t", 12.5, false, 0.0},
    {"12.5\n", 12.5, false, 0.0},
    {"12abc", 12.0, false, 0.0},
    {"1,5", 1.0, false, 0.0},
    {"1_000", 1.0, false, 0.0},
    {"12.5px", 12.5, false, 0.0},
    {"abc", 0.0, false, 0.0},
    {" x1", 0.0, false, 0.0},
    // Signs.
    {"+3", 3.0, true, 3.0},
    {"-3", -3.0, true, -3.0},
    {"  -3.25", -3.25, true, -3.25},
    {"+-3", 0.0, false, 0.0},
    {"-+3", 0.0, false, 0.0},
    {"--3", 0.0, false, 0.0},
    {"++3", 0.0, false, 0.0},
    {"+", 0.0, false, 0.0},
    {"-", 0.0, false, 0.0},
    {"+ 3", 0.0, false, 0.0},
    {"+.5", 0.5, true, 0.5},
    {"-.5", -0.5, true, -0.5},
    // Decimal point.
    {".5", 0.5, true, 0.5},
    {"5.", 5.0, true, 5.0},
    {".", 0.0, false, 0.0},
    {"-.", 0.0, false, 0.0},
    {"1.2.3", 1.2, false, 0.0},
    // Exponent.
    {"1e3", 1000.0, true, 1000.0},
    {"1E-2", 0.01, true, 0.01},
    {"2.5e+2", 250.0, true, 250.0},
    {"-1.5E3", -1500.0, true, -1500.0},
    {"1e", 1.0, false, 0.0},
    {"1e+", 1.0, false, 0.0},
    {"1e-", 1.0, false, 0.0},
    {"1ex", 1.0, false, 0.0},
    {".5e1", 5.0, true, 5.0},
    {"e5", 0.0, false, 0.0},
    // Hexadecimal: rejected (T-798 decision; strtod accepted these).
    {"0x1A", 0.0, false, 0.0, true},
    {"0X1a", 0.0, false, 0.0, true},
    {"-0x10", -0.0, false, 0.0, true},
    {"+0x10", 0.0, false, 0.0, true},
    {"0x1p4", 0.0, false, 0.0, true},
    {"0x1.8", 0.0, false, 0.0, true},
    {"0x.8", 0.0, false, 0.0, true},
    {"0x", 0.0, false, 0.0},
    {"0xg", 0.0, false, 0.0},
    {"0x1g", 0.0, false, 0.0, true},
    // inf / nan.
    {"inf", kInf, true, kInf},
    {"INF", kInf, true, kInf},
    {"-inf", -kInf, true, -kInf},
    {"+inf", kInf, true, kInf},
    {"infinity", kInf, true, kInf},
    {"-Infinity", -kInf, true, -kInf},
    {"infinit", kInf, false, 0.0},
    {"infx", kInf, false, 0.0},
    {"in", 0.0, false, 0.0},
    {"nan", kNaN, true, kNaN},
    {"NaN", kNaN, true, kNaN},
    {"nan(123)", kNaN, true, kNaN},
    {"nan(", kNaN, false, 0.0},
    {"nanx", kNaN, false, 0.0},
    // Out of range: overflow to infinity, underflow to zero, subnormals kept.
    {"1e400", kInf, true, kInf},
    {"-1e400", -kInf, true, -kInf},
    {"1e-400", 0.0, true, 0.0},
    {"-1e-400", -0.0, true, -0.0},
    {"4e-320", 4e-320, true, 4e-320},
    {"1.7976931348623157e308", 1.7976931348623157e308, true, 1.7976931348623157e308},
};

/// Equal as doubles, NaN equal to NaN; the sign of a zero, an infinity and a NaN must match too.
bool sameDouble(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b);
    }
    return a == b && std::signbit(a) == std::signbit(b);
}

/// Checks every row against toNum() / parseDouble(), and against std::strtod itself when
/// `compareStrtod` (the "C" locale only): equal for every row except those marked as differing from
/// strtod (T-798 decision), which must differ.
void checkRows(bool compareStrtod) {
    for (const Row& row : kRows) {
        CAPTURE(std::string(row.text));
        const double num = toNum(str(row.text));
        CHECK_MESSAGE(sameDouble(num, row.num), "toNum gave " << num << ", expected " << row.num);
        double parsed = -12345.0;
        const bool ok = parseDouble(row.text, parsed);
        CHECK(ok == row.parses);
        if (ok && row.parses) {
            CHECK_MESSAGE(sameDouble(parsed, row.parsed), "parseDouble gave " << parsed);
        }
        if (compareStrtod) {
            char* end = nullptr;
            const double reference = std::strtod(row.text, &end);
            const bool same = sameDouble(num, end == row.text ? 0.0 : reference);
            if (row.differsFromStrtod) {
                CHECK_MESSAGE(!same, "differs from strtod (T-798 decision)");
            } else {
                CHECK(same);
            }
        }
    }
}

}  // namespace

TEST_CASE(
    "T-797 binding numbers: toNum / parseDouble match strtod in the \"C\" locale, case for case (T-798: hex "
    "rejected)") {
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current != nullptr ? current : "C";
    std::setlocale(LC_NUMERIC, "C");
    checkRows(true);
    std::setlocale(LC_NUMERIC, saved.c_str());
}

namespace {

/// What "1,5" reads as: toNum() and parseDouble() (and its value).
struct CommaResult {
    double num;
    bool parses;
    double parsed;
};

CommaResult readComma() {
    CommaResult r{toNum(str("1,5")), false, 0.0};
    r.parses = parseDouble("1,5", r.parsed);
    return r;
}

/// Checks "1.5" and "1,5" on a thread whose LC_NUMERIC uses ',' as the decimal point, against `c`
/// (what "1,5" read in the "C" locale). Returns false (and says why) when no such locale exists.
bool checkUnderCommaLocale(const CommaResult& c) {
    auto check = [&c] {
        CHECK(std::strcmp(localeconv()->decimal_point, ",") == 0);
        CHECK(sameDouble(toNum(str("1.5")), 1.5));
        double parsed = 0.0;
        CHECK(parseDouble("1.5", parsed));
        CHECK(sameDouble(parsed, 1.5));
        const CommaResult r = readComma();
        CHECK(sameDouble(r.num, c.num));
        CHECK(r.parses == c.parses);
        if (r.parses && c.parses) {
            CHECK(sameDouble(r.parsed, c.parsed));
        }
        checkRows(false);
    };
#if defined(_WIN32)
    const int previous = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current != nullptr ? current : "C";
    const char* names[] = {"de-DE", "German"};
    bool found = false;
    for (const char* name : names) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr &&
            std::strcmp(localeconv()->decimal_point, ",") == 0) {
            found = true;
            break;
        }
    }
    if (found) {
        check();
    } else {
        MESSAGE(
            "skipped: neither \"de-DE\" nor \"German\" is an LC_NUMERIC locale with ',' as the decimal "
            "point");
    }
    std::setlocale(LC_NUMERIC, saved.c_str());
    _configthreadlocale(previous);
    return found;
#else
    const char* names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE"};
    locale_t comma = static_cast<locale_t>(0);
    for (const char* name : names) {
        comma = newlocale(LC_NUMERIC_MASK, name, static_cast<locale_t>(0));
        if (comma != static_cast<locale_t>(0)) {
            break;
        }
    }
    if (comma == static_cast<locale_t>(0)) {
        MESSAGE("skipped: no de_DE LC_NUMERIC locale is installed");
        return false;
    }
    const locale_t previous = uselocale(comma);
    check();
    uselocale(previous);
    freelocale(comma);
    return true;
#endif
}

}  // namespace

TEST_CASE("T-798 binding numbers: a comma-decimal LC_NUMERIC on the thread does not change what is read") {
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current != nullptr ? current : "C";
    std::setlocale(LC_NUMERIC, "C");
    const CommaResult c = readComma();
    std::setlocale(LC_NUMERIC, saved.c_str());
    CHECK(sameDouble(c.num, 1.0));
    CHECK_FALSE(c.parses);

    // On its own thread so a failure cannot leave this thread's locale changed.
    std::thread worker([&c] { checkUnderCommaLocale(c); });
    worker.join();
}
