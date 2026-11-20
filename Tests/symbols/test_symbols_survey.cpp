#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "doctest.h"
#include "golden_common.h"
#include "test_temp.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/sw/sw_png.h"

/// T-793: the SW state goldens of the survey-2026 preset group
/// (`Assets/Symbols/groups/survey-2026`), the same shape as the Broadsheet case in
/// test_symbols.cpp: the 30 `normal` previews, the four `defiance` previews
/// (`previews/defiance/`), and the other states listed in Tests/symbols/survey/manifest.txt with
/// their event scripts. XT_SYMBOLS_UPDATE=1 rewrites Tests/symbols/survey/expected/.

using namespace xt;

namespace {

namespace fs = std::filesystem;

bool surveyUpdateGoldens() {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, "XT_SYMBOLS_UPDATE") != 0 || v == nullptr) {
        return false;
    }
    const bool on = v[0] == '1';
    std::free(v);
    return on;
#else
    const char* v = std::getenv("XT_SYMBOLS_UPDATE");
    return v != nullptr && v[0] == '1';
#endif
}

const Color kSurveyClear{5 / 255.0f, 5 / 255.0f, 5 / 255.0f, 1.0f};

/// The `normal` (and, with `defiance`, the four defiance-token) fixtures from the group's previews.
std::vector<golden::UiFixture> surveyPreviewFixtures(bool defiance) {
    std::vector<golden::UiFixture> out;
    const fs::path scripts = xt::test::tempDir() / "xt_symbols_survey_previews";
    std::error_code ec;
    fs::create_directories(scripts, ec);
    const std::vector<std::string> defianceNames{"MenuItem", "Panel", "VitalBar", "ConfirmDialog"};
    for (const bundle::PreviewSpec& spec : bundle::thumbnailSpecs(XT_SURVEY_DIR)) {
        if (defiance &&
            std::find(defianceNames.begin(), defianceNames.end(), spec.name) == defianceNames.end()) {
            continue;
        }
        const fs::path script = scripts / (spec.name + (defiance ? ".defiance.events" : ".events"));
        {
            std::ofstream file(script, std::ios::binary);
            file << bundle::previewEventScript(spec);
        }
        golden::UiFixture fx;
        fx.name = "survey_" + spec.name + (defiance ? "_defiance" : "_normal");
        fx.xtPath = (fs::path(XT_SURVEY_DIR) / (defiance ? "previews/defiance" : "previews") /
                     (spec.name + ".preview.xt"))
                        .string();
        fx.width = spec.width;
        fx.height = spec.height;
        fx.time = 0;
        fx.clearColor = kSurveyClear;
        fx.eventsPath = script.string();
        out.push_back(std::move(fx));
    }
    return out;
}

}  // namespace

TEST_CASE("survey symbols: every state matches its golden and differs from the normal state") {
    const std::string dir = std::string(XT_SYMBOLS_TEST_DIR) + "/survey";
    std::vector<golden::UiFixture> fixtures = surveyPreviewFixtures(false);
    const std::vector<golden::UiFixture> defiances = surveyPreviewFixtures(true);
    const std::vector<golden::UiFixture> listed = golden::loadUiManifest(dir + "/manifest.txt");
    REQUIRE(golden::manifestErrors() == 0);  // No fixture dropped by a bad manifest line.
    CHECK(defiances.size() == 4);
    const size_t listedCount = listed.size();
    fixtures.insert(fixtures.end(), listed.begin(), listed.end());
    fixtures.insert(fixtures.end(), defiances.begin(), defiances.end());
    CHECK(fixtures.size() == 30 + listedCount + 4);
    CHECK(listedCount >= 14);
    const bool update = surveyUpdateGoldens();
    std::map<std::string, std::vector<uint8_t>> normal;
    for (golden::UiFixture fx : fixtures) {
        fx.assetRoots = {XT_SURVEY_DIR};
        CAPTURE(fx.name);
        const std::vector<uint8_t> actual = golden::renderUiFixture(fx, dir);
        REQUIRE(!actual.empty());
        const std::string symbol = fx.name.substr(7, fx.name.rfind('_') - 7);  // survey_<Name>_<state>
        const fs::path expectedPath = fs::path(dir) / "expected" / (fx.name + ".png");
        if (update) {
            fs::create_directories(expectedPath.parent_path());
            CHECK(sw::writePng(expectedPath.string(), actual.data(), fx.width, fx.height, fx.width * 4));
        } else {
            std::vector<uint8_t> expected;
            int width = 0;
            int height = 0;
            REQUIRE_MESSAGE(sw::readPng(expectedPath.string(), expected, width, height),
                            "missing golden (run with XT_SYMBOLS_UPDATE=1)");
            REQUIRE(width == fx.width);
            REQUIRE(height == fx.height);
            // SW against an SW golden: no pixel may differ by more than 2/255 (XT_GOLDEN_CHANNEL /
            // XT_GOLDEN_RATIO override that for one run).
            const golden::SwTolerance tolerance = golden::swTolerance();
            const golden::GoldenResult result = golden::compareRgba(
                actual.data(), expected.data(), fx.width, fx.height, tolerance.channel, tolerance.ratio);
            if (result.pass && golden::goldenVerbose() && result.maxChannelDiff > 0) {
                MESSAGE(golden::describeDiff("[DIFF]", fx.name, result, tolerance.ratio, tolerance.channel));
            }
            if (!result.pass) {
                const fs::path out(XT_SYMBOLS_OUT_DIR);
                fs::create_directories(out);
                sw::writePng((out / (fx.name + ".actual.png")).string(), actual.data(), fx.width, fx.height,
                             fx.width * 4);
                golden::writeDiffPng((out / (fx.name + ".diff.png")).string(), actual.data(), expected.data(),
                                     fx.width, fx.height, tolerance.channel);
            }
            CHECK_MESSAGE(result.pass, golden::describeDiff("[FAIL]", fx.name, result, tolerance.ratio,
                                                            tolerance.channel));
        }
        if (fx.name.size() > 7 && fx.name.compare(fx.name.size() - 7, 7, "_normal") == 0) {
            normal[symbol] = actual;
        } else {
            REQUIRE(normal.count(symbol) == 1);  // The previews' `normal` states come first.
            const golden::GoldenResult same =
                golden::compareRgba(actual.data(), normal[symbol].data(), fx.width, fx.height, 2, 0.0f);
            CHECK_MESSAGE(!same.pass, "state renders exactly like the normal state");
        }
    }
}
