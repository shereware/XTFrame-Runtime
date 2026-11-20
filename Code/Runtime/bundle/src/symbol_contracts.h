#pragma once

#include <string>
#include <vector>

#include "xt/bundle/bundle.h"

/// T-200 (Docs/design/22-bundle.md §22.10 `symbols`): pack()'s symbol contract collection.
/// Internal to xt_bundle.

namespace xt::bundle {

/// The contracts of every `<symbol>` in `<rootDir>/<symbolsDir>/*.xt` (by file name) and then in
/// each of `docs` (paths relative to `rootDir`); the first definition of a name wins. `source` is
/// relative to `rootDir`. Unreadable or unparsable files are skipped. T-741 (56 §56.3): between the
/// flat files and `docs`, every `<rootDir>/<symbolsDir>/<ns>/*.xt` (namespace directories -- group
/// id characters -- in name order, one level), whose symbols are named `<ns>/<name>`; names are
/// unique as qualified names.
std::vector<SymbolContract> collectSymbolContracts(const std::string& rootDir, const std::string& symbolsDir,
                                                   const std::vector<std::string>& docs);

}  // namespace xt::bundle
