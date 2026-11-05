// T-617 (Docs/design/50-c-abi.md §50.x localization): xt_bundle_open loads a strings bundle's
// tables, a surface starts in `spec.defaultLanguage` (else the first language) with its `@key` text
// resolved, and xt_surface_set_language re-resolves and posts `languageChanged` to the UI flow.
// XT_CAPI_TEST_L10N_BUNDLE is Tests/capi/fixtures/l10n, packed at build time: #box is red while
// `@greeting` is unresolved, green for "Hello" (en) and blue for "Nihao" (zh-TW), through
// `localize` class bindings; the flow writes `system.languageChanged` into the `heard` port.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "../common/test_temp.h"
#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

namespace fs = std::filesystem;

xt_surface_desc l10nDesc() {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = 32;
    desc.height = 32;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 1;
    return desc;
}

enum class BoxColor { Red, Green, Blue, Other };

/// One RGBA8 buffer per surface, kept across frames (a frame without damage draws nothing).
using Frame = std::vector<uint8_t>;
Frame newFrame() { return Frame(32u * 32u * 4u, 0); }

/// Updates and renders `s` into `rgba` and names the color at the center of #box.
BoxColor boxColor(xt_surface* s, Frame& rgba) {
    xt_damage damage{};
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(xt_surface_render(s, rgba.data(), 32 * 4, &damage) == XT_STATUS_OK);
    const size_t center = (16u * 32u + 16u) * 4u;
    const int r = rgba[center];
    const int g = rgba[center + 1];
    const int b = rgba[center + 2];
    INFO("rgba " << r << " " << g << " " << b << " " << int(rgba[center + 3]));
    REQUIRE(rgba[center + 3] > 200);
    if (r > 200 && g < 50 && b < 50) {
        return BoxColor::Red;
    }
    if (g > 200 && r < 50 && b < 50) {
        return BoxColor::Green;
    }
    if (b > 200 && r < 50 && g < 50) {
        return BoxColor::Blue;
    }
    return BoxColor::Other;
}

/// The `heard` port's text (what the flow's system node last wrote into it).
std::string heard(xt_surface* s) {
    const uint32_t port = xt_surface_port_lookup(s, "heard");
    REQUIRE(port != XT_INVALID_PORT);
    const char* text = xt_surface_port_get_text(s, port);
    return text != nullptr ? text : "";
}

/// Copies the packed fixture into a fresh directory under the test temp root, lets `edit` change
/// its manifest, and returns the copy's path.
template <class F>
std::string editedCopy(const std::string& name, F&& edit) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(fs::path(XT_CAPI_TEST_L10N_BUNDLE), dir, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, ec.message());
    const fs::path manifestPath = dir / "manifest.json";
    std::stringstream text;
    {
        std::ifstream in(manifestPath, std::ios::binary);
        text << in.rdbuf();
    }
    nlohmann::json manifest = nlohmann::json::parse(text.str());
    edit(manifest);
    {
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        out << manifest.dump(2);
    }
    return dir.string();
}

}  // namespace

TEST_CASE("capi 16 (T-617): @key resolves on create; set_language re-resolves and posts languageChanged") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_L10N_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = l10nDesc();
    xt_surface* s = xt_surface_create(b, "l10n", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    Frame frame = newFrame();
    // No spec.defaultLanguage: the first strings language, "en".
    CHECK(boxColor(s, frame) == BoxColor::Green);
    CHECK(heard(s).empty());

    REQUIRE(xt_surface_set_language(s, "zh-TW") == XT_STATUS_OK);
    CHECK(heard(s) == "zh-TW");
    CHECK(boxColor(s, frame) == BoxColor::Blue);

    // A region tag falls back to its language's table; the event carries the tag as given.
    REQUIRE(xt_surface_set_language(s, "en-GB") == XT_STATUS_OK);
    CHECK(heard(s) == "en-GB");
    CHECK(boxColor(s, frame) == BoxColor::Green);

    // A bad tag changes nothing and posts nothing.
    CHECK(xt_surface_set_language(s, "not a tag") == XT_STATUS_INVALID_ARGUMENT);
    CHECK(heard(s) == "en-GB");
    CHECK(boxColor(s, frame) == BoxColor::Green);

    // A second surface of the same bundle starts in the default language on its own Context.
    xt_surface* other = xt_surface_create(b, "l10n", &desc);
    REQUIRE_MESSAGE(other != nullptr, xt_capi_last_error());
    Frame otherFrame = newFrame();
    REQUIRE(xt_surface_set_language(s, "zh-TW") == XT_STATUS_OK);
    CHECK(boxColor(other, otherFrame) == BoxColor::Green);
    CHECK(heard(other).empty());
    CHECK(boxColor(s, frame) == BoxColor::Blue);

    // Hot reload keeps the surface's language.
    REQUIRE(xt_surface_hot_reload(s) == XT_STATUS_OK);
    CHECK(boxColor(s, frame) == BoxColor::Blue);

    xt_surface_destroy(other);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 17 (T-617): spec.defaultLanguage picks the language a surface starts in") {
    const std::string dir = editedCopy(
        "capi-l10n-default", [](nlohmann::json& manifest) { manifest["spec"]["defaultLanguage"] = "zh-TW"; });
    xt_bundle* b = xt_bundle_open(dir.c_str());
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = l10nDesc();
    xt_surface* s = xt_surface_create(b, "l10n", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    Frame frame = newFrame();
    CHECK(boxColor(s, frame) == BoxColor::Blue);
    CHECK(heard(s).empty());  // The starting language is not an event.
    REQUIRE(xt_surface_set_language(s, "en") == XT_STATUS_OK);
    CHECK(heard(s) == "en");
    CHECK(boxColor(s, frame) == BoxColor::Green);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 18 (T-617): a bundle without strings: keys stay unresolved and set_language posts nothing") {
    const std::string dir =
        editedCopy("capi-l10n-none", [](nlohmann::json& manifest) { manifest.erase("strings"); });
    xt_bundle* b = xt_bundle_open(dir.c_str());
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = l10nDesc();
    xt_surface* s = xt_surface_create(b, "l10n", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    Frame frame = newFrame();
    CHECK(boxColor(s, frame) == BoxColor::Red);
    REQUIRE(xt_surface_set_language(s, "zh-TW") == XT_STATUS_OK);
    CHECK(heard(s).empty());
    CHECK(boxColor(s, frame) == BoxColor::Red);
    CHECK(xt_surface_set_language(s, "not a tag") == XT_STATUS_INVALID_ARGUMENT);
    // The host's own post still reaches the flow, as before T-617.
    REQUIRE(xt_surface_post_system_event(s, "languageChanged", "zh-TW") == XT_STATUS_OK);
    CHECK(heard(s) == "zh-TW");
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}
