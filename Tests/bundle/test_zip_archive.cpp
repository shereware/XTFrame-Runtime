#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "json.hpp"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/symbol_set.h"
#include "xt/bundle/zip_archive.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// T-500 (Docs/design/49-symbol-sets.md 49.4, 22 §22.5, 14 §14.9): xt/bundle/zip_archive.h --
/// zipDirectory() / unzipToDirectory() / listZip() round trips, the entry checks against hand-made
/// archives, the limits, broken input; exportSet() / importSet() against the directory form; an
/// `.xtb` opened through Bundle::open(). The hand-made archives are written by this file's own
/// tiny ZIP writer (the test must not include miniz.h: Tools/Scripts/check_layering.sh rule 6).

using namespace xt;
namespace fs = std::filesystem;

namespace {

std::string readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

fs::path freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_zip_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::map<std::string, std::string> fileHashes(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            out[compile::genericPathToUtf8(e.path().lexically_relative(dir))] =
                util::sha256Hex(readBytes(e.path()));
        }
    }
    return out;
}

/// A directory of a few files: nested, binary, empty.
fs::path sampleTree(const std::string& name) {
    const fs::path dir = freshDir(name) / "tree";
    writeBytes(dir / "a.txt", "alpha\n");
    writeBytes(dir / "sub" / "b.bin", std::string("\0\1\2\xff\xfe binary MARKER-STORED-BYTES \0", 36));
    writeBytes(dir / "sub" / "deeper" / "c.json", "{\"c\": 3}\n");
    writeBytes(dir / "empty.txt", "");
    return dir;
}

uint32_t crc32Of(const std::string& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const char c : bytes) {
        crc ^= static_cast<unsigned char>(c);
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void put16(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put32(std::string& out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

struct RawEntry {
    std::string name;
    std::string data;
    /// The external attributes (UNIX st_mode << 16 for a UNIX writer; 0 for a Windows one).
    uint32_t externalAttr = 0;
    /// General purpose flags (bit 0 = encrypted).
    uint32_t flags = 0;
    /// Compression method (0 = stored).
    uint32_t method = 0;
    /// A wrong CRC on purpose.
    bool badCrc = false;
};

/// A stored ZIP of `entries` as a PKZIP writer lays it out (local headers, central directory,
/// end record), no zip64.
std::string makeZip(const std::vector<RawEntry>& entries) {
    std::string out;
    std::string central;
    for (const RawEntry& e : entries) {
        const uint32_t offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = e.badCrc ? ~crc32Of(e.data) : crc32Of(e.data);
        put32(out, 0x04034b50u);
        put16(out, 20);
        put16(out, e.flags);
        put16(out, e.method);
        put16(out, 0);
        put16(out, 0);
        put32(out, crc);
        put32(out, static_cast<uint32_t>(e.data.size()));
        put32(out, static_cast<uint32_t>(e.data.size()));
        put16(out, static_cast<uint32_t>(e.name.size()));
        put16(out, 0);
        out += e.name;
        out += e.data;

        put32(central, 0x02014b50u);
        put16(central, (3u << 8) | 20u);  // made by UNIX
        put16(central, 20);
        put16(central, e.flags);
        put16(central, e.method);
        put16(central, 0);
        put16(central, 0);
        put32(central, crc);
        put32(central, static_cast<uint32_t>(e.data.size()));
        put32(central, static_cast<uint32_t>(e.data.size()));
        put16(central, static_cast<uint32_t>(e.name.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, e.externalAttr);
        put32(central, offset);
        central += e.name;
    }
    const uint32_t centralOffset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(entries.size()));
    put16(out, static_cast<uint32_t>(entries.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, centralOffset);
    put16(out, 0);
    return out;
}

/// The archive `entries` written to `<name>/evil.zip`, extracted into `<name>/out`: the status and
/// whether anything was left behind.
struct Attempt {
    Status status = Status::kOk;
    std::string error;
    bool leftovers = false;
};

Attempt tryExtract(const std::string& name, const std::vector<RawEntry>& entries,
                   bundle::ZipReadOptions options = {}) {
    const fs::path dir = freshDir(name);
    const fs::path zip = dir / "evil.zip";
    writeBytes(zip, makeZip(entries));
    Attempt attempt;
    attempt.status = bundle::unzipToDirectory(zip, dir / "out", options, &attempt.error);
    std::error_code ec;
    attempt.leftovers = fs::exists(dir / "out", ec) || fs::exists(dir / "escaped.txt", ec) ||
                        fs::exists(xt::test::tempDir() / "escaped.txt", ec);
    return attempt;
}

const fs::path kPresetGroup =
    fs::path(XT_TEST_BUNDLE_DIR) / ".." / ".." / "Assets" / "Symbols" / "groups" / "broadsheet-2026";

fs::path emptyLibrary(const std::string& name) {
    const fs::path dir = freshDir(name) / "lib";
    writeBytes(dir / "library.json",
               R"({ "version": 1, "id": "t500", "name": "T-500", "readOnly": false, "groups": [] })");
    fs::create_directories(dir / "groups");
    return dir;
}

}  // namespace

TEST_CASE("T-500 zipDirectory / unzipToDirectory round trip: the same bytes, stored, listed sorted") {
    const fs::path tree = sampleTree("roundtrip");
    const fs::path zip = tree.parent_path() / "tree.zip";
    std::string error;
    REQUIRE_MESSAGE(bundle::zipDirectory(tree, zip, {}, &error) == Status::kOk, error);
    CHECK(error.empty());
    CHECK_FALSE(fs::exists(tree.parent_path() / "tree.zip.tmp"));

    // Store mode: the file bytes are in the archive verbatim (22 §22.2's reason for no compression).
    const std::string archive = readBytes(zip);
    CHECK(archive.find("MARKER-STORED-BYTES") != std::string::npos);
    CHECK(archive.find("{\"c\": 3}") != std::string::npos);

    std::vector<bundle::ZipEntry> entries;
    REQUIRE(bundle::listZip(zip, entries, &error) == Status::kOk);
    std::vector<std::string> names;
    for (const bundle::ZipEntry& e : entries) {
        names.push_back(e.name);
        CHECK_FALSE(e.isDirectory);
    }
    CHECK(names == std::vector<std::string>{"a.txt", "empty.txt", "sub/b.bin", "sub/deeper/c.json"});
    CHECK(entries[2].size == 36);
    CHECK(entries[1].size == 0);

    const fs::path out = tree.parent_path() / "out";
    REQUIRE_MESSAGE(bundle::unzipToDirectory(zip, out, {}, &error) == Status::kOk, error);
    CHECK(fileHashes(tree) == fileHashes(out));
    CHECK(readBytes(out / "sub" / "b.bin") == readBytes(tree / "sub" / "b.bin"));

    // A second extraction into the non-empty directory is refused; an existing archive is replaced.
    CHECK(bundle::unzipToDirectory(zip, out, {}, &error) == Status::kInvalidArgument);
    CHECK(error.find("not empty") != std::string::npos);
    writeBytes(tree / "d.txt", "delta");
    REQUIRE(bundle::zipDirectory(tree, zip, {}, &error) == Status::kOk);
    REQUIRE(bundle::listZip(zip, entries, &error) == Status::kOk);
    CHECK(entries.size() == 5);

    // Deflate when asked; the round trip still holds.
    bundle::ZipWriteOptions deflate;
    deflate.store = false;
    const fs::path zip2 = tree.parent_path() / "tree-deflate.zip";
    REQUIRE(bundle::zipDirectory(tree, zip2, deflate, &error) == Status::kOk);
    const fs::path out2 = tree.parent_path() / "out2";
    REQUIRE_MESSAGE(bundle::unzipToDirectory(zip2, out2, {}, &error) == Status::kOk, error);
    CHECK(fileHashes(tree) == fileHashes(out2));

    // Not a directory / no such archive.
    CHECK(bundle::zipDirectory(tree / "a.txt", zip2, {}, &error) == Status::kNotFound);
    CHECK(bundle::zipDirectory(tree / "none", zip2, {}, &error) == Status::kNotFound);
    CHECK(bundle::unzipToDirectory(tree.parent_path() / "none.zip", out2, {}, &error) == Status::kNotFound);
    CHECK(bundle::listZip(tree.parent_path() / "none.zip", entries, &error) == Status::kNotFound);
    CHECK(entries.empty());
}

TEST_CASE("T-500 unzipToDirectory refuses path traversal, links, duplicates and odd names, keeping nothing") {
    const RawEntry good{"ok.txt", "fine", 0x81A4u << 16, 0, 0, false};  // -rw-r--r--
    SUBCASE("a good archive from the hand-made writer extracts (the writer itself is sound)") {
        const Attempt a =
            tryExtract("good", {good, RawEntry{"dir/", "", 0x41EDu << 16}, RawEntry{"dir/x", "y"}});
        CHECK_MESSAGE(a.status == Status::kOk, a.error);
        CHECK(a.leftovers);  // the output directory exists
        CHECK(readBytes(xt::test::tempDir() / "xt_zip_good" / "out" / "dir" / "x") == "y");
    }
    SUBCASE("names that leave the directory or cannot be written are refused") {
        const std::vector<std::pair<const char*, std::string>> badNames = {
            {"dotdot", "../escaped.txt"},
            {"dotdot_mid", "a/../../escaped.txt"},
            {"absolute", "/escaped.txt"},
            {"backslash", "a\\..\\escaped.txt"},
            {"drive", "C:/escaped.txt"},
            {"drive2", "C:escaped.txt"},
            {"dot_segment", "a/./b.txt"},
            {"empty_segment", "a//b.txt"},
            {"control", std::string("a\nb.txt")},
            {"nul", std::string("a\0b.txt", 7)},
            {"empty_name", ""},
            {"windows_chars", "a?b.txt"},
        };
        for (const auto& [name, entry] : badNames) {
            const std::string label = name;
            CAPTURE(label);
            const Attempt a = tryExtract("bad_" + label, {good, RawEntry{entry, "evil"}});
            CHECK(a.status == Status::kInvalidArgument);
            CHECK_FALSE(a.error.empty());
            CHECK_FALSE(a.leftovers);
        }
    }
    SUBCASE("a symbolic link entry (UNIX mode S_IFLNK) and a device are refused") {
        Attempt a = tryExtract("symlink", {good, RawEntry{"link", "ok.txt", 0xA1FFu << 16}});
        CHECK(a.status == Status::kInvalidArgument);
        CHECK(a.error.find("symbolic link") != std::string::npos);
        CHECK_FALSE(a.leftovers);
        a = tryExtract("device", {good, RawEntry{"dev", "", 0x21B6u << 16}});
        CHECK(a.status == Status::kInvalidArgument);
        CHECK_FALSE(a.leftovers);
    }
    SUBCASE("an encrypted entry, an unknown method and a duplicate name are refused") {
        Attempt a = tryExtract("encrypted", {good, RawEntry{"secret.txt", "x", 0, 1}});
        CHECK(a.status == Status::kInvalidArgument);
        CHECK_FALSE(a.leftovers);
        a = tryExtract("method", {good, RawEntry{"odd.txt", "x", 0, 0, 99}});
        CHECK(a.status == Status::kInvalidArgument);
        CHECK_FALSE(a.leftovers);
        a = tryExtract("duplicate", {good, RawEntry{"ok.txt", "second"}});
        CHECK(a.status == Status::kInvalidArgument);
        CHECK(a.error.find("twice") != std::string::npos);
        CHECK_FALSE(a.leftovers);
    }
    SUBCASE("a CRC mismatch is refused after extraction began, and nothing stays") {
        RawEntry corrupt{"later.txt", "payload"};
        corrupt.badCrc = true;
        const Attempt a = tryExtract("crc", {good, corrupt});
        CHECK(a.status == Status::kInvalidArgument);
        CHECK_FALSE(a.leftovers);
    }
}

TEST_CASE("T-500 limits: maxBytes and maxEntries are enforced both ways") {
    const fs::path tree = sampleTree("limits");
    const fs::path zip = tree.parent_path() / "tree.zip";
    std::string error;
    bundle::ZipWriteOptions few;
    few.maxEntries = 3;
    CHECK(bundle::zipDirectory(tree, zip, few, &error) == Status::kInvalidArgument);
    CHECK_FALSE(fs::exists(zip));
    bundle::ZipWriteOptions small;
    small.maxBytes = 10;
    CHECK(bundle::zipDirectory(tree, zip, small, &error) == Status::kInvalidArgument);
    CHECK_FALSE(fs::exists(zip));
    REQUIRE(bundle::zipDirectory(tree, zip, {}, &error) == Status::kOk);

    bundle::ZipReadOptions fewRead;
    fewRead.maxEntries = 3;
    const fs::path out = tree.parent_path() / "out";
    CHECK(bundle::unzipToDirectory(zip, out, fewRead, &error) == Status::kInvalidArgument);
    CHECK(error.find("entries") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
    bundle::ZipReadOptions smallRead;
    smallRead.maxBytes = 40;
    CHECK(bundle::unzipToDirectory(zip, out, smallRead, &error) == Status::kInvalidArgument);
    CHECK(error.find("bytes") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
    // The limit counts the sizes the central directory claims, so a lying entry is caught too.
    const Attempt lying = tryExtract("lying", {RawEntry{"big.txt", std::string(100, 'x')}}, smallRead);
    CHECK(lying.status == Status::kInvalidArgument);
    CHECK_FALSE(lying.leftovers);
}

TEST_CASE("T-500 a broken archive is refused without a crash") {
    const fs::path dir = freshDir("broken");
    std::string error;
    std::vector<bundle::ZipEntry> entries;
    const std::vector<std::pair<const char*, std::string>> junk = {
        {"empty", ""},
        {"text", "this is not a zip archive at all, just some text\n"},
        {"sig_only", std::string("PK\x05\x06", 4)},
        {"truncated", makeZip({RawEntry{"a.txt", "hello"}}).substr(0, 40)},
        {"no_central", makeZip({RawEntry{"a.txt", "hello"}}).substr(0, 35)},
    };
    for (const auto& [name, bytes] : junk) {
        CAPTURE(name);
        const fs::path zip = dir / (std::string(name) + ".zip");
        writeBytes(zip, bytes);
        const Status listed = bundle::listZip(zip, entries, &error);
        const Status unzipped = bundle::unzipToDirectory(zip, dir / (std::string(name) + "_out"), {}, &error);
        CHECK(listed != Status::kOk);
        CHECK(unzipped != Status::kOk);
        CHECK(listed != Status::kInternal);
        CHECK(unzipped != Status::kInternal);
        CHECK_FALSE(fs::exists(dir / (std::string(name) + "_out")));
    }
    // A central directory whose entry count lies.
    std::string lying = makeZip({RawEntry{"a.txt", "hello"}});
    lying[lying.size() - 12] = 9;  // the "entries on this disk" field
    writeBytes(dir / "lying.zip", lying);
    CHECK(bundle::listZip(dir / "lying.zip", entries, &error) == Status::kInvalidArgument);
}

TEST_CASE("T-500 exportSet then importSet equals the directory form; a tampered archive is refused") {
    const fs::path root = freshDir("sets");
    bundle::ExportOptions exportOptions;
    exportOptions.tempDir = compile::genericPathToUtf8(root / "tmp");
    const fs::path setDir = root / "set-dir";
    const bundle::ExportReport asDir = bundle::exportGroup(kPresetGroup, setDir, exportOptions);
    REQUIRE_MESSAGE(asDir.status == Status::kOk, asDir.error);
    const fs::path archive = root / "set.xtsymbols";
    const bundle::ExportReport asZip = bundle::exportSet(kPresetGroup, archive, exportOptions);
    REQUIRE_MESSAGE(asZip.status == Status::kOk, asZip.error);
    CHECK(asZip.files == asDir.files);
    CHECK(asZip.set.id == asDir.set.id);
    CHECK(asZip.set.setVersion == asDir.set.setVersion);
    CHECK_FALSE(fs::exists(root / "tmp" / "xtframe-symbols" / "export"));  // staging removed
    std::error_code ec;
    CHECK(fs::directory_iterator(root / "tmp" / "xtframe-symbols", ec) == fs::directory_iterator());

    // The archive holds the exported tree, byte for byte.
    std::vector<bundle::ZipEntry> entries;
    std::string error;
    REQUIRE(bundle::listZip(archive, entries, &error) == Status::kOk);
    std::vector<std::string> names;
    for (const bundle::ZipEntry& e : entries) {
        names.push_back(e.name);
    }
    CHECK(names == asDir.files);
    const fs::path unpacked = root / "unpacked";
    REQUIRE(bundle::unzipToDirectory(archive, unpacked, {}, &error) == Status::kOk);
    CHECK(fileHashes(unpacked) == fileHashes(setDir));

    // Import both forms into two libraries: the same files (group.json differs only by its time).
    bundle::ImportOptions importOptions;
    importOptions.tempDir = compile::genericPathToUtf8(root / "tmp");
    const fs::path libDir = emptyLibrary("sets_lib_dir");
    const fs::path libZip = emptyLibrary("sets_lib_zip");
    const bundle::ImportReport fromDir = bundle::importGroupDir(setDir, libDir, importOptions);
    REQUIRE_MESSAGE(fromDir.status == Status::kOk, fromDir.error);
    const bundle::ImportReport fromZip = bundle::importSet(archive, libZip, importOptions);
    REQUIRE_MESSAGE(fromZip.status == Status::kOk, fromZip.error);
    REQUIRE(fromZip.groups.size() == 1);
    CHECK(fromZip.groups[0].action == bundle::ImportAction::Added);
    CHECK(fromZip.groups[0].targetId == fromDir.groups[0].targetId);
    CHECK(fromZip.groups[0].lint.errors() == fromDir.groups[0].lint.errors());
    CHECK(fs::directory_iterator(root / "tmp" / "xtframe-symbols", ec) == fs::directory_iterator());
    const fs::path groupDir = libDir / "groups" / fromDir.groups[0].targetId;
    const fs::path groupZip = libZip / "groups" / fromZip.groups[0].targetId;
    const std::map<std::string, std::string> a = fileHashes(groupDir);
    const std::map<std::string, std::string> b = fileHashes(groupZip);
    REQUIRE(a.size() == b.size());
    CHECK(a.size() > 30);
    for (const auto& [file, hash] : a) {
        CAPTURE(file);
        const auto it = b.find(file);
        REQUIRE(it != b.end());
        if (file != "group.json") {
            CHECK(it->second == hash);
        }
    }
    nlohmann::json metaDir = nlohmann::json::parse(readBytes(groupDir / "group.json"));
    nlohmann::json metaZip = nlohmann::json::parse(readBytes(groupZip / "group.json"));
    metaDir.erase("importedAt");
    metaZip.erase("importedAt");
    CHECK(metaDir == metaZip);
    bundle::SymbolLibrary loaded;
    REQUIRE(bundle::loadLibrary(libZip, loaded) == Status::kOk);
    REQUIRE(loaded.groups.size() == 1);
    CHECK(loaded.groups[0].problems.empty());
    CHECK(loaded.groups[0].source.set == asZip.set.author + "/" + asZip.set.id);

    // A tampered archive: an entry that leaves the set is refused and the library stays as it was.
    const fs::path libTampered = emptyLibrary("sets_lib_tampered");
    const std::string before = readBytes(libTampered / "library.json");
    writeBytes(root / "tampered.xtsymbols", makeZip({RawEntry{"set.json", readBytes(setDir / "set.json")},
                                                     RawEntry{"../escaped.txt", "x"}}));
    const bundle::ImportReport tampered =
        bundle::importSet(root / "tampered.xtsymbols", libTampered, importOptions);
    CHECK(tampered.status == Status::kInvalidArgument);
    CHECK(tampered.error.find("escaped.txt") != std::string::npos);
    CHECK(readBytes(libTampered / "library.json") == before);
    CHECK(fs::directory_iterator(libTampered / "groups", ec) == fs::directory_iterator());
    CHECK_FALSE(fs::exists(root / "tmp" / "xtframe-symbols" / "escaped.txt"));
    // Not an archive at all / no such file.
    writeBytes(root / "text.xtsymbols", "nope");
    CHECK(bundle::importSet(root / "text.xtsymbols", libTampered, importOptions).status ==
          Status::kInvalidArgument);
    CHECK(bundle::importSet(root / "none.xtsymbols", libTampered, importOptions).status == Status::kNotFound);
    CHECK(fs::directory_iterator(root / "tmp" / "xtframe-symbols", ec) == fs::directory_iterator());
}

TEST_CASE("T-500 .xtb: a packed bundle zipped and opened through Bundle::open reads the same manifest") {
    const fs::path root = freshDir("xtb");
    const fs::path source = root / "src";
    std::error_code ec;
    fs::copy(std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/sample", source, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string error;
    Result<bundle::ProjectFile> project = bundle::ProjectFile::load(source.generic_string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    bundle::PackOptions options;
    options.outDir = (root / "out").generic_string();
    options.thumbnails = false;
    REQUIRE(bundle::pack(project.value(), options).ok);

    const fs::path archive = root / "sample.xtb";
    REQUIRE_MESSAGE(bundle::zipDirectory(root / "out", archive, {}, &error) == Status::kOk, error);

    Result<std::unique_ptr<bundle::Bundle>> fromDir = bundle::Bundle::open(options.outDir, nullptr, &error);
    REQUIRE_MESSAGE(fromDir.ok(), error);
    Result<std::unique_ptr<bundle::Bundle>> fromZip =
        bundle::Bundle::open(compile::genericPathToUtf8(archive), nullptr, &error);
    REQUIRE_MESSAGE(fromZip.ok(), error);
    const bundle::Bundle& a = *fromDir.value();
    const bundle::Bundle& b = *fromZip.value();
    CHECK(a.id() == b.id());
    CHECK(a.name() == b.name());
    REQUIRE(a.uis().size() == b.uis().size());
    for (size_t i = 0; i < a.uis().size(); ++i) {
        CHECK(a.uis()[i].id == b.uis()[i].id);
        CHECK(a.uis()[i].file == b.uis()[i].file);
        CHECK(a.uis()[i].fileHash == b.uis()[i].fileHash);
        CHECK(a.uis()[i].assets == b.uis()[i].assets);
    }
    // Read from an unpacked copy (not in place): dir() names it and holds the same manifest bytes.
    const fs::path unpacked = compile::pathFromUtf8(std::string(b.dir()));
    CHECK(unpacked != archive);
    CHECK(fs::is_regular_file(unpacked / "manifest.json"));
    CHECK(readBytes(unpacked / "manifest.json") == readBytes(root / "out" / "manifest.json"));
    CHECK(fileHashes(unpacked) == fileHashes(root / "out"));
    // A UI loads through the unpacked copy's own resolver.
    CHECK(b.ui(a.uis()[0].id) != nullptr);

    // A second open reuses the unpacked copy; a different archive gets its own.
    Result<std::unique_ptr<bundle::Bundle>> again =
        bundle::Bundle::open(compile::genericPathToUtf8(archive), nullptr, &error);
    REQUIRE(again.ok());
    CHECK(again.value()->dir() == b.dir());
    fs::remove_all(unpacked, ec);  // the unpacked copy lives under the system temp directory

    // Not an archive: the usual manifest error, not a crash; a wrong extension is a directory read.
    writeBytes(root / "junk.xtb", "not a zip");
    Result<std::unique_ptr<bundle::Bundle>> junk =
        bundle::Bundle::open(compile::genericPathToUtf8(root / "junk.xtb"), nullptr, &error);
    CHECK_FALSE(junk.ok());
    CHECK(junk.status() == Status::kInvalidArgument);
    CHECK(error.find("not a zip archive") != std::string::npos);
    Result<std::unique_ptr<bundle::Bundle>> missing =
        bundle::Bundle::open(compile::genericPathToUtf8(root / "none.xtb"), nullptr, &error);
    CHECK(missing.status() == Status::kNotFound);
}
