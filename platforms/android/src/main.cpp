#include <jni.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/context.h"
#include "xt/flow/bundle_context.h"
#include "xt/flow/flow_runtime.h"
#include "xt/layout.h"
#include "xt/shell/app.h"
#include "xt/shell/sdl_rw_asset_resolver.h"
#include "xt/sw/sw_png.h"

// SDL2 is a real third-party dependency of xt_shell (see cmake/xt_sdl2.cmake). SDL_main.h renames
// main() to SDL_main(), which SDLActivity calls on its native thread.
#include <SDL.h>

/// XTFrame View (T-240, docs/design/23-host-integration.md §23.11.1): the Android test app. Opens
/// one directory bundle -- the APK's `assets/bundle/` by default, or `--bundle <dir>` (intent
/// extra XT_BUNDLE) -- through SDL_RWops, shows one of its UIs (`--ui <id>` / XT_UI, else the
/// first) with its flow in an xt_shell App on the SW presentation path, and feeds it the screen:
/// viewport = window size, uiscale by the manifest's spec.logicalHeight + spec.uiscaleRule, safe
/// area from the activity's WindowInsets (JNI), touch / back key / text input through App. Flow
/// host events (sound, haptic, spawn, emit) are only logged. `--stats 0` (XT_STATS=0) turns the
/// once-per-second frame-time log off.

namespace {

constexpr const char* kDefaultBundle = "bundle";

struct Options {
    std::string bundle = kDefaultBundle;
    std::string ui;
    bool stats = true;
};

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view key(argv[i]);
        const char* value = argv[i + 1];
        if (key == "--bundle") {
            options.bundle = value;
        } else if (key == "--ui") {
            options.ui = value;
        } else if (key == "--stats") {
            options.stats = std::strcmp(value, "0") != 0;
        } else {
            SDL_Log("xtframe-view: ignoring unknown argument '%s'", argv[i]);
        }
    }
    while (options.bundle.size() > 1 && options.bundle.back() == '/') {
        options.bundle.pop_back();
    }
    return options;
}

/// xt_bundle's file reads (manifest, `.xtir`, themes) through SDL_RWops: a relative path is an APK
/// asset, an absolute one a file on the device.
bool readThroughRw(const std::string& path, std::string& out, void* /*user*/) {
    std::vector<uint8_t> bytes;
    if (!xt::shell::readAllAndClose(SDL_RWFromFile(path.c_str(), "rb"), bytes)) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return true;
}

/// The activity's safe-area insets (XTFrameActivity.safeInsets(): fractions, top/right/bottom/left).
xt::Insets readSafeArea() {
    xt::Insets insets{};
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        return insets;
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, "safeInsets", "()[F");
    if (method != nullptr) {
        auto array = static_cast<jfloatArray>(env->CallObjectMethod(activity, method));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else if (array != nullptr && env->GetArrayLength(array) == 4) {
            jfloat values[4] = {};
            env->GetFloatArrayRegion(array, 0, 4, values);
            insets = xt::Insets{values[0], values[1], values[2], values[3]};
        }
        if (array != nullptr) {
            env->DeleteLocalRef(array);
        }
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return insets;
}

bool sameInsets(const xt::Insets& a, const xt::Insets& b) {
    return std::fabs(a.top - b.top) < 1e-4f && std::fabs(a.right - b.right) < 1e-4f &&
           std::fabs(a.bottom - b.bottom) < 1e-4f && std::fabs(a.left - b.left) < 1e-4f;
}

void logHostEvent(const xt::flow::FlowHostEvent& event) {
    static const char* const kKinds[] = {"sound", "haptic", "spawn", "emit"};
    const auto kind = static_cast<size_t>(event.kind);
    SDL_Log("xtframe-view: flow event %s %s ui=%s instance=%u", kind < 4 ? kKinds[kind] : "?",
            event.name.c_str(), event.uiId.c_str(), event.instanceId);
}

/// Registers the device's own Latin font first (it wins the fallback for text whose family the
/// device lacks), then the whole system font directory.
void addAndroidFonts(xt::shell::App& app) {
#ifndef XT_GL_NO_TEXT
    static const char* const kPreferred[] = {"/system/fonts/Roboto-Regular.ttf"};
    for (const char* path : kPreferred) {
        std::vector<uint8_t> bytes;
        if (xt::shell::readAllAndClose(SDL_RWFromFile(path, "rb"), bytes) && !bytes.empty()) {
            app.textShaper().addFontMemory(std::move(bytes));
        }
    }
#endif
    app.addSystemFonts();
}

int fail(const std::string& message) {
    SDL_Log("xtframe-view: %s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "XTFrame View", message.c_str(), nullptr);
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const Options options = parseOptions(argc, argv);
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // SDLK_AC_BACK -> system.back (App).

    xt::bundle::setBundleFileReader(&readThroughRw);
    std::string error;
    xt::Result<std::unique_ptr<xt::bundle::Bundle>> opened =
        xt::bundle::Bundle::open(options.bundle, xt::sw::decodeImageBytes, &error);
    if (!opened.ok()) {
        return fail("cannot open bundle '" + options.bundle +
                    "': " + (error.empty() ? std::string(xt::statusName(opened.status())) : error));
    }
    xt::bundle::Bundle& bundle = *opened.value();
    if (bundle.uis().empty()) {
        return fail("bundle '" + options.bundle + "' has no UI");
    }
    const xt::bundle::UiInfo* ui = options.ui.empty() ? &bundle.uis().front() : bundle.ui(options.ui);
    if (ui == nullptr) {
        return fail("bundle '" + options.bundle + "' has no UI '" + options.ui + "'");
    }
    SDL_Log("xtframe-view: bundle '%s' (%s), UI '%s' %gx%g fit=%s", std::string(bundle.id()).c_str(),
            options.bundle.c_str(), ui->id.c_str(), static_cast<double>(ui->size.x),
            static_cast<double>(ui->size.y), ui->fit.c_str());

    // A landscape design turns the screen; the manifest default is portrait.
    const bool landscape = ui->size.x > ui->size.y;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, landscape ? "LandscapeLeft LandscapeRight" : "Portrait");

    xt::shell::WindowConfig config;
    config.title = "XTFrame View";
    config.fullscreen = true;
    config.msaa = 0;
    xt::shell::FitMode fit = xt::shell::FitMode::Native;
    if (xt::shell::parseFitMode(ui->fit, fit) && fit != xt::shell::FitMode::Native) {
        config.fit = fit;
        config.designWidth = static_cast<int>(ui->size.x);
        config.designHeight = static_cast<int>(ui->size.y);
    }
    xt::shell::App app(config);
    if (!app.ok()) {
        return fail(std::string("cannot start: ") + app.lastError());
    }
    app.setStatsLog(options.stats);

    // Assets through SDL_RWops, with the manifest's atlas cells / nine-slices / @2x variants.
    app.setAssetRoot(options.bundle);
    for (const xt::bundle::AssetInfo& info : bundle.assetTable()) {
        if (const xt::AssetResolver::ImageMeta* meta = bundle.assets().imageMeta(info.file)) {
            app.assetResolver().setImageMeta(info.file, *meta);
        }
    }
    for (const xt::bundle::AtlasInfo& atlas : bundle.atlases()) {
        for (const xt::bundle::AtlasEntryInfo& entry : atlas.entries) {
            const std::string name = std::string(xt::kAtlasAssetScheme) + atlas.name + "#" + entry.asset;
            if (const xt::AssetResolver::ImageMeta* meta = bundle.assets().imageMeta(name)) {
                app.assetResolver().setImageMeta(name, *meta);
            }
        }
    }
    addAndroidFonts(app);

    xt::Context& ctx = app.context();
    if (xt::bundle::loadThemes(ctx, bundle) != xt::Status::kOk) {
        SDL_Log("xtframe-view: a theme file could not be read");
    }
    xt::Result<xt::DocumentHandle> loaded = xt::bundle::loadUi(ctx, bundle, ui->id);
    if (!loaded.ok()) {
        return fail("cannot load UI '" + ui->id + "': " + xt::statusName(loaded.status()));
    }
    const xt::DocumentHandle handle = loaded.value();
    app.setCurrentDocument(handle);

    // Device: a touch handheld; uiscale from the manifest spec; safe area from the window insets.
    if (const xt::bundle::BundleSpec* spec = bundle.spec()) {
        const xt::shell::UiScaleEdge edge =
            spec->uiscaleRule == xt::bundle::UiScaleRule::Width       ? xt::shell::UiScaleEdge::Width
            : spec->uiscaleRule == xt::bundle::UiScaleRule::ShortEdge ? xt::shell::UiScaleEdge::ShortEdge
            : spec->uiscaleRule == xt::bundle::UiScaleRule::Fit       ? xt::shell::UiScaleEdge::Fit
                                                                      : xt::shell::UiScaleEdge::Height;
        // T-345 (25 §25.4.2 item 1): the project's clamp and DPI floor.
        (void)app.context().setUiScaleLimits(spec->uiscaleLimits.clampMin, spec->uiscaleLimits.clampMax,
                                             spec->uiscaleLimits.dpiFloor);
        app.setLogicalHeight(spec->logicalHeight, edge);
    } else {
        app.syncViewport();
    }
    xt::DeviceInfo device;
    device.platform = xt::DeviceKind::Handheld;
    device.input = xt::InputKind::Touch;
    device.dpr = app.devicePixelRatio();
    device.safeArea = readSafeArea();
    ctx.setDevice(device);
    ctx.setInputDevice(xt::InputKind::Touch);

    xt::flow::BundleContext flows(ctx);
    flows.setHostEventListener(logHostEvent);
    flows.addUi(ui->id, handle);
    if (!ui->flow.empty()) {
        std::string json;
        if (readThroughRw(options.bundle + "/" + ui->flow, json, nullptr)) {
            std::vector<std::string> errors;
            if (flows.loadUiFlow(ui->id, json, errors) != xt::Status::kOk) {
                for (const std::string& e : errors) {
                    SDL_Log("xtframe-view: flow (%s): %s", ui->id.c_str(), e.c_str());
                }
            }
        }
    }

    // App lifecycle -> system.paused / system.resumed; a rotation or resize re-reads the insets.
    int insetChecks = 0;
    app.setEventHook([&](const SDL_Event& event) {
        if (event.type == SDL_APP_WILLENTERBACKGROUND) {
            ctx.postSystemEvent("paused");
            return true;
        }
        if (event.type == SDL_APP_DIDENTERFOREGROUND) {
            ctx.postSystemEvent("resumed");
            return true;
        }
        if ((event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) ||
            event.type == SDL_DISPLAYEVENT) {
            insetChecks = 30;  // The new insets arrive with the next layout pass; poll a while.
        }
        return false;
    });

    uint32_t frame = 0;
    const int exitCode = app.run([&](float dt) {
        flows.tick(dt);
        // The insets: every frame for a while after a change, else twice a second.
        if (insetChecks > 0 || frame % 30u == 0u) {
            if (insetChecks > 0) {
                --insetChecks;
            }
            const xt::Insets safe = readSafeArea();
            if (!sameInsets(safe, device.safeArea)) {
                device.safeArea = safe;
                device.dpr = app.devicePixelRatio();
                ctx.setDevice(device);
                SDL_Log("xtframe-view: safe area %.3f %.3f %.3f %.3f", static_cast<double>(safe.top),
                        static_cast<double>(safe.right), static_cast<double>(safe.bottom),
                        static_cast<double>(safe.left));
            }
        }
        ++frame;
    });
    xt::bundle::setBundleFileReader(nullptr);
    return exitCode;
}
