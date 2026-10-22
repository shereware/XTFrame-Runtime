#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/charconv.h"
#include "xt/compile/compile.h"

/// T-268: number text that does not depend on the process locale. xt::formatFloat against the
/// C locale's printf("%.*g"), and a document compiled under a comma-decimal LC_NUMERIC (what a Qt
/// process takes from the environment on Linux / macOS) must give the same IR bytes as under "C".

namespace fs = std::filesystem;

namespace {

std::string format(double value, int precision) {
    char buf[64];
    const size_t n = xt::formatFloat(buf, sizeof(buf), value, precision);
    return std::string(buf, n);
}

std::string cFormat(double value, int precision) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
    return buf;
}

std::string fixed(double value, int decimals) {
    char buf[512];
    const size_t n = xt::formatFixed(buf, sizeof(buf), value, decimals);
    return std::string(buf, n);
}

std::string cFixed(double value, int decimals) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

/// Sets LC_NUMERIC to a locale with ',' as the decimal separator; restores the previous one when
/// it goes out of scope. ok() is false when the machine has none of the candidates.
class CommaLocale {
public:
    CommaLocale() {
        const char* current = std::setlocale(LC_NUMERIC, nullptr);
        m_saved = current != nullptr ? current : "C";
        for (const char* name : {"de_DE.UTF-8", "German_Germany.1252", "de-DE"}) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr) {
                if (std::strcmp(std::localeconv()->decimal_point, ",") == 0) {
                    m_name = name;
                    return;
                }
            }
        }
        std::setlocale(LC_NUMERIC, m_saved.c_str());
    }
    ~CommaLocale() { std::setlocale(LC_NUMERIC, m_saved.c_str()); }
    CommaLocale(const CommaLocale&) = delete;
    CommaLocale& operator=(const CommaLocale&) = delete;
    bool ok() const { return !m_name.empty(); }
    const std::string& name() const { return m_name; }

private:
    std::string m_saved;
    std::string m_name;
};

const double kValues[] = {0.0,
                          -0.0,
                          0.5,
                          -0.25,
                          1.0,
                          1.5,
                          12.5,
                          100.0,
                          1234.5678,
                          0.001,
                          1e-5,
                          1.23e-7,
                          6.02e23,
                          -3.75e10,
                          1e15,
                          123456789.0,
                          0.1,
                          1.0 / 3.0,
                          2.0 / 3.0,
                          99999.95,
                          1e100,
                          1e-300,
                          3.14159265358979,
                          static_cast<double>(0.1f),
                          16777216.0};
const int kPrecisions[] = {1, 3, 4, 6, 9, 17};

const char* const kXtfx =
    "name: Wave\n"
    "param amplitude float 2.5 [0, 32]\n"
    "param speed float 0.75 [0, 10]\n"
    "---\n"
    "float4 xtMain(XtInput i)\n"
    "{\n"
    "    float2 uv = i.uv + sin(i.uv.yx * 40 + xtTime * speed) * (amplitude * i.texelSize);\n"
    "    return xtSample(uv);\n"
    "}\n";

const char* const kXt =
    "<xt><head><style>"
    "#a { position: absolute; left: 0px; top: 0px; width: 40.5px; height: 40px; opacity: 0.5;"
    " background-color: #f00; effect: custom(\"wave.xtfx\", amplitude 6.5, speed 1.25); }"
    "@timeline main from \"a.xtanim.json#main\";"
    "</style></head><body><camera id=\"cam\" zoom=\"1.5\"/><div id=\"a\"></div></body></xt>";

const char* const kXtanim =
    R"J({"v": 1, "timelines": [{"name": "main", "tracks": [
      {"layer": "a", "prop": "opacity", "keys": [[0, 0.5], [1.5, 0.25, "linear"]]}]}]})J";

xt::compile::CompileResult compileSample(const fs::path& dir) {
    std::vector<xt::compile::SourceFile> files{{"main.xt", kXt}, {"a.xtanim.json", kXtanim}};
    xt::compile::CompileOptions options;
    options.rootDir = dir.string();
    options.emitMeta = false;
    return xt::compile::compileDocumentFromStrings(files, "main.xt", options);
}

}  // namespace

TEST_CASE("T-268 xt::formatFloat matches printf(\"%.*g\") in the C locale") {
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current != nullptr ? current : "C";
    std::setlocale(LC_NUMERIC, "C");
    for (const double value : kValues) {
        for (const int precision : kPrecisions) {
            CAPTURE(value);
            CAPTURE(precision);
            CHECK(format(value, precision) == cFormat(value, precision));
            CHECK(format(-value, precision) == cFormat(-value, precision));
        }
    }
    CHECK(format(std::numeric_limits<double>::infinity(), 6) ==
          cFormat(std::numeric_limits<double>::infinity(), 6));
    std::setlocale(LC_NUMERIC, saved.c_str());

    // Too small a buffer: nothing written, 0 returned.
    char small[3] = {'x', 'x', 'x'};
    CHECK(xt::formatFloat(small, sizeof(small), 0.125, 6) == 0);
    CHECK(xt::formatFloat(small, sizeof(small), 0.5, 6) == 3);
    CHECK(std::string(small, 3) == "0.5");
}

TEST_CASE("T-268 xt::formatFixed matches printf(\"%.*f\") in the C locale") {
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = current != nullptr ? current : "C";
    std::setlocale(LC_NUMERIC, "C");
    const int kDecimals[] = {0, 2, 3, 4, 6};
    for (const double value : kValues) {
        for (const int decimals : kDecimals) {
            CAPTURE(value);
            CAPTURE(decimals);
            CHECK(fixed(value, decimals) == cFixed(value, decimals));
            CHECK(fixed(-value, decimals) == cFixed(-value, decimals));
        }
    }
    CHECK(fixed(-0.0001, 3) == "-0.000");
    std::setlocale(LC_NUMERIC, saved.c_str());

    // Too small a buffer: nothing written, 0 returned.
    char small[4] = {'x', 'x', 'x', 'x'};
    CHECK(xt::formatFixed(small, sizeof(small), 12.5, 3) == 0);
    CHECK(xt::formatFixed(small, sizeof(small), 0.5, 2) == 4);
    CHECK(std::string(small, 4) == "0.50");
}

TEST_CASE("T-268 a comma-decimal LC_NUMERIC changes neither formatFloat, fromChars nor the IR") {
    const fs::path dir = xt::test::tempDir() / "xt_t268_locale";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "wave.xtfx", std::ios::binary);
        out << kXtfx;
    }
    const xt::compile::CompileResult reference = compileSample(dir);
    for (const xt::compile::Diagnostic& d : reference.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != xt::compile::Diagnostic::Level::Error);
    }
    REQUIRE(reference.ok());

    CommaLocale comma;
    if (!comma.ok()) {
        MESSAGE(
            "no comma-decimal locale (de_DE.UTF-8, German_Germany.1252, de-DE) on this machine; "
            "the formatFloat / fromChars unit tests stand in");
        return;
    }
    MESSAGE("LC_NUMERIC set to " << comma.name());
    char probe[32];
    std::snprintf(probe, sizeof(probe), "%g", 0.5);
    CHECK(std::string(probe) == "0,5");  // The locale really is in effect for printf.

    for (const double value : kValues) {
        CAPTURE(value);
        std::string expected = cFormat(value, 9);  // The locale's printf, ',' put back to '.'.
        for (char& c : expected) {
            c = c == ',' ? '.' : c;
        }
        CHECK(format(value, 9) == expected);
        std::string expectedFixed = cFixed(value, 4);
        for (char& c : expectedFixed) {
            c = c == ',' ? '.' : c;
        }
        CHECK(fixed(value, 4) == expectedFixed);
    }
    double parsed = 0;
    const char* text = "0.5";
    CHECK(xt::fromChars(text, text + 3, parsed).ec == std::errc{});
    CHECK(parsed == 0.5);

    const xt::compile::CompileResult underComma = compileSample(dir);
    for (const xt::compile::Diagnostic& d : underComma.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != xt::compile::Diagnostic::Level::Error);
    }
    REQUIRE(underComma.ok());
    CHECK(underComma.diagnostics.size() == reference.diagnostics.size());
    CHECK(underComma.ir == reference.ir);
}
