# Changelog (run time and host integration)

An excerpt of the XTFrame changelog: for each release, the behaviour changes and the host
integration notes. Version numbers are those of the XTFrame release the snapshot was
exported from.

## [0.21.0] - 2026-11-26

### Behaviour changes
- History (36 §36.4, T-883 #816): the GUI editor turns project history (built-in version control)
  on by default -- a new project gets `.xtvc/` and the checkpoint "Start" unless the wizard's box is
  cleared, and opening a project without `.xtvc/` turns it on after the open, with the Messages line
  "History is now on for this project." and a Turn off action. It is not turned on when the
  directory cannot be written, the project's sidecar says off, or the preference is off; headless
  and Editor Service opens never turn it on (`history.*` still answers `kUnsupported` there).
  Turning it off never deletes `.xtvc/`; turning it on again reuses it.
- The flow `clock` node and `#document.clock` accumulate in double: each `tick(float dt)` adds the
  float `dt`, widened to double, to a double sum (it was a float sum), so an external flow executor
  that must match tick for tick has to do the same (53 §53.4.5). This shipped in 0.20.0 and was
  added to its list on 2026-11-25; there is no code change in 0.21.0.
- `.xtfx` (18 §18.7, T-892 #827): a parameter or local variable named `xtU`, `xtLayerTex`,
  `xtLayerSmp`, `XtCustomBlock` or `texture2D` (names of the wgpu wrapper) is now a compile error
  (`identifier '<name>' is reserved in the translation; rename it`, for a parameter `parameter name
  '<name>' is reserved in the translation`); it compiled before and then never ran on wgpu. The
  translation output, the wrapper text and the custom-effect cache keys are unchanged.
- Scenario budgets (59 §59.3, T-890 #826): `shader` and `effects` allow 8 screenshots / 24 video
  frames, `add-animation` and `create` 4 / 24 (all were 2 / 6).
- `scene.screenshot` (T-879 #815, T-885 #817, T-887 #823): now the alias of `view.capture
  {renderer: "sw"}` -- a single shot is redrawn in software in a scratch scene instead of borrowing
  the live preview, with a `stamp` in the reply; its arguments and fields are unchanged and
  `timelineScope` works as before. The scratch scene's document clock starts at 0, so a shot taken
  while a CSS animation is running shows the animation at clock 0.
- `xtframe-view` (T-880 #814, T-882 #819, T-887 #823): a windowed run reads connected game
  controllers by default; `--gamepad` is accepted and does nothing; without `--players` one player
  slot is opened (with `--gamepad` and no `--players` it was 8). Every `--screenshot` prints an
  extra `stamp:` line on stdout; the image files are bit-identical without the new flags.
  `--frames` is still only a frame count.
- Game controllers' sticks and triggers pass the dead zones before they reach `pad.*`, so a stick
  at rest gives exactly 0 and `needsFrame()` turns false; `--event axis` values and host-pushed
  InputEvents are not filtered (T-884 #818).

### Host integration
- A host can give `view.capture` a GPU renderer: implement `ICaptureRenderer` and return it from
  `ServiceHost::captureRenderer()` (a new virtual with a default; hosts that derive from
  `ServiceHost` recompile), or pass a factory to `FileServiceHost::setCaptureRendererFactory()` /
  `runServiceCli(..., captureFactory)`. `xt_editor_service`, `xt_editor_cli` and `xtframe-service`
  must not link a GPU backend (`check_layering.sh` rule 7) (#824, #826).
- Mirrored headers since 0.20.0: `gpu_frame.h` (`CustomEffectParams` 176 B), `command_list.h`
  (`ShaderRef` 188 B, `CommandList` 800 B), `input.h` (`InputEvent` 128 B), `xtframe.h` and
  `host_preview.h` are unchanged. `gpu_device.h`: `GpuDevice::customEffectFailure(glsl)` appended (a
  virtual with a default empty result; a host device returns its compile message to fill
  `effects[].reason`). `frame_builder.h`: `FrameBuilder::EffectReport` (64 B, Windows x64, with
  `failure`), `setEffectReport()`, `effectFailureReason()`; `FrameBuilder` itself is unchanged (8
  B, pimpl). Renderers: `SwRenderer::setEffectReport()` (`SwEffectReport`),
  `GlRenderer::setEffectReport()` / `deviceName()`, `WgpuRenderer::setEffectReport()`.
- A host's own shader wrapper around `.xtfx` bodies must avoid the reserved names in
  `xt/xtfx_reserved.h`; the compiler now rejects them in user code (#827).
- Backends: `PlatformSdl2::setEventHook(std::function<bool(const SDL_Event&, Context&)>)` sees every
  event before translation (true consumes); `PlatformSdl2` grows from 72 to 136 B (Windows x64)
  (T-884, #818). `RendererSw::renderer()` and a const `RendererGl3::renderer()` (#819).
- `xt_shell`: `WindowConfig` gains `gamepad`, `gamepadPlayers` and the three dead zones; a host that
  embeds `xt_shell::App` and handles controllers itself sets `gamepad = false` (#814, #818).

## [0.20.0] - 2026-11-22

### Behaviour changes
- CSS animations (T-872, #807): a finite `animation-iteration-count` above 1 repeats (it played once) and a finite `alternate` runs back; `reverse` / `alternate-reverse` with a finite count keep the delay and end on the last pass's end (`reverse 1` ends on 0%) and finish; `reverse infinite` is reversed. A fractional count is still truncated (2.5 plays 2). `@timeline` play modes are unchanged.
- `@keyframes` without `from`, `to` or both (12 §12.3, T-869, #806): the missing end interpolates toward the property's value at that moment -- a lower layer's write in this frame, the cascade value for opacity and transform components, else the node's own value tracked per instance, following a class switch mid-animation -- as CSS does; before, the element froze at the start value. A CSS animation that ends without a `forwards` fill puts opacity and the transform components back to their cascade values.
- Effect keyframes (T-869 #806, T-872 #807): a stop that does not declare `effect` is no longer a compile error, it is simply not an effect keyframe; sequences that differ by a prefix or `none` are padded with each effect's neutral parameters (18 §18.3.1), so `none` -> `blur(8px)` grows the radius from 0; other different sequences (kind or order, lists diverging after a shorter one, or `backdrop-blur`, `gradient-mask`, `custom()` needing padding) are a warning and stay static instead of a compile error.
- Composed transforms (T-869, #806): `%`, `em` and `rem` translates are accepted in sequences that need matrix composition when no rotation or skew comes before them and each axis ends in one unit (`translate(-50%, -50%) scale(3) rotate(-14deg)`); after a rotation or skew they stay a compile error.
- Clock (59 §59.7, T-870 #808): the Context clock, the Document clocks, animation start clocks and paused positions are kept and accumulated in double (a 2 s animation's phase error at 3600.5 s: 0.795 s -> under 1 us). `xtTime` (`CommandList::time`) is the clock modulo 3600 s, taken in double and then rounded, so it wraps to 0 once an hour. A Document loaded through `Context::loadDocument()` starts from the Context's current clock (before, a late-loaded document's first-frame timelines jumped through the whole clock), and `<anim autoplay>` starts at the load clock (T-874, #811).
- uiscale != 1 (T-863 #799, T-864 #800): px tracks of `grid-template-columns` / `-rows` are multiplied by the factor like other px lengths; an element under `setWorldAnchor` lands exactly on the given screen point; a `spawn()` instance's `at` position and the spacers of a virtualized `<repeat>` are written in design px (they were scaled twice), and the row height is measured again when the `<repeat>`'s factor changes at run time. At uiscale 1 nothing changes.
- Inline `<svg>` and `<img src=*.svg>` with a `viewBox` origin other than 0 map (min-x, min-y) to the box's top-left corner, as SVG does (the content was offset) (T-867, #803).
- Flow: an edge from an `out` port with a declared default reads that default at load, rebind and `bindUi`, not 0 until the first `set` (T-867, #803).
- Hit testing: an element hidden by an animated `visibility: hidden` and its subtree are no longer hit, matching the picture (T-867, #803).
- Triggers: `gesture(pinch)` and `scroll` carry an `event.value` (the scale; the scroll position 0..1), which was always 0 (#805).
- Gamepad: the right stick and both triggers, dropped by core so far, reach `pad.rx|ry|lt|rt`; navigation, focus and scrolling are unchanged (#810).
- Importer (re-importing a board changes its output):
  - Local stylesheets linked with `<link rel="stylesheet">` are read and concatenated with the `<helmet>` styles in the browser's cascade order (`@import` up to depth 8 with cycle detection, `url()` relative to the stylesheet, `media`, `:root` variables, `@keyframes` as inline styles); a remote or unreadable one is a warning naming the path, `media` other than `all` / `screen` is skipped with an info line. Boards that link stylesheets can change a lot -- rules that were dropped silently now arrive, with their findings (T-868, #804).
  - `effect: custom('<path>')` resolves against the board's directory; the `.xtfx` is copied to the project's `shared/fx/` (shared by content, `-2` on a name clash, with a `.meta`) and the reference becomes `../shared/fx/<name>.xtfx` (T-868, #804).
  - `var(--x)` resolves against tokens.json first, then the board's own top-level `:root` / `html` custom properties, substituted recursively (cycles, 32-level chains and values over 16384 characters are reported); those declarations are no longer dropped. A custom property set on another selector, or inside `@media`, is dropped with a finding (T-866, #802).
  - `margin`, `padding`, `inset`, `gap`, `border-width` and `border-radius` holding `calc()` / `min()` / `max()` / `clamp()` are split into longhands instead of dropped (T-866, #802).
  - `html` / `:root` `font-size` becomes `xt { font-size }`, so `rem` matches the web page (`%` / `em` against 16px, `rem` as a multiple of 16px; other units warn) (T-864, #800); `rem` in `box-shadow`, `text-shadow`, `filter` and `backdrop-filter` becomes px at that root size, and `background-position` / `-size` accept `rem` (T-866, #802).
  - `vw` / `vh` / `vmin` / `vmax` in `transform`, `translate`, shadows and filters, and `Q` everywhere, are dropped by the importer with the unit, the property and a suggestion, instead of reaching the compiler as `invalid value` (T-866, #802).
  - The no-break space at the edge of a mixed-content text run is written as U+00A0 (`C2 A0`); it was the mojibake `C3 82 C2 A0` (`Â ` before bold text) since T-113 (T-869, #806).
- Diagnostic source label: a node hidden by a `wheel` or `key(...).release` trigger reports `trigger on:wheel` / `trigger on:key.release` instead of the generic `trigger` (T-875, #809).
- (Added to this list on 2026-11-25; the behaviour shipped in 0.20.0.) The flow `clock` node and `#document.clock` accumulate in double: each `tick(float dt)` adds the float `dt`, widened to double, to a double sum (it was a float sum). A flow executor outside the runtime that must match tick for tick has to do the same; a float sum differs by one tick at `%.1f` rounding boundaries (at 60 Hz, tick 15 formats as "0.3" where the float sum gave "0.2"). `timer` (float, reset on every fire) and `throttle` (already double) are unchanged. Docs/design/53-flow-language.md §53.4.5.

### Host integration
- Clock (23, upgrade note of T-870): `Context::update(float)`, `Context::clock()` (now the double clock rounded to float), `Document::update(float)` and the C ABI keep their signatures; `Context::clockSeconds()` gives the exact clock and `Context::setClock(double)` sets it without stepping. Hosts that use `Animator` / `StyleEngine` directly: their `clock` parameters and `Transition::startClock` are `double` now (source compatible, recompile). A host shader wrapper that accumulates its own time should use `CommandList::time` (#808).
- `CommandList::time` is the Context clock modulo `kEffectTimePeriod` (3600 s), taken in double and rounded. `CommandList::clockSeconds` (new, double, not serialized) is the unwrapped clock; each custom effect's `xtTime` is `customEffectTime(list.clockSeconds, ref.timePeriod)` (`command_list.h`), which the built-in backends write into `CustomEffectParams::time`. A host that wraps custom effects from the CommandList itself should upload that value; keeping `CommandList::time` behaves as before, with every shader on the default 3600 s period (the `.xtfx` `time:` header does not apply there) (23, upgrade note of T-874) (#811).
- Mirrored structures: `CustomEffectParams` is unchanged (176 B, every offset the same; a `static_assert` now pins it); `ShaderRef::_pad` is renamed `timePeriod` (same size, 188 B); `CommandList` gains `double clockSeconds` after `time` (792 -> 800 B, Windows x64); `InputEvent` gains the tail fields `channel`, `channelValue`, `channelStrength`, `channelFlags` and the appended kind `Channel` (112 -> 128 B; the existing fields keep their offsets). `host_preview.h` is unchanged.
- Input channels: push a host or device quantity with `Context::inputChannel(name)` plus an `InputEvent::Channel`, or the C ABI `xt_input_channel` / `xt_surface_push_channel`; read it back with `channelValue()` / `xt_surface_channel_value`. The existing input events already move `pad.*`, `key.*`, `wheel.*` and `pointer.*`; every Document of a Context sees every channel, whichever surface has the focus (60 §60.11) (#810).
- `Document::load()` takes an optional `startClock` (default 0, existing callers unchanged); `Context::loadDocument()` passes the Context clock (#811).

## [0.19.3] - 2026-11-22

### Behaviour changes
- Fonts: within a family the face is chosen by width first (the request is normal width, 100%), then italic, then weight (CSS Fonts 4 order). Families whose typographic family also holds narrow or condensed faces -- `"Arial"`, `"Myriad Pro"`, `"Bodoni MT"`, `"Goudy Old Style"`, `"Minion Pro"` -- now draw their normal-width face instead of a narrow one (measured: `"Arial"` 264 -> 322 px, `"Myriad Pro"` 224 -> 306 px for the same text). Legacy per-style family names (nameID 1: `"Arial Narrow"`, `"Segoe UI Semibold"`, `"Sitka Text"`) match when no face has that typographic family, instead of falling back. A `wdth` axis that holds 100% but does not default to it is set to 100%. Machines with only the narrow face installed are unchanged; `font-stretch` is still not supported (#790).
- Trigger editing (the trigger panel, state bar, timeline markers and the Editor Service's trigger ops) accepts `remove` / `remove()`, as the compiler does, and now rejects, with the compiler's message, five forms it let through although the compile fails: a quoted `state(..., target='A')`, a quoted port in `set('p', ...)`, an empty converter (`set(s, value |)`, `event.value |`), `spawn('', ...)` and `spawn(..., lifetime=0s|fast)`. What the compiler accepts and the IR it writes are unchanged (#780).
- Software renderer: bilinear and trilinear image filtering run on premultiplied texels, as the GPU backends filter their textures, so a translucent image edge no longer picks up the colour stored in neighbouring transparent texels (G-B up to 64 before). Point sampling and opaque images are unchanged; BC / DDS sources and host pixels flagged `straightAlpha` keep straight filtering. The SW cache keeps a premultiplied level 0 per non-BC image (w x h x 4 more bytes) (#773).
- Runtime: the values of `draggable`, `drop-target`, `autofocus`, `focus-trap` and `key-activation` are trimmed of surrounding white space before they are read, as the compiler does (`draggable=" false "` is no longer a drag source; `" any-button "` and `" axis-x "` work) (#778). `Document::appendChild` / `insertBefore` with a pooled `<repeat>` instance as parent or child do nothing (a pooled row attached elsewhere crashed on reuse) (#783).
- Bundle writes on Windows: `writeFile()` and `zipDirectory()`'s final move retry for up to about 0.25 s when another process briefly holds the target, then fail as before (#778, #783).
- Trigger panel: the event list holds every event the compiler accepts (21 more: `pointermove`, `pointercancel`, `pointerdrag`, `scroll`, `outsidepress`, `focusin`, `focusout`, `change`, `input`, `drag.leave`, `drag.cancel`, `drag.value`, `back`, `nav.value`, `nav.blocked`, `nav.blocked(n)` and the button filters of `press` / `release` / `click` / `dblclick` / `pointermove`) (#779); the event list and the action dropdown are built from the compiler's tables, which adds `reset` and `consume` to the actions (#780).
- Interaction panel: `draggable="any-button"` shows as checked and is kept when toggled; `draggable="axis-x"` shows unchecked and the box is disabled. The property panel's `text-rendering` dropdown lists `auto` and `geometricPrecision` once each. `repair.passthrough` accepts `autofocus` / `focus-trap` values with surrounding white space (#776).

### Host integration
- `HostPreviewDevice::setCommandList(const CommandList*)` (`xt/editor_qt/host_preview.h`, appended, default empty: existing hosts compile and behave unchanged). The host preview calls it with the frame's list just before each `FrameBuilder::build()` (a Demo frame with a scene layer: once for the scene's list, once for the UI's) and with nullptr after `endFrame()`; the pointer is valid only in between. To run custom shaders in the host pane, override it and give the list to your `GpuDevice`, so `prepareCustomEffect` finds the effect's source in the host's shading language in `CommandList::shaders` / `shaderText` (`ShaderRef::sourceOffset` / `sourceLength`). In frames the host drives itself, set the command list before every build entry (including warm-up and read-back); without it a custom effect looks as if it had no source -- do not cache "no source" as a permanent failure (23, upgrade note of T-859) (#795).
- `gpu_frame.h`, `command_list.h` and the C API header are unchanged; no new C ABI function.

The full gate (local_ci) was not run for this release; each change passed dev_check --changed, and the portability leg (clang, WSL GCC, fuzz smoke) passed on 8ed52fd4.
Not tested: the GPU backends against the new goldens (`ui_image_alpha_edge`) beyond each change's own run, macOS builds, and the host pane in a real host (the `setCommandList` path is covered by a fake host only).

## [0.19.2] - 2026-11-21

### Behaviour changes
- Import: `filter: brightness()` is converted to look as on the web. The `brightness()` and `contrast()` of one `filter` are composed in the order written and written as an equivalent pair, `contrast(s - 1) brightness(0.5 + (o - 0.5) / s)`, inside the merged `color-adjust()` (`brightness(0.4)` becomes `contrast(-0.6) brightness(-0.75)`); between `brightness(1/3)` and `brightness(2)` the picture matches the web with no `effect.degraded` finding, and `brightness(1)` also writes the (identity) pair. `brightness(0)` is black with no finding; below 1/3 a new finding says dark tones turn black sooner than on the web and suggests a black overlay; above 2 it is clamped to `brightness(2)` with the existing finding. A `filter` that also holds `invert()`, `sepia()` or `opacity()` keeps the old additive conversion with its note, and `backdrop-filter: brightness()` is still additive. Re-importing an existing board makes such elements lighter or darker, as on the web (#763).
- SVG linear gradients under `skewX` / `skewY`, a non-uniform scale not aligned with the gradient, or a diagonal `objectBoundingBox` gradient on a non-square shape draw slanted stripes as browsers do (the other diagonal's corners are t = 0.5); a singular gradient-to-user-space map keeps the old mapping and warns (#767).
- SVG gradients with `objectBoundingBox` and a `gradientTransform` apply the transform in the box's unit space, then the box (SVG's order; it was box first, then transform): linear (#767) and radial -- a radial gradient's centre moves to where SVG puts it, its radius is unchanged (#769).
- `xtframe-view`: a UI's automatically loaded flow (the Bundle manifest's `flow`, or the `.xtflow` next to a `.xt`) that does not load prints `xtframe-view: flow: <ui>: <problem>` and the UI is shown without it, instead of exiting 2; a flow given with `--flow` that does not load still exits 2 (#768).
- `@keyframes` that differ in `custom()` effect parameters animate them (float, vec2 and color parameters, one track each, as the same animation on a `@timeline`); they compiled to no track before, with no diagnostic (#765).
- Custom effects on a scene node (a shape, path or group inside an `<svg>`) read `XtInput.local` and `XtInput.boxSize` from the node's geometry box in its local space without the stroke (a group: the union of its visible descendants'); they read 0 before. Elements are unchanged; a node without geometry still reads 0 (#770).

### Host integration
- No new C ABI function and no interface change. Custom `GpuDevice` backends: the existing `XtInput.local` / `boxSize` fields of `CustomEffectParams` are now filled for scene nodes too (a box whose origin need not be 0,0), so a wrapper that supports them gets non-zero values there (#770).

The full gate (local_ci) was not run for this release; each change passed dev_check --changed.
Not tested: the GPU backends against the new goldens (`ui_svg_gradient_skew`, `ui_svg_gradient_radial_xf`) beyond each change's own run, and Linux / macOS builds.

## [0.19.1] - 2026-11-21

### Behaviour changes
- Responsive: with a `DeviceInfo.dpr` other than 1 (C ABI hosts, Android), `ui-scale: none` is factor 1 (it was the dpr) and `spec.uiscaleDpiFloor` is no longer multiplied by the dpr; nothing changes at dpr 1 (the editor, `xtframe-view`, goldens) (#756).
- Assets: `layer.create` / `layer.setAttrs` refuse a `src` / `href` / `texture` that names no file in the project (`kNotFound`, with the resolved path); the lint reports a missing image or font as `asset.missing` instead of `compile.other` (#755).
- Asset ids: lowercase ASCII letters, digits, `-`, `_` and `.` (not first or last) and non-ASCII characters; `asset.import` without an `id` keeps a file stem that already follows the rule (`bg_01_room` is no longer renamed `bg-01-room`), and a refused `id` comes back with a suggestion; asset hygiene's `badName` and the asset workbench use the same rule (#755, #757).
- Keyframe, marker and cue times are written on the millisecond grid (every write rounds to 0.001 s, as the `.xt` stores them); two keys of a track that fall on the same millisecond: the later write wins (#757).
- `timeline.stagger` replies `layers[].delayMs` in milliseconds instead of `layers[].delay` in seconds (#759).
- `style.setRule` on a source format 2 document writes geometry declarations to `<layout>` and the others to `<style>` in one undo step (it wrote one rule and failed to compile); `renameRule` / `deleteRule` act on both sections (#757).
- Scenario contracts: `catalog.add` / `catalog.list` join `create`, `effects`, `restyle`, `single-element`; `effects` allows `layer.setAttrs`; `repair-import` allows `repair.dedup` / `repair.effectBase` / `repair.passthrough` and `diagnostics.fix` / `diagnostics.accept`; `create` allows `diagnostics.*`, `timeline.removeTrack` and `shader.*`; `shader` allows `catalog.add` and `shader.*` (#757, #759).
- Scenario scope: a scope taken from the selection is always stated in the reply's `notes`, `root: "*"` asks for the whole document, and under a subtree scope an op the contract lists is allowed -- Global ones (#757) and, since T-819, writes that belong to no document (`shader.create`, `shader.setSource`) (T-819).
- Error replies carry the document's `revision`, and some carry structured fields (`errors`, `references`) (#759).
- `shine()` `position` takes -1..2 (it was 0..1); outside that range it is clamped with a warning (#754).
- The built-in `starter.xtfx` shader tints with the layer-local coordinate (#754).
- Images in a format the runtime cannot decode (WebP, AVIF, HEIF/HEIC, JPEG XL, TIFF, QOI; told by the file's leading bytes) are reported: the import copies the file and adds `asset.undecodable` (a warning in `import-report.json` and the diagnostics), `xtframe-compile` warns `image asset "<path>": format <fmt> cannot be decoded (supported: ...)` and fails under `--strict`, the editor's lint reports `asset.undecodable`, and `asset.import` adds the message to `notes`; the supported formats are PNG, JPEG, GIF, BMP, TGA, PSD, HDR, PIC, PNM and DDS (#760).
- The import's `brightness()` finding (`effect.degraded`, still a warning) states the additive value, the consequence and the remedy (`brightness(0.4) is multiplicative in CSS but additive here (becomes -0.60): ...`); the conversion is unchanged (#760).
- SVG gradient features the renderer ignores now warn (a failure under `--strict`): `fx` / `fy`, `spreadMethod` `reflect` / `repeat`, a radial `gradientTransform` with non-uniform scale or skew, `objectBoundingBox` on a non-square shape, and a `<pattern>` fill (painted black); a missing gradient id says it must be defined inside the same `<svg>` (#761).

### Host integration
- Custom `GpuDevice` backends: `CustomEffectParams` gains trailing fields (`localXf[6]`, `boxW`, `boxH`) for `XtInput.local` / `boxSize`; a backend that reads only the old fields keeps working (its shaders read 0); host HLSL wrappers fill them when they support the new inputs (#754).
- C ABI and other hosts passing a dpr other than 1: see the `ui-scale: none` / DPI floor change above (#756).

## [0.19.0] - 2026-11-20

### Behaviour changes
- IR 3.3: `animation-*` and `transition-*` longhands of rules that do not imply each other merge as in CSS (specificity, source order, `!important`, states, `@media`) at run time, and whole-value `var()` works in them; a 3.2 runtime ignores the new declarations and plays no CSS animation or transition of a 3.3 file (#730). IR files were stamped 3.2 by the TEXP programs before that (#705).
- CSS: the `animation-*` longhand properties, ignored before, now take effect, so documents that wrote them change their animation timing (#696).
- CSS: an unquoted `url(...)` (with `engine://` or a `/` path) is valid like a quoted one; the `border-image-source: url(...)` longhand is accepted (it was dropped with a warning); an `engine://` image used as `background-image` / `border-image-source` has an intrinsic size (the bound width and height) (#749).
- Bindings: number parsing no longer depends on the locale, and hexadecimal text (`0x...`) is no longer a number (`"0x1A"` reads as 0) (#737).
- Text: hard line-break characters (LF, CR, VT, FF, NEL, U+2028, U+2029) draw no glyph and have zero width, a TAB draws as one space, a line ending in a hard break no longer counts it in its width (centred and right-aligned lines move), and trailing spaces are now trimmed from the line width with real fonts, so a centred or right-aligned wrapped line that ended in a space moves by half a space or a space (the symbols golden `sym_Stamp_normal` was refreshed for it) (#742).
- 3D: a flat node with grouping properties (opacity, effects, mask, blend, clip) inside a 3D space flattens its 3D descendants -- the group is one item at its own plane depth and its opacity / effects apply to the whole subtree (#740); images, image quads and meshes under nested projections are perspective-correct on GL / wgpu, as on SW (#733); a 3D space root not at the origin draws its own box and clips its children at its own position (was: twice its offset), and damage inside 3D spaces covers moved layers and projected positions (#731).
- Off-screen culling: a layer or effect whose subtree holds a 3D space root is left out when all its reach is off-screen; layers on affine planes whose own box is off-screen use the union with their descendants' reach as their window, and SW now draws such content as the GPU backends do (it drew nothing before) (#724, #725).
- Animations: an instance that has ended and holds with `forwards` no longer sends `AnimationIteration` every period (#734); a static Document no longer paints every frame (`setStaticSkipEnabled(false)` or `XT_STATIC_SKIP=0` restores the old behaviour) (#702); paused timeline members, paused `<anim sync>` clips and CSS animations are no longer evaluated while nothing changes, and seeking to the current position no longer rewrites values (#723).
- Timelines: pause / stop / seek / play(from) triggered by a timeline's own marker or end take effect at the marker time within the step that crosses it; guarded self-control actions, markers skipped by a seek and `<repeat>` instances at their end follow the same rule (#722, #723).
- Effect-parameter transitions are no longer restarted by a re-resolve that keeps their target, and a target change at t = 0 is no longer swallowed (#732).
- State groups are scoped to their symbol: a cross-symbol state-group conflict that failed to compile now compiles as a symbol-local group (the IR gains an `STGS` block that older runtimes skip) (#694).
- Symbols: a symbol library file under `shared/symbols/<dir>/` names its symbols `<dir>/<name>` (#715); a symbol dragged in from a library beside a same-named project symbol of another source is placed in `shared/symbols/<group id>/` and used by its qualified name instead of being skipped or silently replaced (#717, #719); "older than the library" now compares only the recorded source and sha256 (an edited copy shows Modified), and a project symbol without a source record is shown as Project (#713); projects that link two or more symbol groups or applied a style take their implicit token layer from the project style, and symbol lint gains `token.core` (error) and `token.role` (warning) (#720).
- Publishing: `xtframe-pack`, the editor's Publish and the Editor Service `project.publish` compress images with `auto` (BC7 DDS for images at least 256 px on both sides) when the `.xtui` has no `publish.images.compress`, and saving a `.xtui` no longer writes `"compress": "none"`; write `"compress": "none"` or pass `--compress none` to keep the old output. `xt::bundle::pack()` with default options, the automatic pack of `xtframe-view` and the bundle browser's preview pack stay uncompressed (#750, #752).
- Re-import: a UI whose board did not change is not merged and none of its files is written, and a file whose bytes would not change is never written (also with `--force`); merged documents keep `<body>` comments and the original bytes of unchanged parts; `WriteResult::uisWritten` and the service's `import.reimport` `written` count only UIs actually written; `import-report.json` gains `uis[].freshHash`; `diagnostics.json` records the dump path relative to the project, also from the Editor Service (#743, #744).
- Re-import: with `--prune` the stop-loss does not count removed nodes, and without it the stop-loss message suggests `--prune`; with `--prune` and no hand edits a re-import equals a fresh import; v2 `<symbol>` definitions merge three-way (board changes, additions and deletions of unedited definitions apply; edited ones are kept with a `symbol` conflict); kept missing nodes lose `bind:` / `on:` / `port:` attributes whose port the board removed (`reimport[].removedBindings`); an unedited generated `.xtflow` follows the board; UIs no longer on the canvas are listed (`uisNotOnCanvas`) and kept (#744).
- Re-import: `--symbols-dir` library files are no longer kept just because they exist -- unedited ones follow the board, ones the board deleted go with their link, and edited ones are kept with a conflict; without a manifest, imported assets nothing references any more are listed (`unreferencedAssets`) and `--prune` deletes them with their `.meta`; a symbol definition still used by a kept `<use>` stays (`reimport[].keptSymbols`) (#745); `--prune` merges a UI whose board is unchanged when it still has something to clear (#746).
- The import no longer leaves `xt-import-dc-*` directories in the temp directory (the name is unique per run; `ImportResult` gains a `scratch` member), and the other tools remove the temp directories they create (#746).
- The `.xtb` unpack cache keeps at most 8 entries / 256 MB, least recently used first, and marks finished unpacks with a `.xtframe-unpacked` file; unpack directories from before 0.19.0 have no marker and are unpacked again once (#748); unmarked directories modified within the last 10 minutes are not evicted (#749).
- External textures: `IExternalTexture::bindTexture` keeps a straight-alpha flag set on the same name (#751); `setExternalPixels` clears a planar description and `setExternalTexturePlanes` clears CPU pixels (#747); a binding size set directly on the resolver lays its users out at the next update (#735).
- wgpu window surfaces request `TEXTURE_BINDING` when offered; without it each frame is drawn into an intermediate texture and presented, one extra submit (#728).
- Measurement output: `xtframe-view --perf-csv` and `xtframe-gpu-probe --timing-csv` write `FrameRecord` v1 columns plus `host.*` columns (the old `frameIndex` / camelCase columns are gone) (#718); the SW `render_ms` / `build_ms` include the buffer clear and the CSV gains 7 columns (#726); `xtframe-gpu-probe` applies the manifest safe area (`--safe 0` for the old numbers) (#697); `xtframe-view` draws the atlas cells of an atlased bundle instead of whole pages (#709).
- Custom `GpuDevice` backends: new capability queries `supportsBackdropMaskTextures` (#739) and `supportsPlanarExternal` (#747), both defaulting to false; `Shading` gains `TexturePlanar` (11) at the end; `FrameStats` gains trailing members (#700, #714, #726).

### Host integration
- IR 3.3: hosts must update the runtime together with the tools; a 3.2 runtime plays no CSS animation or transition of a 3.3 file (#730).
- `Context::needsFrame()`, C ABI `xt_surface_needs_frame` and `xt_surface_set_perf_detail` (#734); `FrameRecord` v1 and `xt_frame_record` (#718); the measurement C ABI (redundant-work counters, perf zones) (#726); the rules for handing a `Context` over between threads (#736).
- External textures: `setExternalTextureVersion` / `externalTextureUse` (#735); planar external textures (`setExternalTexturePlanes`, `Shading::TexturePlanar` = 11, `supportsPlanarExternal()`) (#747); `bindTexture` keeps the straight-alpha flag (#751); external textures used as background / border images have an intrinsic size (#749).
- Custom `GpuDevice` capability queries `supportsBackdropMaskTextures` and `supportsPlanarExternal` (default false), and the new `Shading` value at the end of the enum, see Behaviour changes.
- `ImportResult` gains a `scratch` member; `WriteResult::uisWritten` counts only UIs actually written (#743, #746).

## [0.18.0] - 2026-11-16

### Behaviour changes
- Rounded-rect clips at 1x use analytic coverage (true arcs) instead of the stencil; effects inside such a clip may overflow it, as on SW and 4x (#673, #675, #676).
- Effect caches are independent of translation: a moving element reuses its cached effect with bilinear sampling and is settled at its exact position when it stops (#677).
- A timeline member that has reached its end and holds is no longer evaluated every frame, and its subtree is pixel-snapped again (#674).
- A transition starts based on its target value; the style invalidation from state changes and binding writes is narrowed (#680).
- Perspective spaces: box shadows on affine planes are converted to the 2D path (#679); an animation whose 3D components are all identity no longer turns the element into a 3D layer (#682); flat content takes part in the ordering of a 3D segment (#683).
- A backdrop blur no longer feeds the element's own shadows, so translucent panels cast a lighter shadow (#686).
- wgpu: HAL labels are off by default (`XT_WGPU_LABELS=1` turns them back on) (#684).
- Custom `GpuDevice` backends: new capability queries `supportsAnalyticClip`, `supportsAlphaChannelReads` and `supportsBackdropMask` (all default to false); `DrawCmd::clipParamIndex` and other members are appended at the end of their structs, so hosts must rebuild.

### Host integration
- C ABI: world layers -- build, query, readback, draw and ray input (#669).
- Custom `GpuDevice` capability queries and appended struct members, see Behaviour changes.

## [0.17.0] - 2026-11-14

### Behaviour changes
- Affine planes inside a 3D space -- the own plane of a `perspective` root and layers with only `translateZ` / scale / `rotateZ` (constant w over the pass within 1/64 device px, `xt/project3d.h` `planeHomography` / `affinePlane`) -- are drawn on the 2D paths in the GPU frame builder, the software renderer and the simplifier, with the plane's mapping folded into the 2D transform. Their pixels change: at 1x they get the coverage antialiasing of 10.11 (fills, strokes, image fringe, clip edges), text goes through the glyph path (atlas / hinted bitmap) instead of outlines, and minified images are sampled through the mip chain; at 4x images and the flattening tolerance follow the 2D path as well. The root plane with an identity fold is byte-identical, and truly projected planes (`rotateX` / `rotateY`, an oblique camera) are unchanged. The GL backend is not changed, and box shadows inside a 3D space still take the off-screen effect (`10` 10.15; T-700).
- 3D transforms animate: a `translateZ()` / `translate3d()` z / three-value `translate` stop in `@keyframes`, `@timeline`, transitions and run-time `animation` is written as a Float keyframe (it used to compile into a track the IR validator rejected, so the UI failed to load with `kInvalidIr`); em / rem z stops are skipped with a warning, as for 2D translate keyframes. A lone run-time `setStyle("transform: translateZ(...)")` takes effect, and a `forwards` 3D animation no longer draws flat for one frame when it ends (`33` 33.13; T-702).
- A compiled `translateZ(2em)` (outside keyframes) is resolved against the font size; it used to be read as 2px (`33` 33.13; T-702).
- A non-animatable property (`world-layer`, `display`, `grid-area`, `hit-slop`, `z-index`, ...) or `auto-orient` in an `@keyframes` stop or an `@timeline` key is left out with one compile-time warning per property naming the property, the `@keyframes` / `@timeline` (and element) and the source line; it used to compile into a track that made the IR fail to load. `visibility` and `effect` keep animating (`33` 33.13; T-705).
- Windows x64: the wgpu-native pin moves from the upstream v29.0.1.1 release to a prebuilt of upstream commit b83eb64 (unmodified source, attached to the `shereware/XTFrame-Backends` release `wgpu-native-b83eb64`), which has memory hints; the default block strategy `manual:32:64` (`XT_WGPU_MEMORY_HINTS` unset) therefore takes effect in pinned builds, and the target-creation log line reads `memory hints manual 32..64 MiB (default)`. Linux x64 and macOS arm64 stay on v29.0.1.1, without memory hints, unchanged (`45` 45.8; T-690).
- Custom `GpuDevice` backends: `acquireLayerTarget`, `acquireLayerTargetWithStencil`, `releaseLayerTarget` and `submitLayer` are appended with safe defaults (no layer targets; `acquireLayerTargetWithStencil` forwards to `acquireLayerTarget`, so a device implementing only the first three keeps a stencil attachment on every layer). Existing devices compile and render unchanged, and `gpu::WorldLayerBuilder` reports `BuildStatus::Unsupported` on them. `CommandList` gains the `worldLayers` member (empty outside world-layer output); code that depends on its layout must rebuild (`33` 33.8, 33.12, 33.13; T-701, T-703).
- `xtframe-view`, `xtframe-pack` and `loadProjectUis` print the validator's reason after `kInvalidIr` (`IrView::openError`) (`33` 33.13; T-702).

## [0.16.0] - 2026-11-12

### Behaviour changes
- GPU targets default to 1x: `xt_gpu_target_desc.sample_count` 0 -- and an older, shorter descriptor without the field -- now means 1 (coverage antialiasing replaces MSAA); it meant 4 in 0.15.2. A host that wants 4x MSAA must pass 4. `xtframe-view --msaa` and `Code/Backends` `BackendOptions::sampleCount` default to 1 as well, the editor's GL and WebGPU previews render at 1x, and `xt::shell::WindowConfig::msaa` defaults to 0. Output at a given sample count is unchanged by this switch (`10` 10.11, `50` 50.z; T-666, T-684).
- 1x GPU rendering (edge fringe on) now antialiases what was hard-edged before: rotated / skewed image quads are feathered (T-659); concave and multi-contour fills, SVG icons and large shapes (tiled up to 4096 px) go through an R8 coverage atlas rasterized with the software renderer's own rasterizer (T-660a, T-660b, T-672); strokes (T-661); outline text, rotated / skewed text and axis-aligned text of 24 device pixels or more, the software renderer's threshold (T-662, T-676, T-681); rounded, rotated and path clips are skipped, scissored, drawn directly or masked instead of using the stencil (T-663); thin and mixed-width borders (T-664, T-676). 4x and software output are unchanged except where noted below (`10` 10.11).
- Box shadows of an opaque box whose effect chain is only an outer shadow become a `DrawBoxShadow` command, an analytic Gaussian / rounded-rectangle convolution in SW, wgpu and GL (no off-screen pass); their pixels change slightly (closer to the true convolution, at most 15/255 from the off-screen result). Solid opaque outlines, text inside its glyph boxes and backgrounds with alpha 0.9 or more qualify too. `Context::setBoxShadowCommands(false)` and `xtframe-view --no-box-shadow` keep the old path; `xtframe-bake` turns it off, so `.xtcl` output is unchanged (`10` 10.12; T-668, T-680, T-681, T-683).
- Minified images (lod > 0, smooth, not 3D-projected) are sampled trilinearly with a CPU-computed LOD in SW, wgpu and GL; the mip chain is made on the first minified draw and counts in `image_bytes` and the idle budget. Draws at lod 0 are byte-identical (`10` 10.11; T-665).
- BC1 / BC3 / BC7 DDS images are no longer decoded to RGBA on the GPU path (only the blocks are kept), DDS files with the 'XTLS' marker are laid out at their logical size, GL samples their stored mip levels trilinearly and SW samples the stored levels instead of building its own. Non-DDS images are unchanged (`10` 10.13; T-686).
- `xtframe-pack` (and the editor's Publish) removes, after a successful directory pack, the regular files in the managed subdirectories (`ui/`, `assets/`, `thumbs/`, `themes/`, `strings/`, `atlas/`, `fonts/`) and the known root artifacts that this pack neither wrote nor listed, and prints `xtframe-pack: removed N stale file(s) (B bytes) from <out>`; `--keep-stale` (`PackOptions::keepStale`) turns it off. The files listed by a previous manifest of a different UI Project are kept (`22`; T-693).
- With compression enabled (`bc7` / `bc3` / `bc1` / `auto`), every packed DDS is padded to a multiple of 4 with its logical size recorded, carries a full mip chain and has its transparent texels bleed-filled; `publish.atlas.mipmaps` no longer affects compressed images. `PackOptions::compress` defaults to the project's `publish.images.compress` (default `none`, under which pack output is byte-identical) (`10` 10.13; T-687).
- Custom `GpuDevice` backends (upgrade notes at the end of `23`): `DrawCmd` gains a trailing `coverage` texture slot (the struct size changes; rebuild against the new header), `GpuDevice::supportsCoverageDraws()` and `supportsBoxShadowShading()` are appended with default `false` (a device that does not override them gets the stencil / glyph-atlas paths and no blurred `DrawBoxShadow`, reported once), and `gpu::applyDeviceCapabilities(device, context)` turns box-shadow commands off for a device without the shading (`10` 10.14; T-660b, T-691).
- GPU targets: `xt_gpu_target_begin_frame` blocks while the GPU is more than two frames behind, and `xt_gpu_target_trim` / `destroy` wait for the last submission (T-674); `xt_gpu_target_create` precompiles every built-in shader before it returns (about 0.4 s, one info log line; `xtframe-view` and `Code/Backends` do the same at init) (T-683); on Windows with no backend requested, the wgpu instance enables D3D12 only (falling back to all primary backends if that fails), and every device asks for `maxNonSamplerBindings` 131072 (T-670) (`50` 50.ab, 50.ac, 50.ad).
- A hidden GPU-attached surface returns its textures to the target's pools, keeps its renderer and drops its coverage atlas; a hidden surface's decoded CPU images go to the resolver's idle list (`50` 50.ab, 50.ac; T-657, T-675, T-683).
- wgpu memory blocks: a wgpu-native build that has memory hints defaults to `manual:32:64` when `XT_WGPU_MEMORY_HINTS` is unset or invalid; the pinned build (v29.0.1.1) has no memory hints and is unchanged (`50` 50.ab; T-671, T-685).
- Public mirrors: the GPU backends (`xt_wgpu`, `xt_gl`, `Code/Backends`), the C ABI's GPU target implementation and `xtframe-view` move to `shereware/XTFrame-Backends`; the `xtframe` library built from the Runtime mirror has no wgpu (its GPU target functions return `XT_STATUS_UNSUPPORTED`). The development repository's targets, paths and sources are unchanged (`45` 45.8; T-689).

## [0.15.2] - 2026-11-07

### Behaviour changes
- C ABI threading contract: one thread per bundle. Two bundles may be driven from two threads at once; every surface of one bundle stays on one thread. Function-local scratch in core became `thread_local`, one-time encoder init uses `std::call_once`, and the log sink is guarded by a mutex (`50` 50.1; T-652).
- The state snapshot JSON is version 4 (`snapshotState()` / `xt_surface_save_state` write 4; `restoreState()` / `xt_surface_load_state` read 1 to 4): version 2 adds runtime class deltas, the document clock and a `shown` flag, version 3 the CSS `animation` items, version 4 in-flight CSS transitions and per-player pointer state. Loading a shown snapshot before the first update skips the initial-show sequence (no `OnShown`, no intro replay), and restoring pointer state does not replay pointer input (`23` XF-097, `50` 50.y; T-650, T-651, T-654).
- GPU targets default to 4x MSAA: `xt_gpu_target_desc.sample_count` 0 means 4, and a descriptor from an older host without the field gets 4 as well; set `sample_count` 1 to opt out (`50` 50.z; T-647).
- A hidden surface releases its software scratch (layer pool, effect scratch, masks) on its next update and re-allocates on its first visible frame, which is slightly slower; on by default, `xt_surface_set_trim_on_hide(s, 0)` turns it off. A hidden GPU-attached surface drops its texture and renderer but stays attached. Layer-pool slots idle for 120 frames are freed and slots no longer keep their largest capacity (`50` 50.aa; T-641).
- SW text: on a destination pixel with alpha below 0.5 the coverage-gamma direction follows the text luminance (the GPU atlas rule) instead of the destination's luminance; pixels with alpha 0.5 or more are unchanged (`50` 50.z; T-647).

## [0.15.1] - 2026-11-06

### Behaviour changes
- A restyle no longer replays CSS animations: animation lists are diffed item by item (identity = `@keyframes` timeline id plus its occurrence index) and changed timing is applied in place; recompiling in the source panel keeps CSS animation and timeline positions (`04` 4.13; T-616).
- Re-import without `--force` takes the board's new version of `on:` / `bind:` / `port:` attributes, class / state / `@media` / `@timeline` rules and `<body>` attributes the editor never touched; hand-edited ones are kept, and a conflict is reported when both sides changed (`import-report.json` gains `uis[].body` and `uis[].rules`) (`24` 24.4; T-618).
- Auto-anchoring centres a box whose middle is within 1 px of the board's middle (`anchor: center ...`) instead of pinning the nearer edge, and `data-xt-anchor="center"` / `"center top"` / `"left middle"` centre the axes no other word pins (`25` 25.4.1, `54` 54.5.1; T-619).
- `select(...)` / `step(...)` branch values are typed by the binding target: `true` / `false` and numbers become real booleans / numbers for bool and number ports, `checked`, `disabled` and `state.*`; a value that cannot be converted is the error `bind.branchType` (`27` 27.3.3, `24`; T-621).
- With string tables and no explicit language, `xtframe-view` shows the default language instead of raw `@key` text (prints `xtframe-view: lang <tag> (default)`; `--lang` still overrides), and the editor's language bar opens in it until the user picks one (`22` 22.16, `17` 17.5; T-623, T-630).
- A bundle with string tables opened through the C ABI loads its localization: surfaces start in the default language and `xt_surface_set_language` re-resolves every `@key` text and `localize` binding and posts `languageChanged` to the surface's flows (`50` 50.x; T-617).
- Re-import no longer moves an existing UI's rules into `shared/base.xts`: cross-UI style dedup runs only on the first import or with `--dedup-shared` (`24` 24.14.1; T-626).
- Re-import prunes `manifest/assets.yaml` entries the importer wrote that are no longer referenced (when the file is gone or still the imported bytes), and a blob that comes back with the same id but a different extension replaces the old file and entry (`24`; T-620, T-625).
- Dedup promotes the tokens referenced by shared classes into a `:root` rule in `shared/base.xts`; a declaration whose tokens differ between UIs stays on the node (`24`; T-628).
- An effect the importer adds as an animation base uses its kind's identity (`blur(0px)`, `shadow(0px 0px 0px 0px #00000000)`, `glow(0px #00000000 0)`, `color-adjust()`) instead of keyframe 0's value (`24`; T-629).

## [0.15.0] - 2026-11-05

### Behaviour changes
- `<xt version="2">` is the only source format the editor and the importer write; version 1 is still read with a deprecated hint `source format 1: run xtframe-compile --migrate` (not an error under `--strict`); rewrite a project with `xtframe-compile --migrate` or `xtframe-pack --upgrade --format 2` (`54` 54.10, 54.12; T-587, T-589, T-590).
- In v2, geometry (position, anchoring, size, flex / grid, order, `z-index`, `overflow`, 3D space) is only allowed in `<layout>` (`layout.notGeometry` elsewhere); ports are declared in `<ports>` (`port:` attributes in `<body>` are an error); `.xts` `on-<event>` / `state-enter` / `state-exit` become `on:<event>@<path>` and `on:state(X).enter|exit` on elements (`54` 54.1; T-587).
- The anchor shorthand `anchor` / `offset` / `offset-end` / `size` expands to longhands; writing `size:` in a v1 style sheet is now the error `layout.notGeometry` instead of a warning (`54` 54.5.1; T-588).
- Document ports are scope-level: flow endpoints `#<root>.<port>` become `document.<port>` (manifest `flowInterface` names of challenge-menu, fairy-forest-menu and npdo), the challenge-menu contract hash changes (`writes` key `#challenge-menu.challenge-menu.listShift` → `#challenge-menu.listShift`); hosts that bind the old key must rename it (`43` 43.6; T-589, T-590).
- The standard ListItem symbol's preview binds `items` instead of `List.items` (T-590).
- `requestShow` / `requestHide` called outside an update notify at the next update; the notification order is `lifecycle.shown` → OnShown → `show` triggers → autofocus, mirrored for hide (`53` 53.102; T-605).
- A UI's initial show reaches `OnShown`, the C ABI visibility callback and the global flow (`ui#<id>.shown`) inside the first `Context::update()`, before the update hook (`53` 53.102; T-603).
- `xtframe-symbols` group save (`saveAsGroup`) writes v2 group files (T-606).

## [0.14.0] - 2026-11-04

### Behaviour changes
- IR 3.0: the IR major is 3; 2.x `.xtir` files and bundles are rejected, and 0.13 runtimes reject 0.14 bundles; republish every bundle with the 0.14 tools (`02` 2.1, 2.2; T-534, T-585, T-586).
- The `bind.formatChain` warning is removed: converter chains in `format()` arguments are compiled into IR sub-chains (T-585).
- `bind.calcNotEvaluated` is reported only for a `calc()` that reads a port; static `calc()` values are evaluated (T-586).
- NPDO main menu: frame 0 is the intro start (the intro starts from `lifecycle.shown` instead of a timer) (T-601).

## [0.13.1] - 2026-11-04

### Behaviour changes
- Every importer, compiler and lint finding is a typed diagnostic with a code; `xtframe-import` writes `ui/<ui>/<ui>.diagnostics.json` per UI and a project-level `diagnostics.json` (format version 1); `--diagnostics <file>` writes the list, `data-xt-allow` and the accepted baseline silence known findings (T-576).
- Import never stops: the CLI keeps going past a compile error, writes conservative values (the failing declaration removed, at most 4 recompiles) and reports the error; `--strict` exits 1 when there is an error or a low-fidelity node, with the output still written (T-576).
- Anchors use one formula: a `data-xt-anchor` with px values is rewritten as `calc(1safe-<edge> + (offset - safe_px))`, so re-importing an existing project changes those px values; anchors measure the authored layout box and read computed style, and two px edges stretch (T-564).
- Bound number text and `setText` use the flow number format (`formatFloat(..., 6)`, about `%.6g`): `123.450003` becomes `123.45`, integers of 1e6 and above print in exponent form (use `thousands` / `fixed(0)` for scores) (T-568).
- A binding whose source is an out port is a warning (`bind.outPortSource`), not silent; flow `emit` to a declared out port now writes that port first, so bindings on it update (T-568, T-573).
- `.ttc` / `.otc` font collections are listed face by face (`xtframe-view --list-fonts` adds `#index` after a collection path), weights are matched by the CSS Fonts 4 ranking, synthetic bold is lighter, and CJK text breaks after every character again with kinsoku rules (no `。，、」』）！？：；` at line start, no `「『（` at line end) (T-569).
- `i18n: unused key` is one summary line, `i18n: N unused key(s) in <lang>.csv (--verbose lists them)`, in `xtframe-pack` and `xtframe-view` (T-571).
- A `pointerdrag` past the drag threshold no longer sends Click on release (T-570).
