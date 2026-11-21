#pragma once

/// T-827: the reason a tool prints when an IR does not load -- the Status name, and for kInvalidIr
/// what IrView::openError() names (for an IR of another major: "IR major 2 is not supported (this
/// runtime reads IR major 3); recompile the source"), e.g. `kInvalidIr: IR major 2 is not ...`.
/// Every tool under Code/Tools that opens or loads an IR (IrView::open, Context::loadDocument,
/// bundle::loadUi) prints this between the parentheses of its failure line. Header-only
/// (Code/Tools/Common has no library); needs xt_core.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "xt/ir.h"
#include "xt/types.h"

namespace xt::tools {

/// `status`'s name, then ": <reason>" when it is kInvalidIr and IrView::openError(`ir`) names one.
inline std::string irLoadFailure(xt::Status status, xt::IrBlob ir) {
    std::string text = xt::statusName(status);
    if (status == xt::Status::kInvalidIr) {
        const std::string why = xt::IrView::openError(ir);
        if (!why.empty()) {
            text += ": " + why;
        }
    }
    return text;
}

/// irLoadFailure() for the IR in the file `path`, which is read only for kInvalidIr.
inline std::string irFileLoadFailure(xt::Status status, const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    if (status == xt::Status::kInvalidIr) {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return irLoadFailure(status, xt::IrBlob{bytes.data(), bytes.size()});
}

}  // namespace xt::tools
