// T-807 (Docs/design/22-bundle.md, "`.xtb` unpack cache cap"): Bundle::open's unpack cache
// `<temp>/xtframe-bundle/<content hash>/` -- a use refreshes the copy, every new copy is followed
// by an eviction down to XTFRAME_BUNDLE_CACHE_MAX_ENTRIES / XTFRAME_BUNDLE_CACHE_MAX_MB (8 / 256
// by default, 0 = no cap), least recently used first, never the copy in use, and a copy without
// the completion marker is never reused.
//
// Every open runs in a child process (xt_xtb_cache_probe) whose TMP / TEMP / TMPDIR name a
// directory under this test's own temp root: the copies one process used are kept from its own
// eviction, and the machine's real temp directory is never touched.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/zip_archive.h"

using namespace xt;
namespace fs = std::filesystem;

namespace {

constexpr const char* kMaxEntriesT807 = "XTFRAME_BUNDLE_CACHE_MAX_ENTRIES";
constexpr const char* kMaxMbT807 = "XTFRAME_BUNDLE_CACHE_MAX_MB";

std::string readT807(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeT807(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// One test's directories: `tmp` (the child's temp directory) and the `.xtb` files beside it.
struct CacheFixtureT807 {
    fs::path root;
    fs::path temp;
    fs::path packed;

    fs::path cache() const { return temp / "xtframe-bundle"; }
};

/// A fresh fixture with the sample project packed once into `packed`.
CacheFixtureT807 fixtureT807(const std::string& name) {
    CacheFixtureT807 f;
    f.root = xt::test::tempDir() / ("xt_t807_" + name);
    f.temp = f.root / "tmp";
    f.packed = f.root / "packed";
    std::error_code ec;
    fs::remove_all(f.root, ec);
    fs::create_directories(f.temp, ec);
    const fs::path source = f.root / "src";
    fs::copy(std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample", source, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string error;
    Result<bundle::ProjectFile> project = bundle::ProjectFile::load(source.generic_string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    bundle::PackOptions options;
    options.outDir = f.packed.generic_string();
    options.thumbnails = false;
    REQUIRE(bundle::pack(project.value(), options).ok);
    return f;
}

/// `<root>/<name>.xtb`: the packed sample plus `pad.bin` (`padBytes` bytes derived from `name`,
/// so every name has its own content hash).
fs::path xtbT807(const CacheFixtureT807& f, const std::string& name, size_t padBytes = 64) {
    const fs::path dir = f.root / ("dir_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(f.packed, dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string pad;
    pad.reserve(padBytes);
    for (size_t i = 0; pad.size() < padBytes; ++i) {
        pad += name;
        pad += static_cast<char>('a' + (i * 7 + name.size()) % 26);
    }
    pad.resize(padBytes);
    writeT807(dir / "pad.bin", pad);
    const fs::path archive = f.root / (name + ".xtb");
    std::string error;
    REQUIRE_MESSAGE(bundle::zipDirectory(dir, archive, {}, &error) == Status::kOk, error);
    fs::remove_all(dir, ec);
    return archive;
}

struct ProbeT807 {
    int exitCode = -1;
    bool ok = false;
    std::string entry;  // the unpacked directory's name under xtframe-bundle/
    std::string id;
    std::string text;
};

std::string quoteT807(const fs::path& path) { return "\"" + fs::path(path).make_preferred().string() + "\""; }

/// Opens `xtb` in xt_xtb_cache_probe with the fixture's temp directory and `env` (NAME=VALUE).
/// Waits a little first, so consecutive uses get distinct file times on every file system.
ProbeT807 probeT807(const CacheFixtureT807& f, const fs::path& xtb,
                    const std::vector<std::string>& env = {}) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const fs::path result = f.root / "probe.txt";
    std::error_code ec;
    fs::remove(result, ec);
    std::string command = quoteT807(fs::path(XT_TEST_XTB_PROBE)) + " " + quoteT807(f.temp) + " " +
                          quoteT807(xtb) + " " + quoteT807(result);
    for (const std::string& pair : env) {
        command += " " + pair;
    }
#if defined(_WIN32)
    command = "\"" + command + "\"";  // cmd /c strips one pair of outer quotes
#endif
    ProbeT807 probe;
    probe.exitCode = std::system(command.c_str());
    probe.text = readT807(result);
    std::istringstream lines(probe.text);
    std::string status;
    std::string dir;
    std::getline(lines, status);
    std::getline(lines, dir);
    std::getline(lines, probe.id);
    probe.ok = probe.exitCode == 0 && status == "ok";
    probe.entry = fs::path(dir).filename().string();
    return probe;
}

#define REQUIRE_PROBE(probe) REQUIRE_MESSAGE((probe).ok, "exit ", (probe).exitCode, ": ", (probe).text)

std::set<std::string> entriesT807(const CacheFixtureT807& f) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(f.cache(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory()) {
            out.insert(it->path().filename().string());
        }
    }
    return out;
}

uint64_t bytesT807(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) {
            total += it->file_size();
        }
    }
    return total;
}

std::string capT807(const char* name, const std::string& value) { return std::string(name) + "=" + value; }

/// T-808: sets the modification time of `dir` and of everything under it to one hour ago.
void ageT808(const fs::path& dir) {
    const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        fs::last_write_time(it->path(), old, fileEc);
        REQUIRE(fileEc.value() == 0);
    }
    fs::last_write_time(dir, old, ec);
    REQUIRE(ec.value() == 0);
}

}  // namespace

TEST_CASE("T-807 .xtb cache: ten different archives under a cap of 3 leave the three used last") {
    const CacheFixtureT807 f = fixtureT807("count");
    std::vector<std::string> used;
    for (int i = 0; i < 10; ++i) {
        const ProbeT807 probe =
            probeT807(f, xtbT807(f, "n" + std::to_string(i)), {capT807(kMaxEntriesT807, "3")});
        REQUIRE_PROBE(probe);
        used.push_back(probe.entry);
    }
    CHECK(entriesT807(f) == std::set<std::string>(used.end() - 3, used.end()));
}

TEST_CASE("T-807 .xtb cache: the size cap holds, and the copy in use stays even when it alone is over") {
    const CacheFixtureT807 f = fixtureT807("size");
    const std::vector<std::string> mb1 = {capT807(kMaxMbT807, "1")};
    std::string last;
    for (int i = 0; i < 4; ++i) {
        const ProbeT807 probe = probeT807(f, xtbT807(f, "s" + std::to_string(i), 400 * 1024), mb1);
        REQUIRE_PROBE(probe);
        last = probe.entry;
        CHECK(bytesT807(f.cache()) <= 1024u * 1024u);
        CHECK(entriesT807(f).count(last) == 1);
    }
    CHECK(entriesT807(f).size() < 4);  // four copies of over 400 KB each do not fit in 1 MB

    const ProbeT807 big = probeT807(f, xtbT807(f, "big", 1536 * 1024), mb1);
    REQUIRE_PROBE(big);
    CHECK(entriesT807(f) == std::set<std::string>{big.entry});
    CHECK(fs::is_regular_file(f.cache() / big.entry / "pad.bin"));
}

TEST_CASE("T-807 .xtb cache: a reuse refreshes the copy") {
    const CacheFixtureT807 f = fixtureT807("reuse");
    const std::vector<std::string> cap3 = {capT807(kMaxEntriesT807, "3")};
    const fs::path a = xtbT807(f, "a");
    const ProbeT807 pa = probeT807(f, a, cap3);
    const ProbeT807 pb = probeT807(f, xtbT807(f, "b"), cap3);
    const ProbeT807 pc = probeT807(f, xtbT807(f, "c"), cap3);
    REQUIRE_PROBE(pa);
    REQUIRE_PROBE(pb);
    REQUIRE_PROBE(pc);
    REQUIRE(entriesT807(f) == std::set<std::string>{pa.entry, pb.entry, pc.entry});

    const ProbeT807 again = probeT807(f, a, cap3);  // reused: nothing is evicted here
    REQUIRE_PROBE(again);
    CHECK(again.entry == pa.entry);
    CHECK(entriesT807(f).size() == 3);

    const ProbeT807 pd = probeT807(f, xtbT807(f, "d"), {capT807(kMaxEntriesT807, "2")});
    REQUIRE_PROBE(pd);
    CHECK(entriesT807(f) == std::set<std::string>{pa.entry, pd.entry});
}

TEST_CASE("T-807 .xtb cache: a copy without the completion marker is not reused and goes first") {
    SUBCASE("a half-unpacked copy is unpacked again") {
        const CacheFixtureT807 f = fixtureT807("half");
        const fs::path x = xtbT807(f, "x");
        const ProbeT807 first = probeT807(f, x);
        REQUIRE_PROBE(first);
        const fs::path copy = f.cache() / first.entry;
        std::error_code ec;
        fs::remove(copy / ".xtframe-unpacked", ec);
        writeT807(copy / "manifest.json", "{");  // as if the unpack stopped half way
        const ProbeT807 second = probeT807(f, x);
        REQUIRE_PROBE(second);
        CHECK(second.entry == first.entry);
        CHECK(second.id == first.id);
        CHECK(readT807(copy / "manifest.json") == readT807(f.packed / "manifest.json"));
    }
    SUBCASE("a directory without the marker is evicted before older complete copies") {
        const CacheFixtureT807 f = fixtureT807("unmarked");
        const std::vector<std::string> cap3 = {capT807(kMaxEntriesT807, "3")};
        const ProbeT807 pa = probeT807(f, xtbT807(f, "a"), cap3);
        const ProbeT807 pb = probeT807(f, xtbT807(f, "b"), cap3);
        REQUIRE_PROBE(pa);
        REQUIRE_PROBE(pb);
        writeT807(f.cache() / "0000000000000000" / "manifest.json", "{}");  // newest, but unmarked
        ageT808(f.cache() / "0000000000000000");  // T-808: and not being unpacked right now
        REQUIRE(entriesT807(f).size() == 3);
        const ProbeT807 pc = probeT807(f, xtbT807(f, "c"), cap3);
        REQUIRE_PROBE(pc);
        CHECK(entriesT807(f) == std::set<std::string>{pa.entry, pb.entry, pc.entry});
    }
}

// T-808 (22-bundle.md, T-807 record, supplementary rule 5): a directory without the marker whose newest
// modification time is within 10 minutes may be another process's unpack in progress -- it stays.
TEST_CASE("T-808 .xtb cache: an unmarked directory modified in the last 10 minutes is not evicted") {
    const CacheFixtureT807 f = fixtureT807("grace");
    const std::vector<std::string> cap4 = {capT807(kMaxEntriesT807, "4")};
    const ProbeT807 pa = probeT807(f, xtbT807(f, "a"), cap4);
    const ProbeT807 pb = probeT807(f, xtbT807(f, "b"), cap4);
    REQUIRE_PROBE(pa);
    REQUIRE_PROBE(pb);
    const std::string fresh = "0000000000000001";  // sorts before `old`: evicted first without the rule
    const std::string old = "0000000000000002";
    writeT807(f.cache() / fresh / "manifest.json", "{");
    writeT807(f.cache() / old / "manifest.json", "{");
    ageT808(f.cache() / old);
    REQUIRE(entriesT807(f).size() == 4);
    const ProbeT807 pc = probeT807(f, xtbT807(f, "c"), cap4);  // 5 copies over a cap of 4: one goes
    REQUIRE_PROBE(pc);
    CHECK(entriesT807(f) == std::set<std::string>{fresh, pa.entry, pb.entry, pc.entry});
}

TEST_CASE("T-807 .xtb cache: 0 lifts a cap, an invalid value counts as unset") {
    SUBCASE("0 and 0: nothing is evicted") {
        const CacheFixtureT807 f = fixtureT807("zero");
        for (int i = 0; i < 10; ++i) {
            const ProbeT807 probe = probeT807(f, xtbT807(f, "z" + std::to_string(i), 200 * 1024),
                                              {capT807(kMaxEntriesT807, "0"), capT807(kMaxMbT807, "0")});
            REQUIRE_PROBE(probe);
        }
        CHECK(entriesT807(f).size() == 10);
    }
    SUBCASE("not a number, negative: the default of 8") {
        const CacheFixtureT807 f = fixtureT807("invalid");
        const char* const values[] = {"abc", "-1", "3x", "1.5", "-0"};
        for (int i = 0; i < 10; ++i) {
            const std::string value = values[i % 5];
            const ProbeT807 probe = probeT807(f, xtbT807(f, "v" + std::to_string(i)),
                                              {capT807(kMaxEntriesT807, value), capT807(kMaxMbT807, "-5")});
            REQUIRE_PROBE(probe);
        }
        CHECK(entriesT807(f).size() == 8);
    }
}

TEST_CASE("T-807 .xtb cache: a copy another process holds open is skipped, the others still go") {
    const CacheFixtureT807 f = fixtureT807("held");
    const std::vector<std::string> cap2 = {capT807(kMaxEntriesT807, "2")};
    const ProbeT807 pa = probeT807(f, xtbT807(f, "a"), cap2);
    const ProbeT807 pb = probeT807(f, xtbT807(f, "b"), cap2);
    REQUIRE_PROBE(pa);
    REQUIRE_PROBE(pb);
    ProbeT807 pc;
    {
        std::ifstream held(f.cache() / pa.entry / "manifest.json", std::ios::binary);  // the oldest copy
        REQUIRE(held.is_open());
        pc = probeT807(f, xtbT807(f, "c"), cap2);
        REQUIRE_PROBE(pc);
#if defined(_WIN32)
        // An open file keeps its directory from being renamed or removed on Windows.
        CHECK(entriesT807(f) == std::set<std::string>{pa.entry, pc.entry});
        CHECK(fs::is_regular_file(f.cache() / pa.entry / "manifest.json"));
#else
        // POSIX removes a directory whose files are open (the open file stays readable): the
        // oldest copy goes as usual.
        CHECK(entriesT807(f) == std::set<std::string>{pb.entry, pc.entry});
#endif
    }
}
