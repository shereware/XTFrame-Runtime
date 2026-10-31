#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xt/types.h"

/// T-500 (Docs/design/49-symbol-sets.md 49.4, 22-bundle.md §22.2 / §22.5, 14 §14.9): the one ZIP
/// reader and writer of the runtime layer, behind `.xtsymbols` symbol sets and `.xtb` bundles.
/// Store mode by default (no compression: a host may later map the file and read the entries in
/// place, 22 §22.2), through miniz (FetchContent, Tools/CMake/xt_miniz.cmake; linked by xt_bundle only).
///
/// Every archive is untrusted input. unzipToDirectory() names its checks per entry:
///   1. the name is 1..1024 bytes of valid UTF-8 without NUL or control characters;
///   2. no backslash (a ZIP name uses '/'; `a\..\b` is refused as a whole, not normalised) and
///      none of `: < > " | ? *` (not file name characters on Windows; `C:` among them);
///   3. not absolute, no root name, no leading '/', and no `.` / `..` / empty segment;
///   4. the joined path, normalised, stays under `outDir` (resolveInside, 14 §14.9 rule 5);
///   5. not a symbolic link, device or other special file (the UNIX mode of the entry's external
///      attributes), not encrypted, and a method miniz supports (store or deflate);
///   6. no duplicate name, no more than `maxEntries` entries, and the uncompressed sizes
///      together never exceed `maxBytes` (checked before any byte is written);
///   7. the extracted bytes must match the entry's CRC-32 and size (miniz verifies).
/// A failed check refuses the whole archive; nothing of it is kept in `outDir` (which must be
/// missing or empty before the call, so no pre-existing link can be followed).
///
/// No exception leaves these functions (14 §14.3 catchAll).

namespace xt::bundle {

struct ZipWriteOptions {
    /// true: entries are stored (no compression); false: deflate at the default level.
    bool store = true;
    /// The sum of the file sizes zipDirectory() accepts.
    uint64_t maxBytes = 64ull * 1024ull * 1024ull;
    /// The number of files zipDirectory() accepts.
    size_t maxEntries = 4096;
};

struct ZipReadOptions {
    /// The sum of the uncompressed entry sizes unzipToDirectory() accepts.
    uint64_t maxBytes = 64ull * 1024ull * 1024ull;
    /// The number of entries unzipToDirectory() accepts.
    size_t maxEntries = 4096;
};

struct ZipEntry {
    /// The entry name as stored in the archive, verbatim ('/' separated when the writer followed
    /// the format; nothing is checked here).
    std::string name;
    /// Uncompressed size in bytes.
    uint64_t size = 0;
    /// A directory entry (`name` ends with '/').
    bool isDirectory = false;
};

/// Writes every regular file under `dir` (recursively; names relative to `dir`, '/' separated,
/// sorted) into the archive `outFile` (written to `<outFile>.tmp` and moved over; an existing
/// file is replaced). kNotFound when `dir` is not a directory; kInvalidArgument (`error`) when a
/// symbolic link or special file is met, or more than `maxEntries` files or `maxBytes` bytes;
/// kIoError when a file cannot be read or the archive cannot be written. Directory entries are
/// not written (an empty directory is not preserved). `error`, when non-null, is cleared first.
Status zipDirectory(const std::filesystem::path& dir, const std::filesystem::path& outFile,
                    const ZipWriteOptions& options = {}, std::string* error = nullptr);

/// Extracts the archive `file` into `outDir` (created; must be missing or empty) after the checks
/// in the file comment. kNotFound when `file` cannot be opened; kInvalidArgument (`error`) for a
/// malformed archive, an entry that fails a check, a non-empty `outDir`, or the limits;
/// kIoError when a file cannot be written. On any failure `outDir` is removed again when this
/// call created it, else emptied.
Status unzipToDirectory(const std::filesystem::path& file, const std::filesystem::path& outDir,
                        const ZipReadOptions& options = {}, std::string* error = nullptr);

/// Lists the entries of the archive `file` in archive order, without extracting or checking
/// them. kNotFound when the file cannot be opened; kInvalidArgument (`error`) when it is not a
/// ZIP archive miniz can read.
Status listZip(const std::filesystem::path& file, std::vector<ZipEntry>& entries,
               std::string* error = nullptr);

}  // namespace xt::bundle
