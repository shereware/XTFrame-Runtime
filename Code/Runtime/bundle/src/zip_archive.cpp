#include "xt/bundle/zip_archive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "symbol_internal.h"
#include "xt/compile/utf8_path.h"

// Third-party (miniz, Tools/CMake/xt_miniz.cmake, T-500): configured through the compile definitions of the
// xt_miniz target (Tools/CMake/xt_miniz.cmake), never by editing the fetched sources. Its
// header is C; MSVC's analysis warnings for it are silenced around the include only.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4244 4245 4267 4334 4456 4457 4701 4702 4706)
#endif
#include "miniz.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// T-500 (Docs/design/49-symbol-sets.md 49.4, 22 §22.2 / §22.5, 14 §14.9): see the header. The
/// archives are streamed through FILE* handles this file opens itself (openFileUtf8: UTF-8 paths,
/// the wide API on Windows), never through miniz's own fopen, which takes narrow names. Helpers
/// live in `xt::bundle::detail::zip` so a unity build of xt_bundle never sees two files' helpers
/// under one name.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using detail::openFileUtf8;
using detail::resolveInside;

namespace detail::zip {

constexpr size_t kMaxEntryName = 1024;

/// Closes a FILE* when it goes out of scope.
struct FileCloser {
    std::FILE* file = nullptr;
    ~FileCloser() {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

/// mz_zip_reader_end / mz_zip_writer_end when it goes out of scope (the FILE* stays ours: miniz
/// closes only the files it opened itself).
struct ZipCloser {
    mz_zip_archive* zip = nullptr;
    bool writer = false;
    ~ZipCloser() {
        if (zip != nullptr) {
            if (writer) {
                mz_zip_writer_end(zip);
            } else {
                mz_zip_reader_end(zip);
            }
        }
    }
};

void setError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string zipError(mz_zip_archive& zip) { return mz_zip_get_error_string(mz_zip_get_last_error(&zip)); }

/// The entry name checks 1..3 of the header: a relative '/' separated name without `.`, `..` or
/// empty segments. Check 4 (inside `outDir`) is resolveInside's.
bool wellFormedName(std::string_view name, std::string& why) {
    if (name.empty() || name.size() > kMaxEntryName) {
        why = "the entry name is empty or longer than 1024 bytes";
        return false;
    }
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            why = "the entry name holds a control character";
            return false;
        }
        if (c == '\\') {
            why = "the entry name holds a backslash";
            return false;
        }
        // Not a file name character on Windows (a `C:` drive among them): the tree could not be
        // written there, so the archive is refused everywhere.
        if (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
            why = "the entry name holds a character that is not allowed in file names";
            return false;
        }
    }
    if (name.front() == '/') {
        why = "the entry name is absolute";
        return false;
    }
    size_t start = 0;
    while (start < name.size()) {
        const size_t slash = name.find('/', start);
        const std::string_view segment =
            name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            why = "the entry name holds an empty, '.' or '..' segment";
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

/// Check 5: the UNIX file type in the external attributes (the high 16 bits hold st_mode when a
/// UNIX tool wrote the archive; 0 when a Windows one did). Regular files and directories only.
bool allowedFileType(mz_uint32 externalAttr, bool isDirectory) {
    const mz_uint32 type = (externalAttr >> 16) & 0xF000u;
    if (type == 0) {
        return true;
    }
    return isDirectory ? type == 0x4000u : type == 0x8000u;
}

/// Opens `file` for reading and initialises `zip` on it; the status to return when it fails.
Status openReader(const fs::path& file, FileCloser& handle, mz_zip_archive& zip, std::string* error) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || !fs::is_regular_file(file, ec)) {
        setError(error, "cannot open " + genericPathToUtf8(file));
        return Status::kNotFound;
    }
    handle.file = openFileUtf8(genericPathToUtf8(file), "rb");
    if (handle.file == nullptr) {
        setError(error, "cannot open " + genericPathToUtf8(file));
        return Status::kNotFound;
    }
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_cfile(&zip, handle.file, static_cast<mz_uint64>(size), 0)) {
        setError(error, genericPathToUtf8(file) + ": not a zip archive (" + zipError(zip) + ")");
        return Status::kInvalidArgument;
    }
    return Status::kOk;
}

struct WriteSink {
    std::FILE* file = nullptr;
    bool failed = false;
};

size_t writeCallback(void* opaque, mz_uint64 /*offset*/, const void* data, size_t n) {
    WriteSink* sink = static_cast<WriteSink*>(opaque);
    if (sink->failed) {
        return 0;
    }
    const size_t written = std::fwrite(data, 1, n, sink->file);
    if (written != n) {
        sink->failed = true;
    }
    return written;
}

/// The files under `dir` as (relative '/' name, path), sorted by name; the checks of zipDirectory.
Status collectFiles(const fs::path& dir, const ZipWriteOptions& options,
                    std::vector<std::pair<std::string, fs::path>>& files, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        setError(error, "not a directory: " + genericPathToUtf8(dir));
        return Status::kNotFound;
    }
    uint64_t bytes = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(dir);
        const std::string name = genericPathToUtf8(rel);
        if (it->is_symlink(ec)) {
            setError(error, "'" + name + "' is a symbolic link");
            return Status::kInvalidArgument;
        }
        if (it->is_directory(ec)) {
            continue;
        }
        if (!it->is_regular_file(ec)) {
            setError(error, "'" + name + "' is not a regular file");
            return Status::kInvalidArgument;
        }
        bytes += static_cast<uint64_t>(it->file_size(ec));
        files.emplace_back(name, it->path());
        if (files.size() > options.maxEntries || bytes > options.maxBytes) {
            setError(error, "more than " + std::to_string(options.maxEntries) + " files or " +
                                std::to_string(options.maxBytes) + " bytes under " + genericPathToUtf8(dir));
            return Status::kInvalidArgument;
        }
    }
    if (ec) {
        setError(error, "cannot read " + genericPathToUtf8(dir) + ": " + ec.message());
        return Status::kIoError;
    }
    std::sort(files.begin(), files.end());
    return Status::kOk;
}

Status zipDirectoryImpl(const fs::path& dir, const fs::path& outFile, const ZipWriteOptions& options,
                        std::string* error) {
    std::vector<std::pair<std::string, fs::path>> files;
    if (const Status status = collectFiles(dir, options, files, error); status != Status::kOk) {
        return status;
    }
    const fs::path temp = fs::path(outFile).concat(".tmp");
    const std::string tempUtf8 = genericPathToUtf8(temp);
    std::error_code ec;
    fs::remove(temp, ec);
    auto failWith = [&](Status status, std::string message) {
        setError(error, std::move(message));
        fs::remove(temp, ec);
        return status;
    };
    {
        FileCloser out;
        out.file = openFileUtf8(tempUtf8, "wb");
        if (out.file == nullptr) {
            return failWith(Status::kIoError, "cannot write " + genericPathToUtf8(outFile));
        }
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_writer_init_cfile(&zip, out.file, 0)) {
            return failWith(Status::kIoError, "cannot start the archive: " + zipError(zip));
        }
        ZipCloser closer{&zip, true};
        const mz_uint level =
            options.store ? static_cast<mz_uint>(MZ_NO_COMPRESSION) : static_cast<mz_uint>(MZ_DEFAULT_LEVEL);
        for (const auto& [name, path] : files) {
            FileCloser in;
            in.file = openFileUtf8(genericPathToUtf8(path), "rb");
            const uintmax_t size = fs::file_size(path, ec);
            if (in.file == nullptr || ec) {
                return failWith(Status::kIoError, "cannot read " + name);
            }
            if (!mz_zip_writer_add_cfile(&zip, name.c_str(), in.file, static_cast<mz_uint64>(size), nullptr,
                                         nullptr, 0, level, nullptr, 0, nullptr, 0)) {
                return failWith(Status::kIoError, "cannot add " + name + ": " + zipError(zip));
            }
        }
        if (!mz_zip_writer_finalize_archive(&zip)) {
            return failWith(Status::kIoError, "cannot finish the archive: " + zipError(zip));
        }
        closer.zip = nullptr;
        if (!mz_zip_writer_end(&zip)) {
            return failWith(Status::kIoError, "cannot finish the archive: " + zipError(zip));
        }
        if (std::fflush(out.file) != 0) {
            return failWith(Status::kIoError, "cannot write " + genericPathToUtf8(outFile));
        }
    }
    fs::rename(temp, outFile, ec);
    if (ec) {
        return failWith(Status::kIoError, "cannot move " + tempUtf8 + " over " + genericPathToUtf8(outFile));
    }
    return Status::kOk;
}

/// One checked entry of an archive about to be extracted.
struct Planned {
    mz_uint index = 0;
    std::string name;
    fs::path path;
    bool isDirectory = false;
};

/// Checks every entry (header items 1..6) and plans the extraction; nothing is written yet.
Status planEntries(mz_zip_archive& zip, const fs::path& outDir, const ZipReadOptions& options,
                   std::vector<Planned>& plan, std::string* error) {
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > options.maxEntries) {
        setError(error, "the archive holds more than " + std::to_string(options.maxEntries) + " entries");
        return Status::kInvalidArgument;
    }
    uint64_t bytes = 0;
    std::set<std::string> seen;
    plan.reserve(count);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            setError(error, "entry " + std::to_string(i) + ": " + zipError(zip));
            return Status::kInvalidArgument;
        }
        const std::string name(stat.m_filename);
        // A NUL inside the stored name would truncate it here (mz_zip_archive_file_stat holds a C
        // string): the stored length must be the length seen, or the entry is refused.
        if (mz_zip_reader_get_filename(&zip, i, nullptr, 0) != name.size() + 1) {
            setError(error, "entry " + std::to_string(i) + ": the entry name holds a NUL byte");
            return Status::kInvalidArgument;
        }
        std::string why;
        const bool isDirectory = stat.m_is_directory != 0;
        std::string_view checked = name;
        if (isDirectory && !checked.empty() && checked.back() == '/') {
            checked.remove_suffix(1);
        }
        if (!wellFormedName(checked, why)) {
            setError(error, "entry '" + name + "': " + why);
            return Status::kInvalidArgument;
        }
        fs::path path;
        if (!resolveInside(outDir, checked, path)) {
            setError(error, "entry '" + name + "' leaves the output directory");
            return Status::kInvalidArgument;
        }
        if (!allowedFileType(stat.m_external_attr, isDirectory)) {
            setError(error, "entry '" + name + "' is a symbolic link or a special file");
            return Status::kInvalidArgument;
        }
        if (stat.m_is_encrypted || !stat.m_is_supported) {
            setError(error, "entry '" + name + "' is encrypted or uses an unsupported method");
            return Status::kInvalidArgument;
        }
        if (!seen.insert(std::string(checked)).second) {
            setError(error, "entry '" + name + "' appears twice");
            return Status::kInvalidArgument;
        }
        if (isDirectory) {
            if (stat.m_uncomp_size != 0) {
                setError(error, "directory entry '" + name + "' has data");
                return Status::kInvalidArgument;
            }
        } else {
            bytes += static_cast<uint64_t>(stat.m_uncomp_size);
            if (bytes > options.maxBytes) {
                setError(error,
                         "the archive unpacks to more than " + std::to_string(options.maxBytes) + " bytes");
                return Status::kInvalidArgument;
            }
        }
        plan.push_back(Planned{i, name, path, isDirectory});
    }
    return Status::kOk;
}

Status unzipToDirectoryImpl(const fs::path& file, const fs::path& outDir, const ZipReadOptions& options,
                            std::string* error) {
    FileCloser handle;
    mz_zip_archive zip;
    if (const Status status = openReader(file, handle, zip, error); status != Status::kOk) {
        return status;
    }
    ZipCloser closer{&zip, false};

    std::error_code ec;
    const bool existed = fs::exists(outDir, ec);
    if (existed) {
        if (!fs::is_directory(outDir, ec) || fs::directory_iterator(outDir, ec) != fs::directory_iterator()) {
            setError(error, "the output directory is not empty: " + genericPathToUtf8(outDir));
            return Status::kInvalidArgument;
        }
    }
    std::vector<Planned> plan;
    if (const Status status = planEntries(zip, outDir, options, plan, error); status != Status::kOk) {
        return status;
    }
    auto failWith = [&](Status status, std::string message) {
        setError(error, std::move(message));
        if (existed) {
            for (const fs::directory_entry& entry : fs::directory_iterator(outDir, ec)) {
                fs::remove_all(entry.path(), ec);
            }
        } else {
            fs::remove_all(outDir, ec);
        }
        return status;
    };
    fs::create_directories(outDir, ec);
    if (ec) {
        return failWith(Status::kIoError, "cannot create " + genericPathToUtf8(outDir));
    }
    for (const Planned& entry : plan) {
        if (entry.isDirectory) {
            fs::create_directories(entry.path, ec);
            if (ec) {
                return failWith(Status::kIoError, "cannot create '" + entry.name + "'");
            }
            continue;
        }
        fs::create_directories(entry.path.parent_path(), ec);
        if (ec) {
            return failWith(Status::kIoError, "cannot create the directory of '" + entry.name + "'");
        }
        WriteSink sink;
        FileCloser out;
        out.file = openFileUtf8(genericPathToUtf8(entry.path), "wb");
        if (out.file == nullptr) {
            return failWith(Status::kIoError, "cannot write '" + entry.name + "'");
        }
        sink.file = out.file;
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, entry.index, &writeCallback, &sink, 0);
        const bool flushed = std::fflush(out.file) == 0;
        // Closed before failWith: Windows cannot remove a file that is still open.
        std::fclose(out.file);
        out.file = nullptr;
        if (sink.failed || !flushed) {
            return failWith(Status::kIoError, "cannot write '" + entry.name + "'");
        }
        if (!extracted) {
            return failWith(Status::kInvalidArgument, "entry '" + entry.name + "': " + zipError(zip));
        }
    }
    return Status::kOk;
}

Status listZipImpl(const fs::path& file, std::vector<ZipEntry>& entries, std::string* error) {
    FileCloser handle;
    mz_zip_archive zip;
    if (const Status status = openReader(file, handle, zip, error); status != Status::kOk) {
        return status;
    }
    ZipCloser closer{&zip, false};
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    entries.reserve(count);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            setError(error, "entry " + std::to_string(i) + ": " + zipError(zip));
            entries.clear();
            return Status::kInvalidArgument;
        }
        ZipEntry entry;
        entry.name = stat.m_filename;
        entry.size = static_cast<uint64_t>(stat.m_uncomp_size);
        entry.isDirectory = stat.m_is_directory != 0;
        entries.push_back(std::move(entry));
    }
    return Status::kOk;
}

}  // namespace detail::zip

Status zipDirectory(const fs::path& dir, const fs::path& outFile, const ZipWriteOptions& options,
                    std::string* error) {
    detail::zip::setError(error, std::string());
    const Status status =
        detail::catchAll([&]() { return detail::zip::zipDirectoryImpl(dir, outFile, options, error); });
    if (status == Status::kInternal) {
        detail::zip::setError(error, "unexpected failure while writing the archive");
    }
    return status;
}

Status unzipToDirectory(const fs::path& file, const fs::path& outDir, const ZipReadOptions& options,
                        std::string* error) {
    detail::zip::setError(error, std::string());
    const Status status =
        detail::catchAll([&]() { return detail::zip::unzipToDirectoryImpl(file, outDir, options, error); });
    if (status == Status::kInternal) {
        detail::zip::setError(error, "unexpected failure while extracting the archive");
    }
    return status;
}

Status listZip(const fs::path& file, std::vector<ZipEntry>& entries, std::string* error) {
    entries.clear();
    detail::zip::setError(error, std::string());
    const Status status = detail::catchAll([&]() { return detail::zip::listZipImpl(file, entries, error); });
    if (status == Status::kInternal) {
        entries.clear();
        detail::zip::setError(error, "unexpected failure while reading the archive");
    }
    return status;
}

}  // namespace xt::bundle
