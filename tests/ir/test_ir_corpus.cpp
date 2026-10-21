// T-267c (docs/design/15 §15.2): corpus-wide IR round trip. Every fixture is compiled, opened with
// IrView::open(), validated, re-serialized through IrView's public readers and IrWriter's public
// interface (tests/ir/ir_reserialize.cpp) and compared byte for byte with the compiled blob.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "ir_reserialize.h"
#include "xt/compile/compile.h"
#include "xt/ir.h"

namespace fs = std::filesystem;

namespace {

/// A fixture whose round trip is known to differ, with the reason (path relative to tests/, with
/// forward slashes). Such a fixture is reported but does not fail the suite.
struct KnownAsymmetric {
    const char* path;
    const char* reason;
};

// Empty: every covered fixture round-trips (T-267c). Add `{"path", "reason"}` entries here.
constexpr std::array<KnownAsymmetric, 0> kKnownAsymmetric{};

const char* knownAsymmetricReason(const std::string& rel) {
    for (const KnownAsymmetric& k : kKnownAsymmetric) {
        if (rel == k.path) {
            return k.reason;
        }
    }
    return nullptr;
}

fs::path testsRoot() { return fs::path(XT_TEST_XT_DIR).parent_path(); }

std::string relName(const fs::path& p) { return p.lexically_relative(testsRoot()).generic_string(); }

void collect(const fs::path& dir, std::string_view ext, bool recursive, std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return;
    }
    if (recursive) {
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().extension() == ext) {
                out.push_back(e.path());
            }
        }
    } else {
        for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().extension() == ext) {
                out.push_back(e.path());
            }
        }
    }
}

/// tests/xt/*.xt, every .xt under tests/golden/ui/ and under any tests/<suite>/fixtures/ (there is
/// no top-level tests/fixtures/), tests/lottie/*.json and tests/golden/lottie/*.json.
std::vector<fs::path> corpus() {
    const fs::path root = testsRoot();
    std::vector<fs::path> files;
    collect(root / "xt", ".xt", false, files);
    collect(root / "golden" / "ui", ".xt", true, files);
    for (const fs::directory_entry& e : fs::directory_iterator(root)) {
        if (e.is_directory()) {
            collect(e.path() / "fixtures", ".xt", true, files);
        }
    }
    collect(root / "lottie", ".json", false, files);
    collect(root / "golden" / "lottie", ".json", false, files);
    std::sort(files.begin(), files.end());
    return files;
}

xt::compile::CompileResult compileFixture(const fs::path& path, bool emitMeta) {
    xt::compile::CompileOptions options;
    options.rootDir = path.parent_path().string();
    options.emitMeta = emitMeta;
    if (path.extension() == ".json") {
        return xt::compile::compileLottie(path.string(), options);
    }
    return xt::compile::compileDocument(path.string(), options);
}

size_t firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return n;
}

/// Counts each chunk tag of the well-formed blob `bytes` into `tally`.
void tallyChunks(const std::vector<uint8_t>& bytes, std::map<std::string, uint32_t>& tally) {
    size_t pos = sizeof(xt::IrHeader);
    while (pos + sizeof(xt::IrChunkHeader) <= bytes.size()) {
        xt::IrChunkHeader h{};
        std::memcpy(&h, bytes.data() + pos, sizeof(h));
        ++tally[std::string(h.type, sizeof(h.type))];
        pos += sizeof(xt::IrChunkHeader) + h.payloadLength;
    }
}

}  // namespace

TEST_CASE("ir corpus: compile, read through IrView, rewrite, compare bytes") {
    const std::vector<fs::path> files = corpus();
    uint32_t covered = 0;
    std::vector<std::string> skipped;
    std::map<std::string, uint32_t> tally;
    for (const fs::path& path : files) {
        const std::string rel = relName(path);
        xt::compile::CompileResult result = compileFixture(path, true);
        if (!result.ok()) {
            skipped.push_back(rel);  // An error fixture (e.g. a symbol library or a negative case).
            continue;
        }
        ++covered;
        tallyChunks(result.ir, tally);
        xt::IrBlob blob{result.ir.data(), result.ir.size()};
        xt::Result<xt::IrView> view = xt::IrView::open(blob);
        if (!view.ok()) {
            FAIL_CHECK(rel, ": IrView::open failed");
            continue;
        }
        if (xt::IrView::validate(view.value()) != xt::Status::kOk) {
            FAIL_CHECK(rel, ": IrView::validate failed");
            continue;
        }
        const std::vector<uint8_t> again = xt::test::reserialize(view.value());
        const char* known = knownAsymmetricReason(rel);
        if (again == result.ir) {
            if (known != nullptr) {
                FAIL_CHECK(rel, ": listed in kKnownAsymmetric but now round-trips; remove the entry");
            }
            continue;
        }
        const size_t at = firstDifference(result.ir, again);
        const std::string tag = xt::test::chunkTagAt(result.ir, at);
        if (known != nullptr) {
            MESSAGE(rel, ": known asymmetry at offset ", at, " (chunk ", tag, "): ", known);
            continue;
        }
        // Writer nondeterminism (compile twice, no META timestamp) versus a reader/writer asymmetry.
        const std::vector<uint8_t> a = compileFixture(path, false).ir;
        const std::vector<uint8_t> b = compileFixture(path, false).ir;
        FAIL_CHECK(rel, ": round trip differs at offset ", at, " (chunk ", tag, "), sizes ", result.ir.size(),
                   " vs ", again.size(),
                   std::string(a == b ? "; reader/writer asymmetry" : "; writer is nondeterministic"));
    }
    std::string skippedList;
    for (const std::string& s : skipped) {
        skippedList += " " + s;
    }
    std::string tallyList;
    for (const auto& [tag, count] : tally) {
        tallyList += " " + tag + "=" + std::to_string(count);
    }
    MESSAGE("ir corpus: ", covered, " fixture(s) round-tripped; chunks:", tallyList, "; ", skipped.size(),
            " skipped (compile errors):", skippedList);
    CHECK(covered >= 150);
}

TEST_CASE("ir corpus: chunkTagAt locates chunks") {
    xt::compile::CompileResult result =
        compileFixture(testsRoot() / "lottie" / "shape_rect_fill.json", false);
    REQUIRE(result.ok());
    CHECK(xt::test::chunkTagAt(result.ir, 0) == "header");
    CHECK(xt::test::chunkTagAt(result.ir, sizeof(xt::IrHeader)) == "STRT");
    CHECK(xt::test::chunkTagAt(result.ir, result.ir.size()) == "?");
}
