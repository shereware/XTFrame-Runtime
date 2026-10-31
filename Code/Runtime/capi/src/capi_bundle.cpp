#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bundle_json.h"  // xt::bundle::detail::readFile / joinPath (the T-240 file reader hook).
#include "capi_internal.h"
#include "xt/sw/sw_png.h"

/// `xt_bundle_*` (Docs/design/50-c-abi.md §50.3 "Bundle"): a published directory bundle, opened
/// read-only, plus the shaper its surfaces share.

namespace xt::capi {

bool readBundleFile(const bundle::Bundle& bundle, std::string_view relative, std::string& out) {
    return bundle::detail::readFile(bundle::detail::joinPath(bundle.dir(), relative), out);
}

TextShaper* ensureShaper(xt_bundle& b, bool systemFonts) {
    if (b.shaper == nullptr) {
#ifdef XT_CAPI_HAS_TEXT
        auto shaper = std::make_unique<text::FtTextShaper>();
        // The bundle's own fonts first, then (below) the system fonts, as xtframe-view does.
        // T-555 (22 §22.10.3): every UI's fonts through bundle::addBundleFonts() (default chain
        // order, dedup), so the later loadUi() of each surface registers nothing again.
        for (const bundle::UiInfo& ui : b.bundle->uis()) {
            std::vector<std::string> warnings;
            (void)bundle::addBundleFonts(*b.bundle, ui.id, *shaper, &warnings);
            for (const std::string& warning : warnings) {
                log(1, "xt_surface_create: " + warning);
            }
        }
        b.ftShaper = shaper.get();
        b.shaper = std::move(shaper);
#else
        b.shaper = std::make_unique<NullTextShaper>();
        log(1, "xtframe was built without XT_BUILD_TEXT: text is not drawn (NullTextShaper)");
#endif
    }
    if (systemFonts && !b.systemFontsAdded) {
#ifdef XT_CAPI_HAS_TEXT
        b.ftShaper->addSystemFonts();
#endif
        b.systemFontsAdded = true;
    }
    return b.shaper.get();
}

}  // namespace xt::capi

using xt::capi::fail;

extern "C" {

XT_CAPI xt_bundle* xt_bundle_open(const char* dir_utf8) {
    XT_CAPI_GUARD_OR("xt_bundle_open", xt_bundle*, nullptr, {
        if (dir_utf8 == nullptr) {
            fail(XT_STATUS_INVALID_ARGUMENT, "xt_bundle_open: dir_utf8 is NULL");
            return nullptr;
        }
        std::string error;
        xt::Result<std::unique_ptr<xt::bundle::Bundle>> opened =
            xt::bundle::Bundle::open(dir_utf8, xt::sw::decodeImageBytes, &error);
        if (!opened.ok()) {
            fail(xt::capi::toCapi(opened.status()),
                 std::string("xt_bundle_open: cannot open '") + dir_utf8 +
                     "': " + (error.empty() ? xt::statusName(opened.status()) : error));
            return nullptr;
        }
        auto b = std::make_unique<xt_bundle>();
        b->bundle = std::move(opened.value());
        return b.release();
    });
}

XT_CAPI xt_status xt_bundle_close(xt_bundle* b) {
    XT_CAPI_GUARD("xt_bundle_close", {
        if (b == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_bundle_close: bundle is NULL");
        }
        if (b->dispatchDepth > 0) {
            return fail(XT_STATUS_UNSUPPORTED, "xt_bundle_close: called from inside a callback");
        }
        if (b->surfaceCount > 0) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_bundle_close: " + std::to_string(b->surfaceCount) + " surface(s) still open");
        }
        delete b;
        return XT_STATUS_OK;
    });
}

XT_CAPI const char* xt_bundle_id(const xt_bundle* b) {
    XT_CAPI_GUARD_OR("xt_bundle_id", const char*, "", {
        if (b == nullptr) {
            return "";
        }
        auto* self = const_cast<xt_bundle*>(b);
        return self->scratch.put(b->bundle->id());
    });
}

XT_CAPI const char* xt_bundle_name(const xt_bundle* b) {
    XT_CAPI_GUARD_OR("xt_bundle_name", const char*, "", {
        if (b == nullptr) {
            return "";
        }
        auto* self = const_cast<xt_bundle*>(b);
        return self->scratch.put(b->bundle->name());
    });
}

XT_CAPI const char* xt_bundle_dir(const xt_bundle* b) {
    XT_CAPI_GUARD_OR("xt_bundle_dir", const char*, "", {
        if (b == nullptr) {
            return "";
        }
        auto* self = const_cast<xt_bundle*>(b);
        return self->scratch.put(b->bundle->dir());
    });
}

XT_CAPI uint32_t xt_bundle_ui_count(const xt_bundle* b) {
    XT_CAPI_GUARD_OR("xt_bundle_ui_count", uint32_t, 0u,
                     { return b == nullptr ? 0u : static_cast<uint32_t>(b->bundle->uis().size()); });
}

XT_CAPI xt_status xt_bundle_ui_info(const xt_bundle* b, uint32_t index, xt_ui_info* out) {
    XT_CAPI_GUARD("xt_bundle_ui_info", {
        if (b == nullptr || out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_bundle_ui_info: bundle or out is NULL");
        }
        const auto uis = b->bundle->uis();
        if (index >= uis.size()) {
            return fail(XT_STATUS_OUT_OF_RANGE, "xt_bundle_ui_info: index " + std::to_string(index) +
                                                    " >= ui count " + std::to_string(uis.size()));
        }
        const xt::bundle::UiInfo& info = uis[index];
        auto* self = const_cast<xt_bundle*>(b);
        out->id = self->scratch.put(info.id);
        out->name = self->scratch.put(info.name);
        out->design_width = info.size.x;
        out->design_height = info.size.y;
        out->fit = self->scratch.put(info.fit);
        out->has_flow = info.flow.empty() ? 0 : 1;
        out->port_count = static_cast<uint32_t>(info.ports.size());
        out->contract = self->scratch.put(info.contract);
        return XT_STATUS_OK;
    });
}

XT_CAPI int32_t xt_bundle_find_ui(const xt_bundle* b, const char* ui_id) {
    XT_CAPI_GUARD_OR("xt_bundle_find_ui", int32_t, -1, {
        if (b == nullptr || ui_id == nullptr) {
            return -1;
        }
        const auto uis = b->bundle->uis();
        for (size_t i = 0; i < uis.size(); ++i) {
            if (uis[i].id == ui_id) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    });
}

XT_CAPI xt_status xt_bundle_ui_port_info(const xt_bundle* b, uint32_t ui_index, uint32_t port_index,
                                         xt_port_info* out) {
    XT_CAPI_GUARD("xt_bundle_ui_port_info", {
        if (b == nullptr || out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_bundle_ui_port_info: bundle or out is NULL");
        }
        const auto uis = b->bundle->uis();
        if (ui_index >= uis.size()) {
            return fail(XT_STATUS_OUT_OF_RANGE, "xt_bundle_ui_port_info: ui index out of range");
        }
        const auto& ports = uis[ui_index].ports;
        if (port_index >= ports.size()) {
            return fail(XT_STATUS_OUT_OF_RANGE, "xt_bundle_ui_port_info: port index out of range");
        }
        auto* self = const_cast<xt_bundle*>(b);
        out->name = self->scratch.put(ports[port_index].name);
        out->type = self->scratch.put(ports[port_index].type);
        out->is_input = ports[port_index].isInput ? 1 : 0;
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_bundle_get_spec(const xt_bundle* b, xt_bundle_spec* out) {
    XT_CAPI_GUARD("xt_bundle_get_spec", {
        if (b == nullptr || out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_bundle_get_spec: bundle or out is NULL");
        }
        const xt::bundle::BundleSpec* given = b->bundle->spec();
        const xt::bundle::BundleSpec spec = given != nullptr ? *given : xt::bundle::BundleSpec{};
        out->logical_height = spec.logicalHeight;
        out->uiscale_rule = static_cast<int>(spec.uiscaleRule);
        out->safe_area_default = spec.safeAreaDefault;
        out->safe_area_console = spec.safeAreaConsole;
        out->uiscale_clamped = spec.uiscaleLimits.clamped ? 1 : 0;
        out->uiscale_clamp_min = spec.uiscaleLimits.clampMin;
        out->uiscale_clamp_max = spec.uiscaleLimits.clampMax;
        out->uiscale_dpi_floor = spec.uiscaleLimits.dpiFloor;
        return XT_STATUS_OK;
    });
}

XT_CAPI uint32_t xt_bundle_reload(xt_bundle* b) {
    XT_CAPI_GUARD_OR("xt_bundle_reload", uint32_t, 0u, {
        if (b == nullptr) {
            fail(XT_STATUS_INVALID_ARGUMENT, "xt_bundle_reload: bundle is NULL");
            return 0u;
        }
        b->changedIds = b->bundle->reload();
        return static_cast<uint32_t>(b->changedIds.size());
    });
}

XT_CAPI const char* xt_bundle_changed_ui_id(const xt_bundle* b, uint32_t index) {
    XT_CAPI_GUARD_OR("xt_bundle_changed_ui_id", const char*, "", {
        if (b == nullptr || index >= b->changedIds.size()) {
            return "";
        }
        return b->changedIds[index].c_str();
    });
}

}  // extern "C"
