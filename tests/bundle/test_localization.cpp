#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/text_shaper.h"

/// T-179: `strings/<lang>.csv` tables, BundleLocalizationProvider, pack()'s `--lang` and `i18n`
/// lint, the manifest's `strings`/`fonts.fallback`, and languageCoverage(). See
/// docs/design/22-bundle.md §22.10, 12-compiler.md §12.7, 13-api.md §13.8.2, docs/tasks/T-179.md.

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;

/// A fresh, writable copy of the `sample` source bundle, with `@key` text in the pause menu, a
/// `@font-fallback` in the shared stylesheet and two strings tables.
std::string localizedSample(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_l10n_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample", dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    {
        std::ofstream pause(dir / "menus" / "pause.xt", std::ios::trunc);
        pause << "<xt><body><div id=\"pausePanel\"><span id=\"title\">@pause_title</span>"
                 "<span id=\"resume\">@pause_resume</span><img src=\"img/a.png\" alt=\"@pause_img\"/>"
                 "</div></body></xt>\n";
        std::ofstream base(dir / "shared" / "base.xts", std::ios::app);
        base << "\n@font-fallback zh-TW { font-family: \"Noto Sans SC\", Fallback Two; }\n";
        fs::create_directories(dir / "strings");
        std::ofstream en(dir / "strings" / "en.csv", std::ios::binary);
        en << "key,text\r\npause_title,Paused\r\npause_resume,\"Resume, now\"\r\npause_img,Image\r\n"
              "old_key,Old\r\n";
        std::ofstream zh(dir / "strings" / "zh-TW.csv", std::ios::binary);
        zh << "pause_title,\xE6\x9A\xAB\xE5\x81\x9C\npause_resume,\n";  // 暫停; resume left empty.
    }
    return dir.generic_string();
}

std::string outDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_l10n_out_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir.generic_string();
}

ProjectFile loadProject(const std::string& dir) {
    std::string error;
    Result<ProjectFile> project = ProjectFile::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return std::move(project.value());
}

bool hasWarning(const PackReport& report, const std::string& file, const std::string& needle) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                       [&](const compile::Diagnostic& d) {
                           return d.level == compile::Diagnostic::Level::Warning && d.file == file &&
                                  d.message.find(needle) != std::string::npos;
                       });
}

}  // namespace

TEST_CASE("T-179: parseStringsCsv follows RFC 4180") {
    std::vector<StringsRow> rows;
    std::string error;
    const std::string text =
        "\xEF\xBB\xBFkey,text\r\n"
        "a,plain\r\n"
        "\r\n"
        "b,\"with, comma\"\n"
        "c,\"say \"\"hi\"\"\"\n"
        "d,\"two\nlines\"\n"
        "e,\n"
        "f,last";
    REQUIRE_MESSAGE(parseStringsCsv(text, rows, &error), error);
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].key == "a");
    CHECK(rows[0].text == "plain");
    CHECK(rows[0].line == 2);
    CHECK(rows[1].text == "with, comma");
    CHECK(rows[2].text == "say \"hi\"");
    CHECK(rows[3].text == "two\nlines");
    CHECK(rows[4].text.empty());
    CHECK(rows[5].key == "f");
    CHECK(rows[5].text == "last");
    CHECK(rows[5].line == 9);

    std::vector<StringsRow> bad;
    CHECK_FALSE(parseStringsCsv("a,b,c\n", bad, &error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_FALSE(parseStringsCsv("a,ok\nb\n", bad, &error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(parseStringsCsv("a,\"open\n", bad, &error));
    CHECK_FALSE(parseStringsCsv("a,\"x\"y\n", bad, &error));
    CHECK_FALSE(parseStringsCsv("a,x\"y\n", bad, &error));
}

TEST_CASE("T-179: BundleLocalizationProvider falls back along the BCP 47 prefix") {
    BundleLocalizationProvider provider;
    REQUIRE(provider.addCsv("zh", "hp,HP-zh\nmp,MP-zh\n") == Status::kOk);
    REQUIRE(provider.addCsv("zh-TW", "hp,HP-tw\nmp,\n") == Status::kOk);
    provider.set("en", "hp", "HP");
    CHECK(provider.resolve("hp", "zh-TW") == "HP-tw");
    CHECK(provider.resolve("mp", "zh-TW") == "MP-zh");  // Empty in zh-TW -> zh.
    CHECK(provider.resolve("hp", "zh-TW-x") == "HP-tw");
    CHECK(provider.resolve("hp", "en") == "HP");
    CHECK(provider.resolve("mp", "en").empty());
    CHECK(provider.resolve("hp", "fr").empty());
    CHECK(provider.languages() == std::vector<std::string>{"en", "zh", "zh-TW"});
    std::string error;
    CHECK(provider.addCsv("de", "a,b,c\n", &error) == Status::kInvalidArgument);
    CHECK(provider.loadFile("de", "does/not/exist.csv", &error) == Status::kIoError);
}

TEST_CASE("T-179: pack copies strings tables, lints them and records fallback chains") {
    const std::string source = localizedSample("full");
    const std::string out = outDir("full");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    CHECK(report.ok);
    // zh-TW lacks pause_img and leaves pause_resume empty; en carries a key no UI uses.
    CHECK(hasWarning(report, "strings/zh-TW.csv", "untranslated key 'pause_img'"));
    CHECK(hasWarning(report, "strings/zh-TW.csv", "untranslated key 'pause_resume'"));
    CHECK_FALSE(hasWarning(report, "strings/zh-TW.csv", "untranslated key 'pause_title'"));
    CHECK(hasWarning(report, "strings/en.csv", "unused key 'old_key'"));
    CHECK_FALSE(hasWarning(report, "strings/en.csv", "untranslated"));

    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    Bundle& bundle = *opened.value();
    REQUIRE(bundle.strings().size() == 2);
    CHECK(bundle.strings()[0].lang == "en");
    CHECK(bundle.strings()[0].file == "strings/en.csv");
    CHECK(bundle.strings()[1].lang == "zh-TW");
    CHECK(fs::exists(out + "/strings/en.csv"));
    CHECK(fs::exists(out + "/strings/zh-TW.csv"));
    REQUIRE(bundle.fontFallbacks().size() == 1);
    CHECK(bundle.fontFallbacks()[0].lang == "zh-TW");
    CHECK(bundle.fontFallbacks()[0].families == std::vector<std::string>{"Noto Sans SC", "Fallback Two"});

    const std::vector<LanguageCoverage> coverage = languageCoverage(bundle);
    REQUIRE(coverage.size() == 2);
    CHECK(coverage[0].lang == "en");
    CHECK(coverage[0].total == 3);
    CHECK(coverage[0].translated == 3);
    CHECK(coverage[1].lang == "zh-TW");
    CHECK(coverage[1].translated == 1);

    // The provider serves the packed tables to a loaded UI; switching language re-resolves.
    BundleLocalizationProvider provider;
    REQUIRE(provider.load(bundle, &error) == Status::kOk);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &bundle.assets()});
    ctx.setLocalizationProvider(&provider);
    REQUIRE(ctx.setLanguage("en") == Status::kOk);
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "pause");
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    CHECK(doc->byId("title").text() == "Paused");
    CHECK(doc->byId("resume").text() == "Resume, now");
    REQUIRE(ctx.setLanguage("zh-TW") == Status::kOk);
    CHECK(doc->byId("title").text() == "\xE6\x9A\xAB\xE5\x81\x9C");
    CHECK(doc->byId("resume").text() == "pause_resume");  // Empty in zh-TW: the key shows.
}

TEST_CASE("T-179: pack --lang packs only the listed languages, and --prune drops the others") {
    const std::string source = localizedSample("lang");
    const std::string out = outDir("lang");
    PackOptions options;
    options.outDir = out;
    options.thumbnails = false;
    REQUIRE(pack(loadProject(source), options).ok);
    CHECK(fs::exists(out + "/strings/en.csv"));

    options.languages = {"zh-TW", "fr"};
    options.prune = true;
    const PackReport report = pack(loadProject(source), options);
    CHECK(report.ok);
    CHECK(hasWarning(report, "strings/fr.csv", "has no strings table"));
    CHECK_FALSE(fs::exists(out + "/strings/en.csv"));
    CHECK(fs::exists(out + "/strings/zh-TW.csv"));
    std::string error;
    Result<std::unique_ptr<Bundle>> opened = Bundle::open(out, nullptr, &error);
    REQUIRE_MESSAGE(opened.ok(), error);
    REQUIRE(opened.value()->strings().size() == 1);
    CHECK(opened.value()->strings()[0].lang == "zh-TW");
}

TEST_CASE("T-179: a malformed strings table fails the pack") {
    const std::string source = localizedSample("bad");
    {
        std::ofstream bad(source + "/strings/de.csv", std::ios::binary);
        bad << "a,b,c\n";
    }
    PackOptions options;
    options.outDir = outDir("bad");
    options.thumbnails = false;
    const PackReport report = pack(loadProject(source), options);
    CHECK_FALSE(report.ok);
    CHECK(std::any_of(report.diagnostics.begin(), report.diagnostics.end(), [](const compile::Diagnostic& d) {
        return d.level == compile::Diagnostic::Level::Error && d.file == "strings/de.csv";
    }));
}
