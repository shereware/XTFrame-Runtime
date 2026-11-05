# XTFrame for programmers

A ten-minute introduction for people who write code. The design chapters under `Docs/design/` are
the specification (written in Traditional Chinese); this page only gets you started and points
at them. Every code block below is built and run by the CTest suite `intro` (`Tests/intro/`), so
it matches the current API.

## 1. Two situations

**You wire a finished UI into a game.** Artists make the UI in the editor and hand you a bundle.
Your side is small: one `xt::Context` per texture, three calls per frame, and the UI's declared
ports (its `<ports>` section - typed values in and events out) as the contract between game and
UI. This is what XTFrame is designed for, and sections 2.3-2.5 and 5 cover it.

**You write the UI yourself.** You can: a UI is one text file, `.xt`, compiled to a binary
`.xtir`. The file has sections (source format 2, 54): `<ports>` (the contract), `<body>`
(elements, triggers, bindings), `<layout>` (geometry: position, size, stacking), `<style>`
(appearance) and `<timelines>` (animation). `<layout>`, `<style>` and `<timelines>` may also live
in `.xts` fragment files pulled in with `src` (`<style src="hud.xts"/>`; that is what the importer
writes for the appearance). It looks like HTML and CSS but is not a browser - read section 3
before you start, and section 4 to decide where your logic goes.

## 2. The ten-minute path

### 2.1 Write the UI

A HUD with one number bound to a port, in one file. `hud.xt`:

```xml
<xt version="2">
  <ports>
    hp: number in = 100
  </ports>

  <body>
    <div id="Panel">
      <span id="Label">HP</span>
      <span id="HpText" bind:text="hp | round(0)" bind:class.low="hp | lt(30)">--</span>
    </div>
  </body>

  <layout>
    body {
      width: 100vw;
      height: 100vh;
    }
    #Panel {
      anchor: left top;
      offset: 24px 24px;
      display: flex;
      gap: 8px;
      padding: 8px 12px;
    }
  </layout>

  <style>
    #Panel {
      border-radius: 6px;
      background-color: #10141ccc;
      font-size: 24px;
      color: #f2f5f8;
    }
    #HpText.low {
      color: #ff5a4f;
    }
  </style>
</xt>
```

- `<ports>` declares one port per line, `name: type [in|out|call] [= default]`: here an input
  number with a default of 100. It is the whole contract the game sees; `--gen-ports` (section 6)
  turns it into a header.
- `bind:text` shows the port rounded; `bind:class.low` turns the class `low` on while `hp < 30`.
  Converters such as `round` and `lt` are a fixed set, chained with `|` (27 §27.3.3) - not an
  expression language. Every element that the host or a selector talks to has an `id`.
- `<layout>` holds every geometry declaration and nothing else; `<style>` holds the look and no
  geometry (a `width` in `<style>` or a `color` in `<layout>` is a compile error). Both are CSS
  with the same selectors and cascade. `anchor: left top; offset: 24px 24px` pins the panel 24 px
  inside the top-left corner of the safe area (the TV / notch margin the host reports with
  `Context::setDevice`; none by default, so here it is the screen corner); plain
  `position: absolute; left: 24px; top: 24px` works too and ignores the safe area (06, 54 §54.5.1).
  Which properties are geometry: `Skills/skills/xtui/references/style-groups.md`, column Section.

### 2.2 Compile and look

```sh
xtframe-compile hud.xt -o hud.xtir
xtframe-view hud.xtir
```

`xtframe-compile` reports errors as `file:line:col: level: message` (line and column in the
`.xt`, inside whichever section); add `--strict` to turn the quiet cases into errors.
`xtframe-view` opens a window (it also takes the `.xt` directly, a bundle or a UI Project);
`--headless --screenshot hud.png` renders without one.

### 2.3 Drive it from C++

Link `xt_core`; `#include <xt/xt.h>` is the whole host contract (13 §13.1-13.3, 23 §23.1).

```cpp
xt::Context ctx(xt::ContextConfig{});  // no shaper, no assets: built-in defaults
xt::Result<xt::DocumentHandle> loaded = ctx.loadDocumentFile(xtirPath);
if (!loaded.ok()) {
    std::fprintf(stderr, "load failed: %s\n", xt::statusName(loaded.status()));
    return 1;
}
xt::Document* doc = ctx.document(loaded.value());
ctx.setViewport(xt::Rect{0, 0, 1280, 720}, /*devicePixelRatio=*/1.0f);

const xt::PortId hp = doc->lookupPort("hp");  // the `hp` of the <ports> of hud.xt

xt::CommandList list;
for (int frame = 0; frame < 3; ++frame) {
    // Game state -> UI. Written before update(), so it shows this frame.
    doc->setPort(hp, xt::Value::ofFloat(42.0f));

    xt::InputEvent move;  // 1. forward your window's input
    move.kind = xt::InputEvent::PointerMove;
    move.pos = xt::Vec2{100, 40};
    ctx.pushInput(move);

    ctx.update(1.0f / 60.0f);  // 2. triggers, bindings, animation, layout
    list.clear();
    ctx.render(list);  // 3. draw commands for your renderer
}
```

The host owns the clock, the input, the assets and the pixels; XTFrame starts no thread. `render`
fills an `xt::CommandList` - walk it with your own renderer (09), or rasterize it into your RGBA
buffer with `xt::sw::SwRenderer` (`xt_sw`) and upload the dirty rectangles (23 §23.2). For text,
put a shaper in `ContextConfig::shaper` (`xt::text::FtTextShaper` from `xt_text`, then
`addSystemFonts()` or `addFontFile()`); without one nothing draws text. To compile `.xt` at run
time instead of shipping `.xtir`, link `xt_compile` (see 2.5). Shipping games load a bundle
(`xt_bundle`, 22) - one directory with every UI, its assets and fonts.

UI to game goes the other way: an `event out` port that a trigger `emit()`s, received with
`Document::addPortListener`, or a plain element listener such as
`doc->addListener(doc->byId("Start"), xt::EventType::Click, callback)` (08, 13 §13.2).

### 2.4 Drive it through the C ABI

For C, C#, Rust, Python or any FFI: the shared library `xtframe` (CMake option `XT_BUILD_CAPI`)
with the pure C99 header `xt/capi/xtframe.h` (50). It opens bundles, so pack the UI first:

```sh
xtframe-pack hud.xt -o hud-bundle
```

```c
xt_bundle* bundle = xt_bundle_open(bundleDir);
if (!bundle) {
    fprintf(stderr, "open failed: %s\n", xt_capi_last_error());
    return 1;
}
xt_surface_desc desc = {0};
desc.struct_size = sizeof desc;
desc.width = 640;
desc.height = 360;
desc.system_fonts = 1; /* or bring fonts in the bundle; without any, no text is drawn */
xt_surface* hud = xt_surface_create(bundle, "hud", &desc); /* UI id = the .xt file's stem */
if (!hud) {
    fprintf(stderr, "surface failed: %s\n", xt_capi_last_error());
    return 1;
}

uint32_t hp = xt_surface_port_lookup(hud, "hp");
unsigned char* pixels = malloc((size_t)desc.width * desc.height * 4);
for (int frame = 0; frame < 3; ++frame) {
    xt_surface_port_set_number(hud, hp, 42.0);
    /* xt_surface_push_input(hud, &event);  for each input event */
    xt_surface_update(hud, 1.0f / 60.0f);
    xt_damage damage;
    xt_surface_render(hud, pixels, desc.width * 4, &damage); /* RGBA8 -> upload to your texture */
}
```

Every call on a surface is made on one thread; callbacks (`xt_surface_set_port_listener` and the
others) run inside `xt_surface_update` and the `port_set_*` calls. Strings handed out live until
the next call on the same object.

### 2.5 Test it headless

No window, no GPU and no fonts are needed to test the wiring: compile in process (`xt_compile`),
drive the ports or `pushInput`, `update`, and read back text, classes and states.

```cpp
xt::compile::CompileResult compiled = xt::compile::compileDocument(XT_INTRO_HUD, {});
for (const xt::compile::Diagnostic& d : compiled.diagnostics) {
    std::fprintf(stderr, "%s\n", d.message.c_str());
}
if (!compiled.ok()) {
    return 1;
}

xt::Context ctx(xt::ContextConfig{});
xt::DocumentHandle handle =
    ctx.loadDocument(xt::IrBlob{compiled.ir.data(), compiled.ir.size()}, /*takeOwnership=*/true).value();
xt::Document* doc = ctx.document(handle);
ctx.setViewport(xt::Rect{0, 0, 1280, 720}, 1.0f);
ctx.update(0.0f);

xt::ElementRef hpText = doc->byId("HpText");
check(hpText.text() == "100", "the port default shows");

doc->setPort(doc->lookupPort("hp"), xt::Value::ofFloat(17.4f));
ctx.update(1.0f / 60.0f);
check(hpText.text() == "17", "bind:text rounds the port");
check(hpText.hasClass("low"), "bind:class.low turns on below 30");
```

`xtframe-view --headless --frames N` and `Examples/desktop_app --headless` run the same way;
the repository's own tests compare rendered frames byte for byte.

## 3. Not a web page

The first fifteen lines of `Skills/skills/xtui/references/not-like-web.md` (read the whole page
once):

1. Geometry only in `<layout>`, the look only in `<style>` (`display: none` is the exception).
2. Always border-box; there is no `box-sizing`.
3. `display` is `block | flex | grid | none` - no inline, inline-block or table.
4. `position` is `static | relative | absolute` - no `fixed` or `sticky`.
5. `z-index` only reorders siblings; to draw above everything use `layer: overlay` in `<layout>`.
6. `pointer-events` is inherited: `none` on a full-screen container, `auto` on each panel.
7. A `<use>` instance root is a sized box that takes clicks even when its inside is hidden.
8. Flex items have no min-content minimum: give text items `min-width` or `flex-shrink: 0`.
9. `translateZ` sorts whole layers by their anchor, not pixels; crossing planes do not cut.
10. Ports have a direction: a flow reads `out` and writes `in`; a board binds `in` ports.
11. `bind:` beats a static class: bind it or write it, not both.
12. List ports are typed (`list<struct{...}>`, `list<string>`); the host sends JSON.
13. Lists nest: an inner `<repeat port="item(rows)">`; `outer.item(field)` reads one level up.
14. `bind:visible` keeps the space; `bind:display` removes the element from the layout.
15. `Disabled` is always the built-in `:disabled`, and it disables the whole subtree for input.

## 4. Four ways to put logic in

| Route | Use it when |
|---|---|
| C++ listener (`Document::addListener`, `addPortListener`) | the logic needs game state or belongs to the game; you want your debugger |
| `on:` + `bind:` on the elements of `<body>` (27 §27.2-27.3) | presentation reacts to the UI's own events and ports: press plays a timeline, a value drives text, width or a class; it travels with the UI and artists can edit it |
| flow (`.xtflow`, `xt_flow`; 27 §27.4, 53) | UI-side event logic across elements or UIs without host code: routing, state machines, filtering; optional (a host may not run `xt_flow` at all) and never for animation |
| `xt::flow::HostPorts` (13 §13.8.1) | the game-UI contract by port name that survives reloads: set in-ports, receive emits, call ports, register converters; what engine plugins and the C ABI use |

Give each value one owner. Ports are the seam between routes: the game writes `in` ports, the UI
and flow `emit` `out` ports, and nobody pokes the same element from two routes.

## 5. When a write shows up, and what a callback may do

`update(dt)` runs, in order: input dispatch (triggers and the flow's pulse edges run here) ->
`ContextListener::onBeforeDocumentUpdate` -> style, animation and layout of every document.
`render` does not resolve styles again. So (23 §23.1):

- A write (port, class, style, text) made **before** `update` shows in that frame.
- A write made **after** `update` and before `render` is only marked dirty and shows at the
  next `update`; a transition it starts takes its start time from the next frame too.
- To act in the same frame as an input event or the flow, write inside the event callback, in
  `onBeforeDocumentUpdate`, or (C ABI) in `xt_surface_set_update_hook`.

Callbacks run synchronously inside `update`, the port setters and other calls that drive the
document (13 §13.9-13.10):

- `unloadDocument` inside a callback makes the document dead at once (no more callbacks from
  it); its memory goes when the outermost call returns.
- `reloadDocument` / `hotReload` inside a callback load and validate at once but install when the
  dispatch ends. After any reload every `Document*`, `ElementRef` and listener id is invalid -
  fetch them again.
- `requestShow` / `requestHide` from a callback take effect when the dispatch ends.
- Do not call `Context::update` from a callback. C ABI: `xt_surface_destroy` and
  `xt_surface_hot_reload` are refused inside callbacks; defer them.

## 6. Tools

| Tool | What it does |
|---|---|
| `xtframe-compile <file.xt> -o <file.xtir>` | compile one UI; `--strict`, `--diagnostics FILE` (12) |
| `xtframe-compile --migrate <project\|file.xt> [-o DIR]` | rewrite a format 1 UI (ports as `port:` attributes, geometry mixed into the `.xts`) as format 2; checks that every element's computed style stays the same (54 §54.10) |
| `xtframe-view <file.xt\|.xtir\|bundle\|.xtui>` | preview in a window; `--headless --frames N --screenshot out.png` for scripts |
| `xtframe-pack <project\|file.xt> -o <dir>` | publish a bundle (22); `--zip` for one `.xtb` file, `--strict` for CI |
| `xtframe-pack ... --gen-ports cpp\|cs <out>` | a typed header of every UI's ports (for hud.xt: `xtui::hud::hp`, one `PortId` constant per port) to compile against (23 §23.10) |
| `xtframe-service --stdio` | the headless Editor Service: JSON lines on stdin, the same operations as the editor, for scripts and agents (17, 20); needs `XT_BUILD_EDITOR_CLI` |

Hot reload while you type: `Examples/desktop_app --watch` recompiles the UI when you save it and
swaps it in (`xt::shell::SourceWatcher` from `xt_shell_watch`; a compile error is printed and
the old UI stays). In your own host call `Context::hotReload` / `xt_surface_hot_reload` (port
values and states carry over when the new UI still has them) or `reloadDocument` yourself.

## 7. Further reading

- [README](../../README.md) - building, modules, quick start, examples.
- [13 Public C++ API](../design/13-api.md) - `Context`, `Document`, `ElementRef`, ports, reentrancy.
- [23 Host integration](../design/23-host-integration.md) - frame order, damage, fonts, assets.
- [50 C ABI](../design/50-c-abi.md) - `xtframe.h`, threads, the Unity package.
- [22 Bundle](../design/22-bundle.md) - what `xtframe-pack` writes and how a game loads it.
- [27 Interaction](../design/27-interaction.md) - states, `on:` triggers, `bind:` and converters, flow.
- [54 Source format 2](../design/54-source-format-v2.md) - the sections of a `.xt`, `src`
  fragments, the anchor shorthands, migrating from format 1.
- [05 Style and CSS](../design/05-style-and-css.md), [06 Layout](../design/06-layout.md),
  [08 Events](../design/08-events.md) - what `<layout>` / `<style>` and the event model support.
- [53 Flow language](../design/53-flow-language.md) - the `.xtflow` graphs.
- [Not like web](../../Skills/skills/xtui/references/not-like-web.md) - every difference from HTML/CSS.
- `Examples/desktop_app/` (a window with `xt_shell`), `Examples/engine_integration/` (a host
  with its own renderer), `Tests/intro/` (the code of this page).
