#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "xt/bundle/bundle.h"
#include "xt/ir.h"

/// T-179: `strings/<lang>.csv` tables (Docs/design/22-bundle.md §22.10), the provider that serves
/// them to a Context (Docs/design/13-api.md §13.8.2) and the coverage figures `xtframe-browse` shows.
///
/// Not covered by Docs/tasks/T-179.md (conservative choices made here):
///   - The csv has no mandatory header; a first record reading exactly `key,text` is skipped.
///   - A record with other than two fields, a quote inside an unquoted field, text after a closing
///     quote, or an unterminated quoted field makes the whole table malformed.
///   - A key repeated in one table: the later row wins (pack() warns about it).

namespace xt::bundle {

namespace {

/// Sets `*error` when the caller asked for a message.
void setErrorLocalization(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

bool parseStringsCsv(std::string_view text, std::vector<StringsRow>& out, std::string* error) {
    size_t i = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFu &&
        static_cast<unsigned char>(text[1]) == 0xBBu && static_cast<unsigned char>(text[2]) == 0xBFu) {
        i = 3;
    }
    int line = 1;
    bool firstRecord = true;
    while (i < text.size()) {
        const int recordLine = line;
        std::vector<std::string> fields;
        std::string field;
        bool endOfRecord = false;
        while (!endOfRecord) {
            field.clear();
            if (i < text.size() && text[i] == '"') {
                ++i;
                bool closed = false;
                while (i < text.size()) {
                    const char c = text[i];
                    if (c == '"') {
                        if (i + 1 < text.size() && text[i + 1] == '"') {
                            field.push_back('"');
                            i += 2;
                            continue;
                        }
                        ++i;
                        closed = true;
                        break;
                    }
                    if (c == '\n') {
                        ++line;
                    }
                    field.push_back(c);
                    ++i;
                }
                if (!closed) {
                    setErrorLocalization(
                        error, "line " + std::to_string(recordLine) + ": unterminated quoted field");
                    return false;
                }
                if (i < text.size() && text[i] != ',' && text[i] != '\n' && text[i] != '\r') {
                    setErrorLocalization(error,
                                         "line " + std::to_string(line) + ": text after a closing quote");
                    return false;
                }
            } else {
                while (i < text.size() && text[i] != ',' && text[i] != '\n' && text[i] != '\r') {
                    if (text[i] == '"') {
                        setErrorLocalization(
                            error, "line " + std::to_string(line) + ": quote inside an unquoted field");
                        return false;
                    }
                    field.push_back(text[i]);
                    ++i;
                }
            }
            fields.push_back(field);
            if (i < text.size() && text[i] == ',') {
                ++i;
                continue;
            }
            // End of record: CRLF, LF, a lone CR, or the end of the text.
            if (i < text.size() && text[i] == '\r') {
                ++i;
            }
            if (i < text.size() && text[i] == '\n') {
                ++i;
            }
            ++line;
            endOfRecord = true;
        }
        if (fields.size() == 1 && fields[0].empty()) {
            continue;  // Empty line.
        }
        if (fields.size() != 2) {
            setErrorLocalization(error, "line " + std::to_string(recordLine) +
                                            ": expected 2 fields (key,text), got " +
                                            std::to_string(fields.size()));
            return false;
        }
        if (firstRecord && fields[0] == "key" && fields[1] == "text") {
            firstRecord = false;
            continue;
        }
        firstRecord = false;
        out.push_back(StringsRow{std::move(fields[0]), std::move(fields[1]), recordLine});
    }
    return true;
}

Status BundleLocalizationProvider::load(const Bundle& bundle, std::string* error) {
    for (const StringsInfo& info : bundle.strings()) {
        const Status status = loadFile(info.lang, detail::joinPath(bundle.dir(), info.file), error);
        if (status != Status::kOk) {
            return status;
        }
    }
    return Status::kOk;
}

Status BundleLocalizationProvider::loadFile(std::string_view lang, std::string_view path,
                                            std::string* error) {
    std::string text;
    if (!detail::readFile(std::string(path), text)) {
        setErrorLocalization(error, "cannot read '" + std::string(path) + "'");
        return Status::kIoError;
    }
    std::string message;
    const Status status = addCsv(lang, text, &message);
    if (status != Status::kOk) {
        setErrorLocalization(error, std::string(path) + ": " + message);
    }
    return status;
}

Status BundleLocalizationProvider::addCsv(std::string_view lang, std::string_view csvText,
                                          std::string* error) {
    std::vector<StringsRow> rows;
    if (!parseStringsCsv(csvText, rows, error)) {
        return Status::kInvalidArgument;
    }
    std::map<std::string, std::string, std::less<>> table;
    for (StringsRow& row : rows) {
        table[row.key] = std::move(row.text);
    }
    m_tables[std::string(lang)] = std::move(table);
    return Status::kOk;
}

void BundleLocalizationProvider::set(std::string_view lang, std::string_view key, std::string_view text) {
    auto it = m_tables.find(lang);
    if (it == m_tables.end()) {
        it = m_tables.emplace(std::string(lang), std::map<std::string, std::string, std::less<>>{}).first;
    }
    it->second[std::string(key)] = std::string(text);
}

std::vector<std::string> BundleLocalizationProvider::languages() const {
    std::vector<std::string> out;
    out.reserve(m_tables.size());
    for (const auto& entry : m_tables) {
        out.push_back(entry.first);
    }
    return out;
}

std::string_view BundleLocalizationProvider::resolve(std::string_view key, std::string_view lang) {
    std::string_view tag = lang;
    while (!tag.empty()) {
        const auto table = m_tables.find(tag);
        if (table != m_tables.end()) {
            const auto row = table->second.find(key);
            if (row != table->second.end() && !row->second.empty()) {
                return row->second;
            }
        }
        const size_t dash = tag.rfind('-');
        if (dash == std::string_view::npos) {
            break;
        }
        tag = tag.substr(0, dash);
    }
    return std::string_view{};
}

std::vector<std::string> localizationKeys(IrBlob blob) {
    std::vector<std::string> keys;
    Result<IrView> view = IrView::open(blob);
    if (!view.ok()) {
        return keys;
    }
    for (uint32_t key : view.value().localization().keys()) {
        keys.emplace_back(view.value().string(key));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::vector<LanguageCoverage> languageCoverage(const Bundle& bundle) {
    std::set<std::string> used;
    for (const UiInfo& ui : bundle.uis()) {
        if (ui.file.empty()) {
            continue;
        }
        std::string bytes;
        if (!detail::readFile(detail::joinPath(bundle.dir(), ui.file), bytes)) {
            continue;
        }
        for (std::string& key :
             localizationKeys(IrBlob{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()})) {
            used.insert(std::move(key));
        }
    }
    std::vector<LanguageCoverage> out;
    for (const StringsInfo& info : bundle.strings()) {
        LanguageCoverage coverage;
        coverage.lang = info.lang;
        coverage.total = static_cast<uint32_t>(used.size());
        std::string text;
        std::vector<StringsRow> rows;
        if (detail::readFile(detail::joinPath(bundle.dir(), info.file), text)) {
            parseStringsCsv(text, rows);
        }
        std::set<std::string> translated;
        for (const StringsRow& row : rows) {
            if (!row.text.empty() && used.count(row.key) != 0) {
                translated.insert(row.key);
            }
        }
        coverage.translated = static_cast<uint32_t>(translated.size());
        out.push_back(std::move(coverage));
    }
    return out;
}

}  // namespace xt::bundle
