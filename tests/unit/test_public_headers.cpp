#include "doctest.h"
#include "xt/xt.h"

/// The <xt/xt.h> umbrella: a host must get every core row of the stable interface list from this
/// one include. This translation unit deliberately includes nothing else from xt (doctest aside),
/// so it stops compiling the moment the umbrella stops covering the list. See
/// docs/design/23-host-integration.md §23.6, docs/tasks/T-100.md.

TEST_CASE("public header umbrella covers the 23.6 core surface") {
    // xt/version.h
    REQUIRE(xt::versionString() != nullptr);

    // xt/text_shaper.h, xt/asset_resolver.h, xt/context.h
    xt::NullTextShaper shaper;
    xt::FileAssetResolver assets(".", nullptr);
    xt::ContextConfig config;
    config.shaper = &shaper;
    config.assets = &assets;
    xt::Context ctx(config);
    ctx.setViewport(xt::Rect{0.0f, 0.0f, 64.0f, 32.0f}, 1.0f);

    // xt/document.h (DocumentHandle, Document), xt/element_ref.h
    const xt::DocumentHandle handle = 0;
    const xt::Document* doc = ctx.document(handle);
    CHECK(doc == nullptr);
    const xt::ElementRef ref;
    CHECK_FALSE(ref.valid());

    // xt/input.h
    xt::InputEvent event;
    event.kind = xt::InputEvent::PointerDown;
    event.button = xt::PointerButton::Left;
    event.key = xt::KeyCode::Unknown;
    ctx.pushInput(event);
    CHECK(ctx.pendingInputCount() == 1);
    CHECK(ctx.cursor() == xt::Cursor::Default);

    // xt/command_list.h, xt/damage.h
    ctx.update(0.0f);
    xt::CommandList list;
    xt::DamageList damage;
    ctx.render(list, &damage);
    CHECK(list.commands.empty());
    CHECK(damage.rects.empty());

    // xt/asset_resolver.h (ImageData), xt/perf.h
    const xt::ImageData image;
    CHECK(image.width == 0);
    const xt::PerfTracker& perf = ctx.perf();
    CHECK(perf.history().size() <= xt::PerfTracker::kHistoryCapacity);
    const xt::FrameStats& frame = perf.current();
    CHECK(frame.frameIndex == 0u);
}
