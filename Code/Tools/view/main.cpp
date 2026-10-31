#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef XT_SHELL_SW
#include "gl_loader.h"  // Private header (Code/Runtime/gl/src, see Code/Tools/CMakeLists.txt); --bind-texture's
                        // GL upload.
#endif
#include "../Common/console_utf8.h"
#include "../Common/print_version.h"
#include "../Common/standard_symbols_note.h"
#include "view_flow.h"
#include "view_renderer.h"
#include "xt/animator.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_file.h"
#include "xt/bundle/project_runtime.h"
#include "xt/bundle/symbol_library.h"
#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/flow/bundle_context.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/scene_instance.h"
#include "xt/shell/app.h"
#ifndef XT_GL_NO_TEXT
#include "xt/text/ft_text_shaper.h"
#endif
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/timeline.h"

/// xtframe-view: opens a `.xtir`, `.json` (Lottie) or `.xt` (T-56: compiled at runtime, the same
/// pattern Examples/engine_integration/main.cpp uses) file, a Bundle or a UI Project in an xt_shell
/// window for interactive preview, or renders a single frame to a PNG with --screenshot. See
/// Docs/tasks/T-20.md "xtframe-view".
///
/// T-213 (Docs/design/22-bundle.md §22.11.2): the input may be
///   - a Bundle directory (holding `manifest.json`) or its `manifest.json`: the UI's `.xtir` is
///     opened with the Bundle root as asset root, its manifest `flow` loaded (announced like T-135's
///     auto flow), its `fit`/`size` and manifest `spec` (safe area, logical height) applied;
///   - a UI Project: its directory, its `<name>.xtui` (T-214), or an old `ui-project.json` /
///     `bundle.xtproj` (upgraded in memory, the file is not touched). It is packed (xt::bundle::pack(), no
///     thumbnails, no atlas, no `manifest/assets.yaml` write-back, `--prune`) into
///     `%TEMP%/xtframe-view/<id>-<hash of the project path>` and that Bundle is opened as above;
///     stderr says `xtframe-view: packed <dir> -> <bundle>`. Compile errors print as diagnostics and
///     exit 1 only when no UI could be packed. R (reload) packs again first;
///   - a `.xtir`, `.xt` or Lottie `.json` file, as before.
/// A Bundle or UI Project with several UIs needs `--ui <id>` (the ids are listed otherwise).
///
/// Usage: xtframe-view <file.xtir | file.json | file.xt | bundle-dir | manifest.json | ui-project-dir |
///                    name.xtui | ui-project.json | bundle.xtproj> [--root DIR] [--width W]
///                    [--height H] [--ui <id>] [--no-flow] [--strict]
///                    [--msaa N] [--screenshot PATH] [--flow <file.xtflow>] [--headless] [--transparent]
///                    [--frames N]
///                    [--print-text <id>] [--perf] [--perf-csv <file>] [--show-damage]
///                    [--event "<frame> #<id>.<Port>[=<value>]"] [--pointer "<frame> <x> <y> [move|down|up]"]
///                    [--dump-flow-values] [--dump-every N] [--dump-commands]
///                    [--bind-texture <name>=<file.png>]
///                    [--fit native|stretch|contain|cover|center] [--play <timeline>]
///                    [--seek <seconds>] [--device W:H] [--safe F|T,R,B,L]
///                    [--platform desktop|console|handheld] [--input mouse|gamepad|touch]
///                    [--lang <tag>] [--port <name>=<value>] [--gamepad] [--players N]
///                    [--borderless] [--resize-border N] [--no-live-resize]
///                    [--fullscreen desktop|exclusive]
///                    [--second-window <file> [--second-display N] [--second-size WxH]] [--help]
///        xtframe-view --version
///        xtframe-view --list-fonts[=names]
///
/// --borderless (T-376, Docs/design/23-host-integration.md §23.ab) opens the window without OS
/// decorations: `app-region: drag` elements move the window, the edges (--resize-border N
/// window-logical pixels, default 8; 0 = none) resize it, and an out event `winMinimize`,
/// `winMaximize` (maximize / restore) or `winClose` -- a UI `emit(...)` or a flow `emit` node --
/// minimizes, maximizes or closes it.
///
/// --second-window <file> (T-384b, 23 §23.ad) opens a second window (App::createWindow) showing
/// another `.xt` / `.xtir` / Lottie `.json` file in its own Context, through the same loading code,
/// fonts and asset resolver. --second-display N centres it on display N (App::displayBounds),
/// --second-size WxH sizes it (default: the main window's size). It has vsync off (23 §23.ad
/// pacing), never --borderless, and --screenshot only captures the main window. Closing it keeps
/// the main window running; closing the main window ends the run.
///
/// --fullscreen desktop|exclusive (T-398, 23 §23.ad.1) opens the main window fullscreen:
/// `desktop` borderless at the desktop resolution (no mode change), `exclusive` at the window size.
/// With --second-window, the second window is raised (Window::raise()) once it has loaded, so it
/// starts in front of the main window.
///
/// --no-live-resize (T-377, 23 §23.ab) turns off App's live resize: by default the window keeps
/// drawing (and reflowing) while its edge is dragged, and --stats counts those frames too.
///
/// --lang <tag> (Docs/design/13-api.md §13.8.2, Docs/tasks/T-179.md) reads every
/// `strings/<lang>.csv` below the root directory (a bundle.xtproj input's bundle root, else --root
/// or the input's directory) into an xt::bundle::BundleLocalizationProvider and calls
/// Context::setLanguage(<tag>) before the document is loaded, so its `@key` text shows that
/// language. A malformed table exits 1; no `strings/` directory just shows the keys.
///
/// --port <name>=<value> (Docs/design/27-interaction.md §27.3, Docs/tasks/T-175.md) sets a declared
/// port (Document::lookupPort / setPortText: a number, true/false, a color, an enum value name,
/// text; T-177: a JSON object for a struct port, a JSON array of objects for a list port, e.g.
/// `--port items='[{"name":"A"},{"name":"B"}]'`; T-381: a JSON array of values for a list<T> port,
/// e.g. `--port 'labels=["a","b"]'`; T-200: `--port pos=[1,2]` for a vec2/vec3 port) just
/// before the first frame's update(); repeatable. `--event
/// "<frame> port <name> <value>"` does the same before a later frame. An unknown port or a value that does
/// not parse prints one warning and the run continues.
///
/// --device/--safe/--platform/--input (Docs/design/13-api.md §13.8.2, Docs/tasks/T-176.md) call
/// Context::setDevice() before the first frame: --device W:H sets the aspect ratio `@media
/// (aspect-ratio)` sees and makes the viewport width --height x W/H; --safe gives the safe-area
/// insets (fractions of the viewport) the `safe-*` units resolve to; --platform and --input feed
/// `@media (platform)` and `(input)`. T-207: without --safe, a Bundle or UI Project input takes the
/// manifest's `spec.safeArea` (`console` with --platform console; a Bundle packed from a lone `.xt`
/// has none). T-210: such an input also sets uiscale = viewport height / `spec.logicalHeight`
/// (T-241: the viewport width or its short edge instead when `spec.uiscaleRule` says so).
///
/// --play <timeline> (Docs/design/12-compiler.md §12.6/13-api.md §13.8.2, Docs/tasks/T-172.md)
/// plays the document's root-scope `@timeline` of that name just before the first frame's
/// update(); --seek <seconds> then moves it to that time (it keeps playing), so a --headless
/// --frames 1 screenshot shows exactly that time point. --seek needs --play. An unknown timeline
/// prints one warning and the run continues.
///
/// --fit (Docs/tasks/T-156.md) chooses how the windowed run places the document: `native` lays it
/// out at the window size (the behaviour before T-156); the other modes lay it out at the design
/// size and scale/centre that image in the window (xt::shell::FitMode). A `bundle.xtproj` input
/// takes the chosen UI's `fit` and `size`; any other input defaults to `native`, with --width x
/// --height as the design size. --headless ignores it (it renders at --width x --height).
///
/// --bind-texture <name>=<file.png> (Docs/tasks/T-150.md) simulates a host texture: the PNG is
/// uploaded as a GL texture the renderer did not create (straight alpha, as an engine would hand
/// it over) and bound as `engine://<name>` through AssetResolver::setExternalTexture, so an
/// `<img src="engine://<name>">` samples it. Under --headless only the size is bound (layout is
/// right; T-191: the PNG's pixels are bound for the software renderer too).
///
/// --event / --pointer (Docs/tasks/T-125.md) script interaction for a run that has no user: both
/// are repeatable and both are applied just before the named frame's Context::update(), i.e. after
/// every earlier frame's layout has settled, so an element's box is known.
///   --event "<frame> #<id>.<Port>" drives one of §17.6's element connection points: `Click`
///   (a PointerDown + PointerUp at the element's centre), `PointerEnter` (a PointerMove onto the
///   centre), `PointerLeave` (a PointerMove off the viewport), `Focus`/`Blur`
///   (Document::setFocus) and `Change` (which needs a `=<value>` suffix and calls
///   ElementRef::setValue, whose own Change dispatch is the event). T-174 adds
///   `state(<Name>[,on|off|toggle])` (no spaces; default `on`), which sets a custom state through
///   Context::setCustomState and so fires the element's `state(<Name>).enter/.exit` triggers.
///   T-175 adds `--event "<frame> port <name> <value>"` (a declared port, see --port).
///   T-194 adds `--event "[<frame>] press <button> [<player>]"`: a gamepad button press (down +
///   up) of local player <player> (default 0) before frame <frame> (default 0); <button> is one of
///   A B X Y Back Guide Start LS RS LB RB Up Down Left Right (Up..Right = the d-pad).
///   T-193 adds `--event "[<frame>] system <kind> [<value>]"`: FlowRuntime::postSystemEvent() on the
///   --flow graph and, with --project, on the global and every other UI's flow (`kind`: loading,
///   paused, resumed, disconnected, languageChanged, platform, inputDevice; frame 0 when omitted).
///   T-200: also Context::postSystemEvent(), so every Document's `system.*` port gets it.
///
/// --project <dir> (T-193, Docs/design/27-interaction.md §27.4; T-213: an alias of giving <dir> as
/// the input, so it cannot be combined with one) opens a UI Project or Bundle as a whole: the
/// `--ui` UI, else the first one, is the main UI. Every other UI of the Bundle is loaded too
/// (before the main one, so the main UI stays the foreground Document that takes input) with its
/// own flow, all of them are registered on an xt::flow::BundleContext, and the Bundle's global flow
/// (manifest `flows.global`) is loaded against it (announced as `xtframe-view: loaded global flow
/// <path>`; an invalid one exits 2). --dump-flow-values also prints the global flow's values, and
/// every flow action node event (sound / haptic / spawn / emit) prints as `xtframe-view: flow event
/// <kind> <name> ui=<id> instance=<n>`. Not covered: R (reload) reloads the main UI only and then
/// reloads the global flow (T-546: after each reload the flow diagnostics print again; --strict
/// does not exit on a reload).
///
/// --gamepad (T-194, Docs/design/08-events.md §8.9) opens SDL game controllers in the windowed
/// mode (one player slot each, in connection order). --players N (1..8) limits the slots to N and
/// marks players 0..N-1 as gamepad players (Context::setPlayerDevice).
///   --pointer "<frame> <x> <y> [move|down|up]" pushes one raw InputEvent at viewport coordinates
///   <x>/<y> (default `move`, left button for `down`/`up`) through Context::pushInput.
///
/// --dump-flow-values prints the loaded --flow graph's output-port values as `<node>.<port> =
/// <value>` (one per line) when a --headless run ends; --dump-every N prints the same block every
/// N frames as well. Without --flow there is nothing to print.
///
/// --perf (Docs/design/21-perf-tracing.md §21.3) enables xt::Context's built-in frame tracing: in
/// interactive mode the window title shows `fps | update a.b ms | render c.d ms`, refreshed every
/// 0.5 s; in every mode (including --headless) a PerfSummary table is printed to stdout when the
/// run ends. --perf-csv <file> additionally writes one CSV row per frame (it implies --perf).
///
/// --show-damage (Docs/design/23-host-integration.md §23.2, Docs/tasks/T-98.md) asks
/// Context::render() for this frame's dirty rectangles and outlines each one with a 1 px red
/// border in the software backend's output; a `full` frame outlines the whole viewport. The GL
/// (windowed) mode has no such overlay: it prints one warning and ignores the flag. Damage is
/// also computed (without the overlay) under --perf, which then reports the `damage` phase time
/// and the average share of the viewport the rectangles covered.
///
/// --strict (T-392) compiles `.xt` input with CompileOptions::strict and, when a --headless run
/// ends, prints every Document::warnings() entry as `xtframe-view: warning: <message>` on stderr
/// (27 §27.3.2: a virtualized `<repeat>` whose viewport is 0 px high).
///
/// --print-text <id> prints `text=<the element's text>` when a --headless run ends (T-66a: the
/// editor's own `--demo` headless run prints the same number, which is how the two are compared).
/// It is ignored outside --headless, and prints an empty text for an id the document does not have
/// (T-392: plus a `xtframe-view: --print-text: no element '<id>'` line on stderr).
/// It is repeatable (T-125); the lines come out in the order the options were given.
///
/// T-135 (Docs/design/12-compiler.md §12.6) auto-loads the flow graph the importer writes next
/// to the document, so an imported UI is interactive without a hand-written --flow:
///   - a `.xt` input whose directory holds `<stem>.xtflow` loads that file;
///   - a Bundle (or packed UI Project, T-213) input loads the chosen UI's manifest `flow`.
/// An auto-loaded graph prints one `xtframe-view: loaded flow <path>` line to stderr (a hand-given
/// --flow does not). `--no-flow` disables all of it, including an explicit --flow.
///
/// --flow loads a `.xtflow` demo graph (Docs/design/17-editor.md §17.6, Docs/tasks/T-56.md)
/// against the opened document and ticks it every frame; a load/parse error is printed to stderr
/// (one line per problem) and exits 2. --headless (added by T-56, xtframe-view had no headless mode
/// before) skips creating a window/GL context entirely, drives a plain xt::Context + xt_sw
/// software renderer for exactly --frames frames (default 60) instead, and exits 0 -- used by
/// CTest, which has no display.
///
/// --renderer sw|gl|wgpu (T-439, view_renderer.h) picks one of the renderers built in (the default is
/// unchanged: xt_sw under --headless, else the shell's -- gl, or sw for an XT_SHELL_GL=OFF build). Under
/// --headless gl and wgpu render the frames offscreen (an FBO of a hidden GL window; a headless WebGPU
/// device) instead of xt_sw; a windowed run with a non-default renderer draws through Code/Backends' files in
/// an SDL window (no shell extras: --fit, --scene, --second-window, --borderless... are the shell's). An
/// unknown or missing renderer prints one line and uses the default. --version adds `renderers: <built in>`.
///
/// Exit codes: 0 success, 1 a load/compile error, 2 argument error (usage printed to stderr).
/// --help/-h prints the usage to stdout and exits 0. --version (T-420) prints the version lines
/// (Code/Tools/Common/print_version.h) to stdout and exits 0, whatever else is on the command line.
/// --list-fonts (T-426) registers the system fonts (FtTextShaper::addSystemFonts()) and prints
/// one `family<TAB>path` line per registered font file to stdout (path = the file it was
/// registered from; UTF-8; sorted and deduplicated, T-429), and exits 0, whatever else is on the
/// command line (--version still wins). --list-fonts=names prints only the family names, one per
/// line, sorted and deduplicated (the T-426 output). Tools/Scripts/corpus_check.py uses it to check a
/// corpus project's required fonts and their file hashes.
///
/// Keys (read via Context::drainInput, this task's queue-only pushInput/drainInput -- no
/// dispatch/hit-testing yet, T-32): Space pause/resume every `anim` element's defaultAnimation
/// instance; Left/Right step one frame back/forward (seek(localT +/- 1/frameRate), frameRate from
/// the anim's Timeline, or 1/30 when frameRate == 0); R reload (recompile/reread the file into a
/// fresh Document, replacing the old one); Escape quits.
///
/// Not covered by Docs/tasks/T-20.md (conservative choices made here):
///   - `.xtir` vs `.json` vs `.xt` is chosen by file extension (case-sensitive ".json"/".xt";
///     anything else is treated as `.xtir`), matching xtframe-compile's convention.
///   - `--root` defaults to the input file's parent directory (same rule as xtframe-compile), used
///     both for compileLottie's CompileOptions::rootDir and as the shell's asset root.
///   - Space/Left/Right/R act on every element with a non-zero defaultAnimation instance (not
///     just the first `anim`), since a document may contain more than one.
///   - Reload (R) failure (compile/parse error) leaves the previous Document displayed and prints
///     the diagnostics to stderr; it does not exit the process.
///   - T-135 left these open: an unknown `--ui <id>` lists the bundle's ids and exits 2 like an
///     ambiguous bundle does; `--no-flow` also cancels an explicit
///     `--flow` (it means "run without a graph", not "do not guess one").
///   - T-125 left these open: a `--event`/`--pointer` whose frame is past the last frame rendered
///     never fires (no warning); an `--event` naming an id the document does not have prints one
///     stderr line and the run continues (exit code unchanged); `Blur` clears the document focus
///     outright rather than checking that the named element is the one holding it; `PointerLeave`
///     only moves the pointer to (-1, -1) (outside the viewport), so it is meant to be paired with
///     an earlier `PointerEnter`; `--dump-flow-values` prints Pulse ports as the literal `pulse`
///     and Numbers with `%g`.
///   - System font registration (Docs/design/12-compiler.md §12.6, Docs/tasks/T-30.md goal 5):
///     every `.ttf`/`.otf` directly inside the platform's font directory, at most 200 files in
///     total across the scanned directories, registered before the document is loaded (so a
///     document's own `@font-face` fonts, registered at load time, are matched first only by
///     family name -- there is no priority ordering between the two beyond FtTextShaper's own
///     family matching).

namespace {

namespace fs = std::filesystem;
// T-261f: every path this tool keeps in a std::string is UTF-8 (its arguments included); these
// convert, never `fs::path(std::string)` / `path::string()` (the ANSI code page on Windows).
using xt::compile::genericPathToUtf8;
using xt::compile::pathFromUtf8;
using xt::compile::pathToUtf8;
using xt::tools::view::loadFlow;

/// One `--event`: a §17.6 element connection point driven just before `frame`'s update().
struct ScriptedEvent {
    uint32_t frame = 0;
    std::string id;      ///< Element id, without the leading '#'; T-175 `Port`: the port name.
    std::string port;    ///< Click / PointerEnter / PointerLeave / Focus / Blur / Change / State / Port.
    std::string value;   ///< `Change` / `Port`: the value to write; `State` (T-174): the state name.
    std::string mode;    ///< `State` only: on / off / toggle.
    uint8_t player = 0;  ///< T-194 `Pad`: the local player.
};

/// One `--pointer`: a raw InputEvent pushed just before `frame`'s update().
struct ScriptedPointer {
    uint32_t frame = 0;
    float x = 0;
    float y = 0;
    xt::InputEvent::Kind kind = xt::InputEvent::PointerMove;
};

struct Args {
    std::string input;
    std::string root;
    /// T-498b: the preset symbol library's default group next to the executable (empty when
    /// none): resolves `xt-standard:` links and supplies the implied token layer when a UI
    /// Project is packed (PackOptions::standardSymbolDir with copyStandardSymbols off).
    std::string standardSymbolDir;
    /// T-527d (22 §22.10.3): the standard fonts next to the executable (`<exe dir>/fonts`, the
    /// same root as `standardSymbolDir`), PackOptions::standardFontDir of that pack.
    std::string standardFontDir;
    int width = 1280;
    int height = 720;
    int msaa = 4;
    std::string screenshot;  // Empty = interactive mode.
    std::string flow;        // Empty = no flow graph.
    std::string ui;          // --ui: which UI of a Bundle / UI Project to open.
    std::string fit;         // --fit (T-156); empty = the bundle UI's, else native.
    int designWidth = 0;     // Set by resolveInput() from a bundle UI's `size`.
    int designHeight = 0;
    std::vector<std::string> printText;     // Element ids to report at the end of a headless run.
    std::string perfCsv;                    // Empty = no per-frame CSV.
    std::vector<ScriptedEvent> events;      // --event, in the order given.
    std::vector<ScriptedPointer> pointers;  // --pointer, in the order given.
    std::vector<std::pair<std::string, std::string>> bindTextures;  // --bind-texture name=png (T-150).
    bool headless = false;
    bool transparent = false;   // T-342: --transparent (headless clear colour alpha 0).
    std::string scene;          // T-343: --scene <png>: the host scene under the UI (Direct mode).
    bool sceneAnimate = false;  // T-343: --scene-animate: the scene scrolls 1 px per frame.
    bool hidden = false;        // T-240: --hidden (the window is created hidden).
    bool borderless = false;    // T-376: --borderless (no OS window decorations).
    int resizeBorder = 8;       // T-376: --resize-border N (window-logical pixels).
    bool liveResize = true;     // T-377: --no-live-resize turns it off.
    int fullscreen = 0;         // T-398: --fullscreen (0 off, 1 exclusive, 2 desktop).
    bool stats = false;         // T-240: --stats (App's once-per-second frame-time log).
    std::string secondWindow;   // T-384b: --second-window <file>.
    int secondDisplay = -1;     // T-384b: --second-display N (-1 = the OS places it).
    int secondWidth = 0;        // T-384b: --second-size WxH (0 = the main window's size).
    int secondHeight = 0;
    bool perf = false;
    bool showDamage = false;
    bool dumpFlowValues = false;
    bool dumpCommands = false;  // T-202: print the last headless frame's CommandList.
    std::string dumpPorts;      // T-401: --dump-ports <file>: the main UI's port table as JSON.
    bool logPorts = false;      // T-401: --log-ports: print every port event of a headless run.
    bool noFlow = false;
    bool strict = false;    // T-392: --strict (compile strict; runtime warnings printed at the end).
    bool flowAuto = false;  // Set by resolveInput(): `flow` was guessed, not given.
    // Asset search directories handed to the resolver and the `.xt` compiler; empty = --root.
    std::vector<std::string> assetRoots;
    bool help = false;
    bool version = false;        // T-420: --version.
    std::string renderer;        // T-439: --renderer (empty = the default).
    bool listFonts = false;      // T-426: --list-fonts.
    bool listFontNames = false;  // T-429: --list-fonts=names (families only).
    uint32_t dumpEvery = 0;      // 0 = only at the end of the run.
    uint32_t frames = 60;
    /// T-389 (measurement only): --synth-pointer N queues N PointerMove events before every
    /// frame's update(), stepping along the viewport diagonal (16 = a 1000 Hz mouse at 60 fps).
    uint32_t synthPointer = 0;
    /// T-403 (measurement only): --synth-ports N writes the first N document-level Number ports
    /// (declaration order, any direction) before every frame's update(), each a different
    /// triangle wave in 0..100 that changes every frame (Docs/tasks/T-403.md item 2).
    uint32_t synthPorts = 0;
    uint32_t warmup = 0;            // T-165: windowed --screenshot frames rendered before the capture.
    float warmupDt = 1.0f / 60.0f;  // T-165: dt of each warm-up frame.
    uint32_t screenshotEvery = 0;   // T-165: also capture every Nth warm-up frame (0 = off).
    std::string playTimeline;       // T-172: --play, empty = none.
    xt::DeviceInfo device;          // T-176: --device/--safe/--platform/--input.
    bool safeGiven = false;         // T-207: --safe given (else a UI Project's spec.safeArea).
    float logicalHeight = 0;        // T-210: a UI Project's spec.logicalHeight (0 = none).
    xt::bundle::UiScaleRule uiscaleRule = xt::bundle::UiScaleRule::Height;  // T-241: spec.uiscaleRule.
    xt::bundle::UiScaleLimits uiscaleLimits;  // T-345: spec.uiscaleClamp / spec.uiscaleDpiFloor.
    std::string lang;                         // T-179: --lang, empty = none.
    std::vector<std::pair<std::string, std::string>> ports;  // T-175: --port name=value.
    float seekTime = -1.0f;                                  // T-172: --seek, < 0 = none.
    bool gamepad = false;                                    // T-194: --gamepad.
    uint32_t players = 0;                                    // T-194: --players, 0 = not given.
    std::string project;                                     // T-193: --project <dir>.
    std::string mainUi;         // T-193: the main UI's id when opened through a project / bundle.
    bool wholeProject = false;  // T-213: --project: every UI of the Bundle plus its global flow.
    std::string packSource;     // T-213/T-214: the UI Project (dir or root file) packed; R re-packs.
    std::string bundleDir;      // T-213: the Bundle the input resolved to; empty for a lone file.
    // T-213: the Bundle's other UIs for --project: { id, `.xtir` path, flow path or "" }.
    std::vector<std::array<std::string, 3>> otherUis;
    std::string globalFlow;   // T-213: the Bundle's `flows.global`, as a path; empty = none.
    bool loadFailed = false;  // T-213: resolveInput() failed on the content (exit 1, not 2).
    // T-555: the Bundle the input resolved to, kept open for addBundleFonts(); null for a lone file.
    std::shared_ptr<xt::bundle::Bundle> bundle;
};

/// T-194: `--event "... press <button>"` button names -> GamepadButton.
bool gamepadButtonFromName(const std::string& name, xt::GamepadButton& out) {
    static const std::pair<const char*, xt::GamepadButton> kNames[] = {
        {"A", xt::GamepadButton::A},
        {"B", xt::GamepadButton::B},
        {"X", xt::GamepadButton::X},
        {"Y", xt::GamepadButton::Y},
        {"Back", xt::GamepadButton::Back},
        {"Guide", xt::GamepadButton::Guide},
        {"Start", xt::GamepadButton::Start},
        {"LS", xt::GamepadButton::LeftStick},
        {"RS", xt::GamepadButton::RightStick},
        {"LB", xt::GamepadButton::LeftShoulder},
        {"RB", xt::GamepadButton::RightShoulder},
        {"Up", xt::GamepadButton::DpadUp},
        {"Down", xt::GamepadButton::DpadDown},
        {"Left", xt::GamepadButton::DpadLeft},
        {"Right", xt::GamepadButton::DpadRight},
    };
    for (const auto& n : kNames) {
        if (name == n.first) {
            out = n.second;
            return true;
        }
    }
    return false;
}

/// Splits `text` on ASCII whitespace, dropping empty pieces.
std::vector<std::string> splitWhitespace(std::string_view text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.emplace_back(text.substr(start, i - start));
        }
    }
    return out;
}

/// Parses a decimal frame number; false when `text` is not all digits (or is empty).
bool parseFrame(const std::string& text, uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    out = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
    return true;
}

/// T-392 (XF-078): the one stderr line of a malformed --event: the spec as given, the reason and
/// the element form.
void eventSpecError(const std::string& spec, const std::string& reason) {
    std::fprintf(stderr,
                 "xtframe-view: cannot parse --event '%s': %s (expected \"<frame> #id.<Event>[=value]\")\n",
                 spec.c_str(), reason.c_str());
}

/// `--event "<frame> #<id>.<Port>[=<value>]"`; prints its own stderr line and returns false on a
/// malformed spec (the caller then prints the usage and exits 2).
bool parseEventSpec(const std::string& spec, ScriptedEvent& out) {
    std::vector<std::string> parts = splitWhitespace(spec);
    {
        // T-194: `[<frame>] press <button> [<player>]`.
        size_t at = 0;
        uint32_t frame = 0;
        if (!parts.empty() && parseFrame(parts[0], frame)) {
            at = 1;
        }
        if (parts.size() > at && parts[at] == "press") {
            xt::GamepadButton button{};
            uint32_t player = 0;
            const bool playerOk =
                parts.size() == at + 2 ||
                (parts.size() == at + 3 && parseFrame(parts[at + 2], player) && player < xt::kMaxPlayers);
            if (parts.size() < at + 2 || !gamepadButtonFromName(parts[at + 1], button) || !playerOk) {
                eventSpecError(
                    spec,
                    "a gamepad press is \"[<frame>] press <A|B|X|Y|Back|Guide|Start|LS|RS|LB|RB|Up|"
                    "Down|Left|Right> [<player 0-7>]\"");
                return false;
            }
            out.frame = frame;
            out.port = "Pad";
            out.id = parts[at + 1];
            out.player = static_cast<uint8_t>(player);
            return true;
        }
    }
    // T-193: `[<frame>] system <kind> [<value>]`.
    const size_t systemAt =
        !parts.empty() && parts[0] == "system" ? 0 : (parts.size() > 1 && parts[1] == "system" ? 1 : 2);
    if (systemAt < 2) {
        if (systemAt == 1 && !parseFrame(parts[0], out.frame)) {
            eventSpecError(spec, "'" + parts[0] + "' is not a frame number");
            return false;
        }
        if (parts.size() < systemAt + 2) {
            eventSpecError(spec, "a system event is \"[<frame>] system <kind> [<value>]\"");
            return false;
        }
        bool known = false;
        for (std::string_view kind : xt::flow::systemEventKinds()) {
            known = known || kind == parts[systemAt + 1];
        }
        if (!known) {
            eventSpecError(spec, "unknown system event '" + parts[systemAt + 1] +
                                     "' (loading, paused, resumed, disconnected, languageChanged, platform, "
                                     "inputDevice)");
            return false;
        }
        out.port = "System";
        out.id = parts[systemAt + 1];
        for (size_t i = systemAt + 2; i < parts.size(); ++i) {
            out.value += (i == systemAt + 2 ? "" : " ") + parts[i];
        }
        return true;
    }
    if (parts.size() >= 4 && parts[1] == "port" && parseFrame(parts[0], out.frame)) {
        // T-175: `<frame> port <name> <value...>` (the value may contain spaces).
        out.port = "Port";
        out.id = parts[2];
        size_t at = spec.find(parts[2], spec.find("port") + 4) + parts[2].size();
        while (at < spec.size() && (spec[at] == ' ' || spec[at] == '\t')) {
            ++at;
        }
        out.value = spec.substr(at);
        return true;
    }
    if (parts.size() != 2) {
        eventSpecError(spec, parts.size() < 2 ? "missing the frame or the target"
                                              : "too many words (no spaces inside the target)");
        return false;
    }
    if (!parseFrame(parts[0], out.frame)) {
        eventSpecError(spec, "'" + parts[0] + "' is not a frame number");
        return false;
    }
    if (parts[1].size() < 2 || parts[1][0] != '#') {
        eventSpecError(spec, "the target '" + parts[1] + "' does not start with '#'");
        return false;
    }
    const std::string& target = parts[1];
    size_t dot = target.find('.', 1);
    if (dot == std::string::npos) {
        eventSpecError(spec, "'" + target + "' has no '.<Event>'");
        return false;
    }
    out.id = target.substr(1, dot - 1);
    std::string port = target.substr(dot + 1);
    size_t eq = port.find('=');
    if (eq != std::string::npos) {
        out.value = port.substr(eq + 1);
        port = port.substr(0, eq);
    }
    if (out.id.empty()) {
        eventSpecError(spec, "'" + target + "' has an empty element id");
        return false;
    }
    if (port.rfind("state(", 0) == 0 && port.size() > 7 && port.back() == ')' && eq == std::string::npos) {
        // T-174: `state(<Name>[,on|off|toggle])`.
        std::string inner = port.substr(6, port.size() - 7);
        size_t comma = inner.find(',');
        out.value = inner.substr(0, comma);
        out.mode = comma == std::string::npos ? std::string("on") : inner.substr(comma + 1);
        if (out.value.empty() || (out.mode != "on" && out.mode != "off" && out.mode != "toggle")) {
            eventSpecError(spec, "'" + port + "' is not state(<Name>[,on|off|toggle])");
            return false;
        }
        out.port = "State";
        return true;
    }
    if (port != "Click" && port != "PointerEnter" && port != "PointerLeave" && port != "Focus" &&
        port != "Blur" && port != "Change") {
        eventSpecError(spec, "unknown event '" + port +
                                 "' (Click, PointerEnter, PointerLeave, Focus, Blur, Change, state(<Name>))");
        return false;
    }
    if (port == "Change" && eq == std::string::npos) {
        eventSpecError(spec, "Change needs a '=<value>' suffix");
        return false;
    }
    out.port = std::move(port);
    return true;
}

/// `--pointer "<frame> <x> <y> [move|down|up]"`; same error convention as parseEventSpec().
bool parsePointerSpec(const std::string& spec, ScriptedPointer& out) {
    std::vector<std::string> parts = splitWhitespace(spec);
    if ((parts.size() != 3 && parts.size() != 4) || !parseFrame(parts[0], out.frame)) {
        std::fprintf(stderr,
                     "xtframe-view: --pointer: expected \"<frame> <x> <y> [move|down|up]\", got "
                     "'%s'\n",
                     spec.c_str());
        return false;
    }
    out.x = static_cast<float>(std::atof(parts[1].c_str()));
    out.y = static_cast<float>(std::atof(parts[2].c_str()));
    if (parts.size() == 4) {
        if (parts[3] == "move") {
            out.kind = xt::InputEvent::PointerMove;
        } else if (parts[3] == "down") {
            out.kind = xt::InputEvent::PointerDown;
        } else if (parts[3] == "up") {
            out.kind = xt::InputEvent::PointerUp;
        } else {
            std::fprintf(stderr, "xtframe-view: --pointer: unknown kind '%s' (move, down, up)\n",
                         parts[3].c_str());
            return false;
        }
    }
    return true;
}

bool parseArgs(int argc, char** argv, Args& out) {
    // T-420: --version wins over every other argument, a malformed one included. T-426:
    // --list-fonts is the same kind of pre-scanned flag, after --version.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            out.version = true;
            return true;
        }
        if (std::string_view(argv[i]) == "--list-fonts") {
            out.listFonts = true;
        } else if (std::string_view(argv[i]) == "--list-fonts=names") {
            out.listFonts = true;
            out.listFontNames = true;
        }
    }
    if (out.listFonts) {
        return true;
    }
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const char* flag, std::string& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "xtframe-view: %s requires an argument\n", flag);
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--root") {
            if (!needValue("--root", out.root)) {
                return false;
            }
        } else if (arg == "--width") {
            std::string v;
            if (!needValue("--width", v)) {
                return false;
            }
            out.width = std::atoi(v.c_str());
        } else if (arg == "--height") {
            std::string v;
            if (!needValue("--height", v)) {
                return false;
            }
            out.height = std::atoi(v.c_str());
        } else if (arg == "--msaa") {
            std::string v;
            if (!needValue("--msaa", v)) {
                return false;
            }
            out.msaa = std::atoi(v.c_str());
        } else if (arg == "--screenshot") {
            if (!needValue("--screenshot", out.screenshot)) {
                return false;
            }
        } else if (arg == "--flow") {
            if (!needValue("--flow", out.flow)) {
                return false;
            }
        } else if (arg == "--project") {
            if (!needValue("--project", out.project)) {
                return false;
            }
        } else if (arg == "--ui") {
            if (!needValue("--ui", out.ui)) {
                return false;
            }
        } else if (arg == "--fit") {
            xt::shell::FitMode mode = xt::shell::FitMode::Native;
            if (!needValue("--fit", out.fit)) {
                return false;
            }
            if (!xt::shell::parseFitMode(out.fit, mode)) {
                std::fprintf(
                    stderr,
                    "xtframe-view: --fit: unknown mode '%s' (native, stretch, contain, cover, center)\n",
                    out.fit.c_str());
                return false;
            }
        } else if (arg == "--no-flow") {
            out.noFlow = true;
        } else if (arg == "--strict") {
            out.strict = true;
        } else if (arg == "--print-text") {
            std::string v;
            if (!needValue("--print-text", v)) {
                return false;
            }
            out.printText.push_back(std::move(v));
        } else if (arg == "--event") {
            std::string v;
            if (!needValue("--event", v)) {
                return false;
            }
            ScriptedEvent event;
            if (!parseEventSpec(v, event)) {
                return false;
            }
            out.events.push_back(std::move(event));
        } else if (arg == "--pointer") {
            std::string v;
            if (!needValue("--pointer", v)) {
                return false;
            }
            ScriptedPointer pointer;
            if (!parsePointerSpec(v, pointer)) {
                return false;
            }
            out.pointers.push_back(pointer);
        } else if (arg == "--dump-flow-values") {
            out.dumpFlowValues = true;
        } else if (arg == "--dump-ports") {
            if (!needValue("--dump-ports", out.dumpPorts)) {  // T-401.
                return false;
            }
        } else if (arg == "--log-ports") {
            out.logPorts = true;  // T-401.
        } else if (arg == "--dump-every") {
            std::string v;
            if (!needValue("--dump-every", v)) {
                return false;
            }
            out.dumpEvery = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
            out.dumpFlowValues = true;  // --dump-every implies --dump-flow-values.
        } else if (arg == "--gamepad") {
            out.gamepad = true;  // T-194.
        } else if (arg == "--players") {
            std::string v;
            if (!needValue("--players", v)) {
                return false;
            }
            if (!parseFrame(v, out.players) || out.players < 1 || out.players > xt::kMaxPlayers) {
                std::fprintf(stderr, "xtframe-view: --players: expected 1-8, got '%s'\n", v.c_str());
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            out.help = true;
            return true;  // Nothing else matters; main() prints the usage and exits 0.
        } else if (arg == "--headless") {
            out.headless = true;
        } else if (arg == "--renderer") {
            if (!needValue("--renderer", out.renderer)) {
                return false;
            }
        } else if (arg == "--transparent") {
            out.transparent = true;
        } else if (arg == "--scene") {
            if (!needValue("--scene", out.scene)) {
                return false;
            }
        } else if (arg == "--scene-animate") {
            out.sceneAnimate = true;
        } else if (arg == "--hidden") {
            out.hidden = true;
        } else if (arg == "--borderless") {
            out.borderless = true;  // T-376.
        } else if (arg == "--resize-border") {
            std::string v;
            if (!needValue("--resize-border", v)) {
                return false;
            }
            uint32_t n = 0;
            if (!parseFrame(v, n)) {
                std::fprintf(stderr, "xtframe-view: --resize-border: expected a pixel count, got '%s'\n",
                             v.c_str());
                return false;
            }
            out.resizeBorder = static_cast<int>(std::min<uint32_t>(n, 0x7FFFFFFFu));
        } else if (arg == "--no-live-resize") {
            out.liveResize = false;  // T-377.
        } else if (arg == "--fullscreen") {
            std::string v;
            if (!needValue("--fullscreen", v)) {  // T-398.
                return false;
            }
            if (v == "desktop") {
                out.fullscreen = 2;
            } else if (v == "exclusive") {
                out.fullscreen = 1;
            } else {
                std::fprintf(stderr, "xtframe-view: --fullscreen: expected desktop or exclusive, got '%s'\n",
                             v.c_str());
                return false;
            }
        } else if (arg == "--stats") {
            out.stats = true;
        } else if (arg == "--second-window") {
            if (!needValue("--second-window", out.secondWindow)) {  // T-384b.
                return false;
            }
        } else if (arg == "--second-display") {
            std::string v;
            if (!needValue("--second-display", v)) {
                return false;
            }
            uint32_t n = 0;
            if (!parseFrame(v, n) || n > 64) {
                std::fprintf(stderr, "xtframe-view: --second-display: expected a display index, got '%s'\n",
                             v.c_str());
                return false;
            }
            out.secondDisplay = static_cast<int>(n);
        } else if (arg == "--second-size") {
            std::string v;
            if (!needValue("--second-size", v)) {
                return false;
            }
            const size_t x = v.find('x');
            uint32_t w = 0;
            uint32_t h = 0;
            if (x == std::string::npos || !parseFrame(v.substr(0, x), w) || !parseFrame(v.substr(x + 1), h) ||
                w == 0 || h == 0 || w > 16384 || h > 16384) {
                std::fprintf(stderr, "xtframe-view: --second-size: expected WxH, got '%s'\n", v.c_str());
                return false;
            }
            out.secondWidth = static_cast<int>(w);
            out.secondHeight = static_cast<int>(h);
        } else if (arg == "--dump-commands") {
            out.dumpCommands = true;
        } else if (arg == "--bind-texture") {
            std::string v;
            if (!needValue("--bind-texture", v)) {
                return false;
            }
            size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= v.size()) {
                std::fprintf(stderr,
                             "xtframe-view: --bind-texture: expected \"<name>=<file.png>\", got '%s'\n",
                             v.c_str());
                return false;
            }
            out.bindTextures.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (arg == "--perf") {
            out.perf = true;
        } else if (arg == "--synth-pointer") {
            std::string v;
            if (!needValue("--synth-pointer", v)) {
                return false;
            }
            out.synthPointer = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--synth-ports") {
            std::string v;
            if (!needValue("--synth-ports", v)) {
                return false;
            }
            out.synthPorts = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--show-damage") {
            out.showDamage = true;
        } else if (arg == "--perf-csv") {
            if (!needValue("--perf-csv", out.perfCsv)) {
                return false;
            }
            out.perf = true;  // --perf-csv implies --perf.
        } else if (arg == "--frames") {
            std::string v;
            if (!needValue("--frames", v)) {
                return false;
            }
            out.frames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--warmup") {
            std::string v;
            if (!needValue("--warmup", v)) {
                return false;
            }
            out.warmup = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--warmup-dt") {
            std::string v;
            if (!needValue("--warmup-dt", v)) {
                return false;
            }
            out.warmupDt = static_cast<float>(std::atof(v.c_str()));
        } else if (arg == "--screenshot-every") {
            std::string v;
            if (!needValue("--screenshot-every", v)) {
                return false;
            }
            out.screenshotEvery = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (arg == "--device" || arg == "--safe" || arg == "--platform" || arg == "--input") {
            // T-176: Context::setDevice() inputs.
            std::string v;
            if (!needValue(arg.c_str(), v)) {
                return false;
            }
            bool ok = false;
            if (arg == "--device") {
                ok = xt::shell::parseDeviceAspect(v, out.device.aspect);
            } else if (arg == "--safe") {
                ok = xt::shell::parseSafeArea(v, out.device.safeArea);
                out.safeGiven = true;
            } else if (arg == "--platform") {
                ok = xt::shell::parseDeviceKind(v, out.device.platform);
            } else {
                ok = xt::shell::parseInputKind(v, out.device.input);
            }
            if (!ok) {
                std::fprintf(stderr, "xtframe-view: %s: invalid value '%s'\n", arg.c_str(), v.c_str());
                return false;
            }
        } else if (arg == "--lang") {
            if (!needValue("--lang", out.lang)) {
                return false;
            }
        } else if (arg == "--port") {
            std::string v;
            if (!needValue("--port", v)) {
                return false;
            }
            size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::fprintf(stderr, "xtframe-view: --port: expected <name>=<value>, got '%s'\n", v.c_str());
                return false;
            }
            out.ports.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (arg == "--play") {
            if (!needValue("--play", out.playTimeline)) {
                return false;
            }
        } else if (arg == "--seek") {
            std::string v;
            if (!needValue("--seek", v)) {
                return false;
            }
            char* end = nullptr;
            out.seekTime = std::strtof(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || out.seekTime < 0.0f) {
                std::fprintf(stderr, "xtframe-view: --seek: expected seconds >= 0, got '%s'\n", v.c_str());
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "xtframe-view: unknown option '%s'\n", arg.c_str());
            return false;
        } else {
            positional.push_back(arg);
        }
    }
    // T-213: --project <dir> is an alias of the input <dir> that also loads every UI.
    if (!out.project.empty()) {
        if (!positional.empty()) {
            std::fprintf(stderr, "xtframe-view: --project takes the place of the input; give one of them\n");
            return false;
        }
        positional.push_back(out.project);
        out.wholeProject = true;
    }
    if (positional.size() != 1) {
        return false;
    }
    if (out.seekTime >= 0.0f && out.playTimeline.empty()) {
        std::fprintf(stderr, "xtframe-view: --seek needs --play <timeline>\n");
        return false;
    }
    if (out.device.aspect > 0.0f && out.height > 0) {
        // T-176: --device W:H shapes the viewport (its width follows --height).
        out.width = static_cast<int>(static_cast<float>(out.height) * out.device.aspect + 0.5f);
    }
    out.input = positional.empty() ? std::string() : positional[0];
    return true;
}

void printDiagnostic(const xt::compile::Diagnostic& d);

/// T-213: FNV-1a 64 of `text` (the scratch Bundle's directory name).
uint64_t pathHash(const std::string& text) {
    uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

/// T-213 (Docs/design/22-bundle.md §22.11.2): packs the UI Project in `dir` into the scratch
/// Bundle `outDir` the way xtframe-browse does (no thumbnails, `--prune`, nothing written back into
/// the project). An old `ui-project.json` / `bundle.xtproj` is upgraded in memory only; the atlas is off
/// because this viewer's resolver does not know atlas cells. Prints the diagnostics; false (after its own
/// stderr line) when the project cannot be read or no UI could be packed.
bool packProject(const std::string& dir, const std::string& outDir, const std::string& standardSymbolDir,
                 const std::string& standardFontDir) {
    std::string error;
    xt::Result<xt::bundle::UiProject> loaded = xt::bundle::UiProject::load(dir, &error);
    if (!loaded.ok()) {
        std::fprintf(stderr, "xtframe-view: %s\n",
                     error.empty() ? xt::statusName(loaded.status()) : error.c_str());
        return false;
    }
    xt::bundle::UiProject project = std::move(loaded.value());
    project.legacy = false;
    project.publish.atlas.enabled = false;
    xt::bundle::PackOptions options;
    options.outDir = outDir;
    options.thumbnails = false;
    options.prune = true;
    options.writeAssetsManifest = false;
    // T-498b: the preset group resolves `xt-standard:` links and is the implied token layer (the
    // group's tokens, fonts and textures), as in xtframe-pack -- but nothing is copied into the
    // project (T-263f).
    options.standardSymbolDir = standardSymbolDir;
    options.copyStandardSymbols = false;
    options.standardFontDir = standardFontDir;  // T-527d
    const xt::bundle::PackReport report = xt::bundle::pack(project, options);
    for (const xt::compile::Diagnostic& d : report.diagnostics) {
        printDiagnostic(d);
    }
    {
        // T-263f: this pack copies no standard symbol into the project (copyStandardSymbols off),
        // so a project that was never opened in the editor or packed with xtframe-pack fails on
        // the missing files; say what fetches them. pack() prefixes a UI's diagnostics "<ui id>: ".
        std::vector<xt::tools::NoteDocument> documents;
        for (const xt::bundle::ProjectUi& ui : project.uis) {
            std::error_code ec;
            const fs::path doc = fs::absolute(
                xt::compile::pathFromUtf8(project.rootDir) / xt::compile::pathFromUtf8(ui.doc), ec);
            documents.push_back(xt::tools::NoteDocument{doc.parent_path(), ui.id + ": "});
        }
        xt::tools::noteMissingStandardSymbols(project, documents, report.diagnostics);
    }
    if (report.packedUis.empty()) {
        std::fprintf(stderr, "xtframe-view: failed to pack '%s'\n", dir.c_str());
        return false;
    }
    return true;
}

/// T-135/T-213: turns whatever the user named into the document/flow/root actually opened.
///
/// A Bundle (directory or `manifest.json`) is resolved through its manifest (`--ui` picks the UI);
/// a UI Project (directory, `.xtui`, `ui-project.json` or `bundle.xtproj`) is packed first (packProject())
/// and then opened as a Bundle; a `.xt` input picks up a sibling `<stem>.xtflow`. `--no-flow`
/// short-circuits every flow rule, including an explicit `--flow`. Returns false after printing
/// its own stderr line; the caller then exits 2 *without* printing the usage block (the input, not
/// the command line, is what was wrong) -- or 1 when `loadFailed` is set (a pack or manifest
/// error).
bool resolveInput(Args& args) {
    if (args.noFlow) {
        args.flow.clear();
    }

    const fs::path inputPath = pathFromUtf8(args.input);
    std::error_code ec;
    fs::path bundleDir;
    fs::path projectDir;
    // T-214: what UiProject::load() gets -- the directory, or the root file the user named (so an
    // explicit `ui-project.json` wins over a `.xtui` next to it).
    std::string projectInput;
    if (fs::is_directory(inputPath, ec)) {
        if (fs::is_regular_file(inputPath / "manifest.json", ec)) {
            bundleDir = inputPath;
        } else {
            projectDir = inputPath;  // UiProject::load() reports a directory that is neither.
            projectInput = pathToUtf8(projectDir);
        }
    } else if (inputPath.filename() == "manifest.json") {
        bundleDir = inputPath.parent_path().empty() ? fs::path(".") : inputPath.parent_path();
    } else if (inputPath.extension() == ".xtb" && fs::is_regular_file(inputPath, ec)) {
        bundleDir = inputPath;  // T-500: Bundle::open() unpacks it; bundle.dir() is the unpacked copy.
    } else if (xt::bundle::isUiProjectFilePath(args.input)) {
        projectDir = inputPath.parent_path().empty() ? fs::path(".") : inputPath.parent_path();
        projectInput = args.input;
    } else if (args.wholeProject) {
        std::fprintf(stderr, "xtframe-view: --project needs a UI Project or Bundle directory, got '%s'\n",
                     args.input.c_str());
        return false;
    }

    // T-139's sibling convention survives packing: a UI whose project entry names no flow still
    // gets `<ui dir>/<stem>.xtflow` from the UI Project's sources.
    xt::bundle::UiProject source;
    auto siblingFlow = [&source](const std::string& uiId) {
        const xt::bundle::ProjectUi* entry = source.ui(uiId);
        if (entry == nullptr) {
            return std::string();
        }
        const fs::path doc = pathFromUtf8(source.rootDir) / pathFromUtf8(entry->doc);
        const fs::path sibling = fs::path(doc).replace_extension(".xtflow");
        std::error_code siblingEc;
        return fs::is_regular_file(sibling, siblingEc) ? pathToUtf8(sibling.lexically_normal())
                                                       : std::string();
    };
    if (!projectDir.empty()) {
        std::string id;
        {
            xt::Result<xt::bundle::UiProject> peek = xt::bundle::UiProject::load(projectInput);
            id = peek.ok() && !peek.value().id.empty() ? peek.value().id : std::string("project");
            if (peek.ok()) {
                source = std::move(peek.value());
            }
        }
        const std::string absolute = genericPathToUtf8(fs::absolute(projectDir, ec).lexically_normal());
        char hash[17];
        std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(pathHash(absolute)));
        bundleDir = fs::temp_directory_path(ec) / "xtframe-view" / pathFromUtf8(id + "-" + hash);
        args.packSource = projectInput;
        if (!packProject(args.packSource, pathToUtf8(bundleDir), args.standardSymbolDir,
                         args.standardFontDir)) {
            args.loadFailed = true;
            return false;
        }
        std::fprintf(stderr, "xtframe-view: packed %s -> %s\n", genericPathToUtf8(projectDir).c_str(),
                     genericPathToUtf8(bundleDir).c_str());
    }

    if (!bundleDir.empty()) {
        std::string error;
        xt::Result<std::unique_ptr<xt::bundle::Bundle>> opened =
            xt::bundle::Bundle::open(pathToUtf8(bundleDir), nullptr, &error);
        if (!opened.ok()) {
            std::fprintf(stderr, "xtframe-view: %s\n",
                         error.empty() ? xt::statusName(opened.status()) : error.c_str());
            args.loadFailed = true;
            return false;
        }
        args.bundle = std::shared_ptr<xt::bundle::Bundle>(std::move(opened.value()));  // T-555.
        const xt::bundle::Bundle& bundle = *args.bundle;
        bundleDir = pathFromUtf8(std::string(bundle.dir()));  // T-500: the unpacked directory for an .xtb.
        // T-207 (Docs/design/17-editor.md §17.4.1 item 5): without --safe, the safe area is the
        // manifest's spec.safeArea (`console` for --platform console), as in the editor and a host.
        // T-210 (25 §25.4.1 item 2): its spec.logicalHeight gives uiscale = output height / it.
        if (const xt::bundle::BundleSpec* spec = bundle.spec()) {
            if (!args.safeGiven) {
                const float safe = args.device.platform == xt::DeviceKind::Console ? spec->safeAreaConsole
                                                                                   : spec->safeAreaDefault;
                args.device.safeArea = xt::Insets{safe, safe, safe, safe};
            }
            args.logicalHeight = spec->logicalHeight;
            args.uiscaleRule = spec->uiscaleRule;      // T-241
            args.uiscaleLimits = spec->uiscaleLimits;  // T-345
        }

        std::string ids;
        for (const xt::bundle::UiInfo& entry : bundle.uis()) {
            if (!ids.empty()) {
                ids += ", ";
            }
            ids += entry.id;
        }
        const xt::bundle::UiInfo* ui = nullptr;
        if (!args.ui.empty()) {
            ui = bundle.ui(args.ui);
            if (ui == nullptr) {
                std::fprintf(stderr, "xtframe-view: bundle has no UI '%s': %s\n", args.ui.c_str(),
                             ids.c_str());
                return false;
            }
        } else if (bundle.uis().size() == 1 || (args.wholeProject && !bundle.uis().empty())) {
            ui = &bundle.uis().front();
        } else {
            std::fprintf(stderr, "xtframe-view: bundle has %zu UIs; pass --ui <id>: %s\n",
                         bundle.uis().size(), ids.c_str());
            return false;
        }

        args.bundleDir = pathToUtf8(bundleDir);
        args.input = pathToUtf8((bundleDir / pathFromUtf8(ui->file)).lexically_normal());
        args.mainUi = ui->id;
        if (args.fit.empty()) {
            xt::shell::FitMode mode = xt::shell::FitMode::Native;
            if (xt::shell::parseFitMode(ui->fit, mode)) {
                args.fit = ui->fit;
            } else {
                std::fprintf(stderr, "xtframe-view: UI '%s' has unknown fit '%s'; using native\n",
                             ui->id.c_str(), ui->fit.c_str());
            }
        }
        args.designWidth = static_cast<int>(ui->size.x);
        args.designHeight = static_cast<int>(ui->size.y);
        if (args.flow.empty() && !args.noFlow && !ui->flow.empty()) {
            args.flow = pathToUtf8((bundleDir / pathFromUtf8(ui->flow)).lexically_normal());
            args.flowAuto = true;
        }
        if (args.flow.empty() && !args.noFlow) {
            args.flow = siblingFlow(ui->id);
            args.flowAuto = !args.flow.empty();
        }
        if (args.root.empty()) {
            args.root = pathToUtf8(bundleDir);
        }
        args.assetRoots = {pathToUtf8(bundleDir)};
        if (args.wholeProject) {
            for (const xt::bundle::UiInfo& other : bundle.uis()) {
                if (other.id == ui->id) {
                    continue;
                }
                args.otherUis.push_back(
                    {other.id, pathToUtf8((bundleDir / pathFromUtf8(other.file)).lexically_normal()),
                     other.flow.empty()
                         ? siblingFlow(other.id)
                         : pathToUtf8((bundleDir / pathFromUtf8(other.flow)).lexically_normal())});
            }
            if (!bundle.globalFlow().empty()) {
                args.globalFlow =
                    pathToUtf8((bundleDir / pathFromUtf8(bundle.globalFlow())).lexically_normal());
            }
        }
        return true;
    }

    if (inputPath.extension() == ".xt" && args.flow.empty() && !args.noFlow) {
        const fs::path sibling = fs::path(inputPath).replace_extension(".xtflow");
        if (fs::exists(sibling, ec)) {
            args.flow = pathToUtf8(sibling.lexically_normal());
            args.flowAuto = true;
        }
    }
    return true;
}

/// T-150: binds every --bind-texture PNG on `assets` as `engine://<name>`. With `upload`
/// (windowed mode, a GL context is current) the pixels become a GL texture the renderer never
/// created, exactly like an engine-owned one; without it (headless) the pixels are bound for the
/// software renderer instead (AssetResolver::setExternalPixels, T-191).
/// Returns false (after its own stderr line) when a PNG cannot be read.
bool bindExternalTextures(const Args& args, xt::AssetResolver& assets, bool upload) {
    for (const auto& [name, file] : args.bindTextures) {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        if (!xt::sw::readPng(file, rgba, width, height) || width <= 0 || height <= 0) {
            std::fprintf(stderr, "xtframe-view: --bind-texture: failed to read '%s'\n", file.c_str());
            return false;
        }
        uint32_t texture = 0;
#ifdef XT_SHELL_SW
        upload = false;  // T-240: the SW presentation path has no GL context; CPU pixels instead.
#else
        if (upload) {
            xt::gl::GLuint tex = 0;
            xt::gl::glGenTextures(1, &tex);
            xt::gl::glBindTexture(xt::gl::GL_TEXTURE_2D, tex);
            xt::gl::glTexParameteri(xt::gl::GL_TEXTURE_2D, xt::gl::GL_TEXTURE_MIN_FILTER,
                                    static_cast<xt::gl::GLint>(xt::gl::GL_LINEAR));
            xt::gl::glTexParameteri(xt::gl::GL_TEXTURE_2D, xt::gl::GL_TEXTURE_MAG_FILTER,
                                    static_cast<xt::gl::GLint>(xt::gl::GL_LINEAR));
            xt::gl::glTexParameteri(xt::gl::GL_TEXTURE_2D, xt::gl::GL_TEXTURE_WRAP_S,
                                    static_cast<xt::gl::GLint>(xt::gl::GL_CLAMP_TO_EDGE));
            xt::gl::glTexParameteri(xt::gl::GL_TEXTURE_2D, xt::gl::GL_TEXTURE_WRAP_T,
                                    static_cast<xt::gl::GLint>(xt::gl::GL_CLAMP_TO_EDGE));
            xt::gl::glPixelStorei(xt::gl::GL_UNPACK_ALIGNMENT, 1);
            xt::gl::glTexImage2D(xt::gl::GL_TEXTURE_2D, 0, static_cast<xt::gl::GLint>(xt::gl::GL_RGBA8),
                                 width, height, 0, xt::gl::GL_RGBA, xt::gl::GL_UNSIGNED_BYTE, rgba.data());
            xt::gl::glPixelStorei(xt::gl::GL_UNPACK_ALIGNMENT, 4);
            texture = tex;
        }
#endif
        // The PNG's pixels are straight alpha: the flag makes the GL backend premultiply while
        // sampling (Shading::TextureStraight), which is also the path an engine texture takes.
        assets.setExternalTexture(name, texture, width, height, /*straightAlpha=*/true);
        if (!upload) {
            // T-191: the software renderer draws a binding's CPU pixels; they live until exit.
            static std::deque<xt::ImageData> cpuPixels;
            xt::ImageData& image = cpuPixels.emplace_back();
            image.width = width;
            image.height = height;
            image.rgba = std::move(rgba);
            assets.setExternalPixels(name, &image);
        }
        std::printf("xtframe-view: bound engine://%s = '%s' (%dx%d%s)\n", name.c_str(), file.c_str(), width,
                    height, upload ? "" : ", CPU pixels");
    }
    return true;
}

/// The usage block, on `out` (stdout for --help, stderr for an argument error).
void printUsage(std::FILE* out) {
    std::fprintf(
        out,
        "usage: xtframe-view <file.xtir | file.json | file.xt | bundle-dir | manifest.json |\n"
        "                     ui-project-dir | name.xtui | ui-project.json | bundle.xtproj> [options]\n"
        "\n"
        "  A UI Project (name.xtui, or an old ui-project.json / bundle.xtproj) is packed to\n"
        "  %%TEMP%%/xtframe-view/<id>-<hash> first and that Bundle is opened.\n"
        "\n"
        "  --root DIR              asset root (default: the input file's directory)\n"
        "  --width W --height H    viewport size in logical pixels (default 1280x720)\n"
        "  --msaa N                MSAA samples, 4 or 1 (none), for the windowed backend and\n"
        "                          --renderer gl|wgpu (default 4)\n"
        "  --screenshot PATH       render one frame (or, with --headless, the last one) to a PNG\n"
        "  --flow <file.xtflow>    load and tick a signal graph against the document\n"
        "                          (default: the input's own .xtflow, or a bundle UI's)\n"
        "  --ui <id>               which UI of a Bundle / UI Project input to open\n"
        "  --fit MODE              windowed placement: native, stretch, contain, cover, center\n"
        "                          (default: the bundle UI's `fit`, else native)\n"
        "  --no-flow               do not load any signal graph\n"
        "  --strict                compile with strict checks; print the document's runtime\n"
        "                          warnings when a headless run ends\n"
        "  --project DIR           same as the input DIR (a UI Project or Bundle), plus every\n"
        "                          other UI, their flows and the global flow\n"
        "  --headless              no window/GL context; software-render --frames frames\n"
        "  --renderer sw|gl|wgpu   the renderer (built in: see --version; default: sw with\n"
        "                          --headless, else the shell's);\n"
        "                          with --headless gl / wgpu render offscreen on the GPU\n"
        "  --transparent           headless: clear to transparent instead of white, so a\n"
        "                          --screenshot keeps the UI's alpha (what a host composites)\n"
        "  --scene PNG             draw the UI onto this image as the host scene (Direct target\n"
        "                          mode, 23 23.aa: blend modes and backdrop blurs see it; editor-only\n"
        "                          elements are stripped); windowed and headless\n"
        "  --scene-animate         with --scene: scroll the scene 1 px per frame (wrapping)\n"
        "  --hidden                create the window hidden (a windowed --screenshot from automation)\n"
        "  --borderless            no OS window frame: `app-region: drag` moves the window, the\n"
        "                          edges resize it; out events winMinimize / winMaximize / winClose\n"
        "                          minimize, maximize-or-restore and close it\n"
        "  --resize-border N       with --borderless: edge resize band in pixels (default 8, 0 = off)\n"
        "  --no-live-resize        do not redraw while the window edge is being dragged\n"
        "  --fullscreen MODE       desktop (borderless, desktop resolution) or exclusive\n"
        "  --second-window <file>  also open a second window showing another .xt/.xtir/.json file\n"
        "  --second-display N      with --second-window: centre it on display N\n"
        "  --second-size WxH       with --second-window: its size (default: the main window's)\n"
        "  --stats                 log ms/frame, render ms and damage area once per second (windowed)\n"
        "  --frames N              headless frame count (default 60)\n"
        "  --print-text <id>       print `text=<element text>` when a headless run ends "
        "(repeatable)\n"
        "  --event \"<frame> #<id>.<Port>[=<value>]\"\n"
        "                          drive "
        "Click/PointerEnter/PointerLeave/Focus/Blur/Change/state(<Name>) on an\n"
        "                          element before frame <frame> (repeatable); or\n"
        "                          \"<frame> port <name> <value>\" sets a declared port;\n"
        "                          \"[<frame>] system <kind> [<value>]\" posts a system event\n"
        "  --port <name>=<value>   set a declared port before the first frame (repeatable;\n"
        "                          a list/struct port takes JSON: items='[{\"name\":\"A\"}]';\n"
        "                          a vec2/vec3 port x,y[,z] or pos=[1,2])\n"
        "  --pointer \"<frame> <x> <y> [move|down|up]\"\n"
        "                          push one raw pointer event before frame <frame> (repeatable)\n"
        "  --dump-flow-values      print `<node>.<port> = <value>` for the --flow graph at the end\n"
        "  --dump-every N          also print that block every N frames (implies the above)\n"
        "  --dump-commands         print the last headless frame's command list\n"
        "  --dump-ports FILE       headless: write the main UI's declared ports (name, type,\n"
        "                          direction, default) to FILE as JSON before the first frame\n"
        "  --log-ports             headless: print `port <kind> <name> = <value> (frame N)` for\n"
        "                          every port change / emit / list change of the main UI\n"
        "  --perf                  frame tracing; --perf-csv <file> writes one row per frame\n"
        "  --synth-pointer N       measurement: N PointerMove events per frame along the diagonal\n"
        "  --synth-ports N         measurement: write the first N document-level number ports\n"
        "                          every frame (a different 0..100 triangle wave each)\n"
        "  --show-damage           outline the dirty rectangles (needs --headless)\n"
        "  --bind-texture <name>=<file.png>\n"
        "                          bind the PNG as the host texture `engine://<name>` (repeatable;\n"
        "                          --headless binds its CPU pixels)\n"
        "  --play <timeline>       play the document's root @timeline before the first frame\n"
        "  --seek <seconds>        then seek it to that time (needs --play)\n"
        "  --device W:H            device aspect ratio for @media; viewport width = height x W/H\n"
        "  --safe F | T,R,B,L      safe-area insets as fractions of the viewport (0..0.5)\n"
        "  --platform KIND         desktop, console or handheld (@media (platform))\n"
        "  --input KIND            mouse, gamepad or touch (@media (input))\n"
        "  --lang TAG              show @key text in this language (strings/<lang>.csv)\n"
        "  --event \"[<frame>] press <button> [<player>]\"\n"
        "                          press a gamepad button (A B X Y Back Guide Start LS RS LB RB\n"
        "                          Up Down Left Right) for a local player (default 0)\n"
        "  --gamepad               open SDL game controllers (windowed mode), one player each\n"
        "  --players N             local players 1-8: gamepad players 0..N-1 (and the slot limit)\n"
        "  --help, -h              print this text and exit 0\n"
        "  --version               print the version, IR format and behavior version and exit 0\n"
        "  --list-fonts            print the system fonts as `family<TAB>path` lines and exit 0\n"
        "  --list-fonts=names      print the system font family names, one per line, and exit 0\n"
        "\n"
        "exit codes: 0 success, 1 load/write error, 2 argument error\n");
}

/// Docs/design/21-perf-tracing.md §21.3: the end-of-run summary table, on stdout so CI and an AI
/// agent can read it. Times are milliseconds; `render`/`effects`/`text` come from the backend and
/// are CPU time (the GL backend never calls glFinish).
/// `damageAreaPercent` < 0 means "damage was not computed this run", which hides that row.
void printPerfSummary(const xt::PerfSummary& s, float damageAreaPercent = -1.0f) {
    std::printf("xtframe-view: perf summary over %u frame(s), fps avg %.1f\n", s.frames, s.fpsAvg);
    if (s.frames == 0) {
        return;
    }
    std::printf("  %-12s %10s %10s\n", "phase", "avg ms", "max ms");
    auto row = [](const char* name, float avg, float max) {
        std::printf("  %-12s %10.3f %10.3f\n", name, avg, max);
    };
    row("update", s.avg.updateMs, s.max.updateMs);
    row("  input", s.avg.inputMs, s.max.inputMs);
    row("  style", s.avg.styleMs, s.max.styleMs);
    row("  layout", s.avg.layoutMs, s.max.layoutMs);
    row("  animate", s.avg.animateMs, s.max.animateMs);
    row("  transform", s.avg.transformMs, s.max.transformMs);
    row("paint", s.avg.paintMs, s.max.paintMs);
    row("render", s.avg.renderMs, s.max.renderMs);
    row("  effects", s.avg.effectsMs, s.max.effectsMs);
    row("  text", s.avg.textMs, s.max.textMs);
    row("damage", s.avg.damageMs, s.max.damageMs);
    row("flow", s.avg.flowMs, s.max.flowMs);
    // T-475 (21 §21.6): exclusive flow stages; below behavior 1 they also run inside input.
    row("  deliver", s.avg.flowDeliverMs, s.max.flowDeliverMs);
    row("  eval", s.avg.flowEvalMs, s.max.flowEvalMs);
    row("  sink", s.avg.flowSinkMs, s.max.flowSinkMs);
    row("  poll", s.avg.flowPollMs, s.max.flowPollMs);
    // SW backend breakdown (T-82); all zero for the GL backend.
    row("  sw:path", s.avg.swPathMs, s.max.swPathMs);
    row("  sw:raster", s.avg.swRasterMs, s.max.swRasterMs);
    row("  sw:span", s.avg.swSpanMs, s.max.swSpanMs);
    // T-128's breakdown of sw:span. sw:spanClip overlaps the two paint-kind rows (it splits the
    // same time by clip state), and sw:spanImg is a sibling of sw:span, not a part of it.
    row("   spanSolid", s.avg.swSpanSolidMs, s.max.swSpanSolidMs);
    row("   spanGrad", s.avg.swSpanGradientMs, s.max.swSpanGradientMs);
    row("   spanClip*", s.avg.swSpanClippedMs, s.max.swSpanClippedMs);
    row("  sw:spanImg", s.avg.swSpanImageMs, s.max.swSpanImageMs);
    row("  sw:clip", s.avg.swClipMs, s.max.swClipMs);
    row("  sw:layer", s.avg.swLayerMs, s.max.swLayerMs);
    row("  sw:fxBlur", s.avg.swFxBlurMs, s.max.swFxBlurMs);
    row("  sw:fxDilate", s.avg.swFxDilateMs, s.max.swFxDilateMs);
    row("  sw:fxSdf", s.avg.swFxSdfMs, s.max.swFxSdfMs);
    row("  sw:fxPix", s.avg.swFxPixMs, s.max.swFxPixMs);
    if (damageAreaPercent >= 0.0f) {
        // T-98: the share of the viewport a host would have had to upload, averaged over the run.
        std::printf("  %-12s %10.1f %%\n", "damage area", static_cast<double>(damageAreaPercent));
    }
    std::printf("  %-12s %10s %10s\n", "workload", "avg", "max");
    auto count = [](const char* name, uint32_t avg, uint32_t max) {
        std::printf("  %-12s %10u %10u\n", name, avg, max);
    };
    count("styled", s.avg.styledElements, s.max.styledElements);
    count("laidOut", s.avg.laidOutElements, s.max.laidOutElements);
    count("animations", s.avg.activeAnimations, s.max.activeAnimations);
    count("transitions", s.avg.activeTransitions, s.max.activeTransitions);
    count("commands", s.avg.commands, s.max.commands);
    count("layers", s.avg.layers, s.max.layers);
    count("effectLayers", s.avg.effectLayers, s.max.effectLayers);
    count("projections", s.avg.projections, s.max.projections);
    count("glyphs", s.avg.glyphs, s.max.glyphs);
    count("pathPoints", s.avg.pathPoints, s.max.pathPoints);
    count("modifierVertices", s.avg.modifierVertices, s.max.modifierVertices);
    count("skinnedPoints", s.avg.skinnedPoints, s.max.skinnedPoints);  // T-244
    count("rigBones", s.avg.rigBones, s.max.rigBones);                 // T-244
    count("imageDraws", s.avg.imageDraws, s.max.imageDraws);
    count("imageMeshes", s.avg.imageMeshes, s.max.imageMeshes);
    count("meshTriangles", s.avg.meshTriangles, s.max.meshTriangles);
    count("spans", s.avg.spansPerFrame, s.max.spansPerFrame);
    count("spanPixels", s.avg.spanPixelsPerFrame, s.max.spanPixelsPerFrame);
    count("arenaNodes", s.avg.arenaNodes, s.max.arenaNodes);
    count("commandBytes", s.avg.commandBytes, s.max.commandBytes);
    // T-343 (Docs/design/23-host-integration.md §23.aa, 21 §21.1): Direct target mode.
    count("bands", s.avg.bands, s.max.bands);
    count("bandRedraws", s.avg.bandRedraws, s.max.bandRedraws);
    count("backdropCopyPx", s.avg.backdropCopyPx, s.max.backdropCopyPx);
    count("pyramidLevels", s.avg.pyramidLevels, s.max.pyramidLevels);
    count("directFallback", s.avg.directFallback, s.max.directFallback);
    count("setStyle", s.avg.setStyleCalls, s.max.setStyleCalls);  // T-389
    count("flowSignals", s.avg.flowSignals, s.max.flowSignals);   // T-475
    count("flowCoalesced", s.avg.flowSignalsCoalesced, s.max.flowSignalsCoalesced);
    count("flowEvals", s.avg.flowNodeEvals, s.max.flowNodeEvals);
    count("flowSinkWrites", s.avg.flowSinkWrites, s.max.flowSinkWrites);
    count("flowFanOut", s.avg.flowFanOutEdges, s.max.flowFanOutEdges);
    count("flowSinkTyped", s.avg.flowSinkTyped, s.max.flowSinkTyped);  // T-477
    count("flowSinkText", s.avg.flowSinkText, s.max.flowSinkText);
    count("flowAllocs", s.avg.flowAllocs, s.max.flowAllocs);
}

/// T-343: `--scene` strips the editor-only subtree (the reference frame under a HUD) so the
/// scene image is what lies under the UI, as in the game.
bool g_stripEditorOnly = false;
bool g_strict = false;  // T-392: --strict.

/// T-343: `src` (`sw x sh` straight RGBA8) resampled bilinearly to `dw x dh`.
void resampleRgba(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh,
                  std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4u, 0);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    auto at = [&](int x, int y, int c) {
        return static_cast<float>(
            src[(static_cast<size_t>(y) * static_cast<size_t>(sw) + static_cast<size_t>(x)) * 4u +
                static_cast<size_t>(c)]);
    };
    for (int y = 0; y < dh; ++y) {
        const float fy =
            (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, sh - 1);
        const int y1 = std::clamp(y0 + 1, 0, sh - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (int x = 0; x < dw; ++x) {
            const float fx =
                (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, sw - 1);
            const int x1 = std::clamp(x0 + 1, 0, sw - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            uint8_t* o =
                out.data() + (static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)) * 4u;
            for (int c = 0; c < 4; ++c) {
                const float top = at(x0, y0, c) + (at(x1, y0, c) - at(x0, y0, c)) * tx;
                const float bottom = at(x0, y1, c) + (at(x1, y1, c) - at(x0, y1, c)) * tx;
                o[c] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

/// T-343: reads `--scene`'s PNG; false (after its own stderr line) when it cannot be read.
bool loadScene(const std::string& file, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (!xt::sw::readPng(file, rgba, width, height) || width <= 0 || height <= 0) {
        std::fprintf(stderr, "xtframe-view: --scene: failed to read '%s'\n", file.c_str());
        return false;
    }
    return true;
}

/// `--perf-csv`: one row per frame, written as the frames are recorded so a run longer than the
/// tracker's 240-frame history still produces every row.
class PerfCsvWriter {
public:
    /// Opens `path` and writes the header; ok() is false when the file could not be opened.
    explicit PerfCsvWriter(const std::string& path) : m_out(pathFromUtf8(path), std::ios::binary) {
        if (!m_out) {
            return;
        }
        m_out << "frameIndex";
#define XT_VIEW_CSV_HEAD(name) m_out << "," << #name;
        XT_FRAME_STATS_FLOAT_FIELDS(XT_VIEW_CSV_HEAD)
        XT_FRAME_STATS_UINT_FIELDS(XT_VIEW_CSV_HEAD)
#undef XT_VIEW_CSV_HEAD
        m_out << "\n";
    }
    /// True when the file is open and rows are being written.
    bool ok() const { return static_cast<bool>(m_out); }
    /// Writes every not-yet-written frame in `tracker`'s history, oldest first.
    void writeNewFrames(const xt::PerfTracker& tracker) {
        if (!m_out) {
            return;
        }
        for (const xt::FrameStats& f : tracker.history()) {
            if (f.frameIndex < m_written) {
                continue;
            }
            char buffer[32];
            m_out << f.frameIndex;
#define XT_VIEW_CSV_FLOAT(name)                                                 \
    std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(f.name)); \
    m_out << "," << buffer;
            XT_FRAME_STATS_FLOAT_FIELDS(XT_VIEW_CSV_FLOAT)
#undef XT_VIEW_CSV_FLOAT
#define XT_VIEW_CSV_UINT(name) m_out << "," << f.name;
            XT_FRAME_STATS_UINT_FIELDS(XT_VIEW_CSV_UINT)
#undef XT_VIEW_CSV_UINT
            m_out << "\n";
            m_written = f.frameIndex + 1;
        }
    }

private:
    std::ofstream m_out;
    uint32_t m_written = 0;  // frameIndex of the next row to write.
};

/// --show-damage: outlines every dirty rectangle with a 1 px red border in the software
/// backend's RGBA8 output (a `full` frame outlines the whole target). Drawn after the frame is
/// rendered, so it is an overlay and never feeds back into the next frame's diff -- the buffer is
/// cleared before every render.
void drawDamageOverlay(uint8_t* rgba, int width, int height, int strideBytes, const xt::DamageList& d) {
    auto plot = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        uint8_t* px =
            rgba + static_cast<size_t>(y) * static_cast<size_t>(strideBytes) + static_cast<size_t>(x) * 4;
        px[0] = 255;
        px[1] = 0;
        px[2] = 0;
        px[3] = 255;
    };
    auto outline = [&](int x0, int y0, int x1, int y1) {  // [x0, x1) x [y0, y1)
        for (int x = x0; x < x1; ++x) {
            plot(x, y0);
            plot(x, y1 - 1);
        }
        for (int y = y0; y < y1; ++y) {
            plot(x0, y);
            plot(x1 - 1, y);
        }
    };
    if (d.full) {
        outline(0, 0, width, height);
        return;
    }
    for (const xt::Rect& r : d.rects) {
        outline(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.x + r.w),
                static_cast<int>(r.y + r.h));
    }
}

/// Share of `viewport`'s area covered by `d` (1.0 for a `full` frame), for the --perf table's
/// "damage area" row.
float damageAreaFraction(const xt::DamageList& d, const xt::Rect& viewport) {
    float total = viewport.w * viewport.h;
    if (total <= 0) {
        return 0.0f;
    }
    if (d.full) {
        return 1.0f;
    }
    float covered = 0.0f;
    for (const xt::Rect& r : d.rects) {
        covered += r.w * r.h;
    }
    return covered / total;
}

void printDiagnostic(const xt::compile::Diagnostic& d) {
    const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
    const std::string& file = d.file.empty() ? std::string("<input>") : d.file;
    if (d.line == 0 && d.col == 0) {
        std::fprintf(stderr, "%s: %s: %s\n", file.c_str(), level, d.message.c_str());
    } else {
        std::fprintf(stderr, "%s:%d:%d: %s: %s\n", file.c_str(), d.line, d.col, level, d.message.c_str());
    }
}

/// Compiles (`.json`) or reads (`.xtir`) `path` into `ctx`, replacing `handle` (unloaded first, if
/// valid). Returns false on failure, leaving `handle`/`ctx` untouched (diagnostics/errors already
/// printed to stderr).
bool loadInto(xt::Context& ctx, const std::string& path, const std::string& rootDir,
              const std::vector<std::string>& searchDirs, xt::DocumentHandle& handle) {
    xt::Result<xt::DocumentHandle> result{xt::Result<xt::DocumentHandle>::error(xt::Status::kInternal)};
    std::vector<uint8_t> compiled;  // Kept alive until loadDocument (takeOwnership copies it) returns.

    const fs::path inputPath = pathFromUtf8(path);
    if (inputPath.extension() == ".json" || inputPath.extension() == ".xt") {
        xt::compile::CompileOptions options;
        options.rootDir = rootDir;
        options.searchDirs = searchDirs;
        options.stripEditorOnly = g_stripEditorOnly;  // T-343: --scene shows the published UI.
        options.strict = g_strict;                    // T-392
        // T-56: `.xt` source (Docs/tasks/T-56.md's `xtframe-view --flow` CTest runs against
        // Examples/desktop_app/ui/app.xt directly, not a pre-compiled `.xtir`), compiled at
        // runtime the same way Examples/engine_integration/main.cpp does.
        xt::compile::CompileResult compileResult = inputPath.extension() == ".json"
                                                       ? xt::compile::compileLottie(path, options)
                                                       : xt::compile::compileDocument(path, options);
        for (const xt::compile::Diagnostic& d : compileResult.diagnostics) {
            printDiagnostic(d);
        }
        if (!compileResult.ok()) {
            // T-263f: a UI Project's document whose standard symbols were never fetched fails
            // on the missing files; say what fetches them (this viewer writes nothing there).
            xt::bundle::UiProject project;
            if (inputPath.extension() == ".xt" && xt::tools::enclosingUiProject(inputPath, project)) {
                std::error_code ec;
                const xt::tools::NoteDocument document{fs::absolute(inputPath, ec).parent_path(),
                                                       std::string()};
                xt::tools::noteMissingStandardSymbols(project, {&document, 1}, compileResult.diagnostics);
            }
            std::fprintf(stderr, "xtframe-view: failed to compile '%s'\n", path.c_str());
            return false;
        }
        compiled = std::move(compileResult.ir);
        result = ctx.loadDocument(xt::IrBlob{compiled.data(), compiled.size()}, /*takeOwnership=*/true);
    } else {
        result = ctx.loadDocumentFile(path);
    }

    if (!result.ok()) {
        std::fprintf(stderr, "xtframe-view: failed to load '%s' (%s)\n", path.c_str(),
                     xt::statusName(result.status()));
        return false;
    }

    if (handle != 0) {
        ctx.unloadDocument(handle);
    }
    handle = result.value();
    return true;
}

/// Every Document element with a non-zero defaultAnimation() instance (Docs/tasks/T-20.md
/// "xtframe-view" keys).
std::vector<xt::ElementRef> animatedElements(xt::Document& doc) {
    std::vector<xt::ElementRef> out;
    for (uint32_t i = 0;; ++i) {
        xt::ElementRef ref = doc.elementAt(i);
        if (!ref.valid()) {
            break;
        }
        if (doc.defaultAnimation(ref) != 0) {
            out.push_back(ref);
        }
    }
    return out;
}

float frameRateFor(xt::Document& doc, xt::ElementRef anim) {
    const xt::SceneInstance* inst = doc.sceneInstance(anim);
    if (inst == nullptr) {
        return 30.0f;
    }
    xt::TimelineId timelineId = doc.sceneTimeline(inst->scene);
    const xt::Timeline* timeline = doc.animator().timeline(timelineId);
    if (timeline == nullptr || timeline->frameRate <= 0) {
        return 30.0f;
    }
    return timeline->frameRate;
}

/// Pushes one raw pointer InputEvent (T-125's --pointer) into `ctx`'s queue; Context::update()
/// dispatches it into the foreground Document before that frame's own update.
void pushPointer(xt::Context& ctx, xt::InputEvent::Kind kind, xt::Vec2 pos) {
    xt::InputEvent event;
    event.kind = kind;
    event.pos = pos;
    event.button = xt::PointerButton::Left;
    ctx.pushInput(event);
}

/// Applies one --event against `doc`. Click/PointerEnter/PointerLeave go through Context's input
/// queue (so Document::dispatchInput does the hit testing and the 08.4 state machine);
/// Focus/Blur call Document::setFocus directly and Change calls ElementRef::setValue, whose own
/// Change dispatch is the event. A missing id is reported and skipped.
/// T-175: sets declared port `name` from text; one warning when it fails.
void setPortText(xt::Document& doc, const std::string& name, const std::string& value) {
    xt::PortId port = doc.lookupPort(name);
    if (port == xt::kInvalidPort) {
        std::fprintf(stderr, "xtframe-view: port: no port '%s' in the document\n", name.c_str());
        return;
    }
    if (doc.setPortText(port, value) != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-view: port: '%s' does not accept '%s'\n", name.c_str(), value.c_str());
    }
}

void applyScriptedEvent(xt::Context& ctx, xt::Document& doc, const ScriptedEvent& scripted) {
    if (scripted.port == "Port") {
        setPortText(doc, scripted.id, scripted.value);
        return;
    }
    if (scripted.port == "Pad") {
        // T-194: a gamepad button press of one local player, through the surface stack.
        xt::GamepadButton button{};
        gamepadButtonFromName(scripted.id, button);
        xt::InputEvent ev;
        ev.kind = xt::InputEvent::GamepadButtonDown;
        ev.gamepadButton = button;
        ev.player = scripted.player;
        ctx.pushInput(ev);
        ev.kind = xt::InputEvent::GamepadButtonUp;
        ctx.pushInput(ev);
        return;
    }
    xt::ElementRef ref = doc.byId(scripted.id);
    if (!ref.valid()) {
        std::fprintf(stderr, "xtframe-view: --event: no element '#%s'\n", scripted.id.c_str());
        return;
    }
    if (scripted.port == "Focus") {
        doc.setFocus(ref);
        return;
    }
    if (scripted.port == "Blur") {
        doc.setFocus(xt::ElementRef{});
        return;
    }
    if (scripted.port == "Change") {
        ref.setValue(scripted.value);
        return;
    }
    if (scripted.port == "State") {
        const bool on =
            scripted.mode == "toggle" ? !doc.hasCustomState(ref, scripted.value) : scripted.mode == "on";
        if (ctx.setCustomState(ref, scripted.value, on) != xt::Status::kOk) {
            std::fprintf(stderr, "xtframe-view: --event: no custom state '%s' in the document\n",
                         scripted.value.c_str());
        }
        return;
    }
    // The element's border box in viewport coordinates (its layout box through the world
    // transform), whose centre every pointer-driven port aims at.
    xt::Rect box = ref.boundingBox();
    xt::Vec2 center{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
    if (scripted.port == "PointerEnter") {
        pushPointer(ctx, xt::InputEvent::PointerMove, center);
    } else if (scripted.port == "PointerLeave") {
        pushPointer(ctx, xt::InputEvent::PointerMove, xt::Vec2{-1.0f, -1.0f});
    } else {  // Click.
        // T-359: a zero-area element, or one whose centre hits something outside its own subtree,
        // would never receive the pointer click; fire its triggers directly instead.
        bool reachable = box.w > 0.0f && box.h > 0.0f;
        if (reachable) {
            reachable = false;
            for (xt::NodeId hit = doc.hitTestPress(center); hit != xt::kInvalidNode;
                 hit = doc.arena().parent(hit)) {
                if (hit == ref.node) {
                    reachable = true;
                    break;
                }
            }
        }
        if (!reachable) {
            std::fprintf(stderr, "warning: #%s has no hit area; dispatching click directly\n",
                         scripted.id.c_str());
            (void)doc.fireTrigger(ref, xt::TriggerEvent::Press);
            (void)doc.fireTrigger(ref, xt::TriggerEvent::Release);
            (void)doc.fireTrigger(ref, xt::TriggerEvent::Click);
            return;
        }
        pushPointer(ctx, xt::InputEvent::PointerDown, center);
        pushPointer(ctx, xt::InputEvent::PointerUp, center);
    }
}

/// Applies every --event/--pointer scheduled for `frame`, in the order they were given on the
/// command line (all --pointer after all --event). Called just before that frame's update().
void applyScriptedInput(const Args& args, xt::Context& ctx, xt::DocumentHandle handle, uint32_t frame,
                        const std::function<void(const ScriptedEvent&)>& systemEvent) {
    xt::Document* doc = ctx.document(handle);
    if (doc == nullptr) {
        return;
    }
    // T-175: --port, before the first frame's update().
    if (frame == 0) {
        for (const auto& [name, value] : args.ports) {
            setPortText(*doc, name, value);
        }
    }
    // T-172: --play/--seek, before the first frame's update().
    if (frame == 0 && !args.playTimeline.empty()) {
        if (doc->playTimeline("", args.playTimeline) != xt::Status::kOk) {
            std::fprintf(stderr, "xtframe-view: --play: no @timeline '%s' in the document\n",
                         args.playTimeline.c_str());
        } else if (args.seekTime >= 0.0f) {
            doc->seekTimeline("", args.playTimeline, args.seekTime);
        }
    }
    for (const ScriptedEvent& event : args.events) {
        if (event.frame != frame) {
            continue;
        }
        if (event.port == "System") {
            systemEvent(event);  // T-193.
        } else {
            applyScriptedEvent(ctx, *doc, event);
        }
    }
    for (const ScriptedPointer& pointer : args.pointers) {
        if (pointer.frame == frame) {
            pushPointer(ctx, pointer.kind, xt::Vec2{pointer.x, pointer.y});
        }
    }
    // T-389: a synthetic fast mouse along the diagonal, one pixel-ish step per event, bouncing.
    if (args.synthPointer > 0) {
        const xt::Rect box = doc->root().boundingBox();
        const uint32_t steps = 600;
        for (uint32_t i = 0; i < args.synthPointer; ++i) {
            const uint32_t k = (frame * args.synthPointer + i) % (2 * steps);
            const float t = static_cast<float>(k < steps ? k : 2 * steps - k) / static_cast<float>(steps);
            pushPointer(ctx, xt::InputEvent::PointerMove, xt::Vec2{box.x + box.w * t, box.y + box.h * t});
        }
    }
    // T-403: synthetic host port traffic, one write per port per frame.
    if (args.synthPorts > 0) {
        uint32_t written = 0;
        for (const xt::PortRecord& port : doc->ports()) {
            if (written == args.synthPorts) {
                break;
            }
            if (!port.documentLevel || port.type != xt::PortValueType::Number) {
                continue;
            }
            const uint32_t k = (frame * 3u + written * 7u) % 200u;
            const float value = static_cast<float>(k < 100u ? k : 200u - k);
            (void)doc->setPort(port.id, xt::Value::ofFloat(value));
            ++written;
        }
    }
}

/// One FlowRuntime port value as --dump-flow-values prints it.
std::string formatPortValue(const xt::flow::PortValue& value) {
    switch (value.type) {
        case xt::flow::PortType::Pulse:
            return "pulse";
        case xt::flow::PortType::Bool:
            return value.b ? "true" : "false";
        case xt::flow::PortType::Number: {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", value.n);
            return buffer;
        }
        case xt::flow::PortType::String:
            return value.s;
    }
    return {};
}

/// T-401: a PortValueType as the .xt `port:` declaration spells it.
const char* portTypeName(xt::PortValueType type) {
    switch (type) {
        case xt::PortValueType::Number:
            return "number";
        case xt::PortValueType::String:
            return "string";
        case xt::PortValueType::Bool:
            return "bool";
        case xt::PortValueType::Color:
            return "color";
        case xt::PortValueType::Vec2:
            return "vec2";
        case xt::PortValueType::Vec3:
            return "vec3";
        case xt::PortValueType::Entity:
            return "entity";
        case xt::PortValueType::Enum:
            return "enum";
        case xt::PortValueType::Struct:
            return "struct";
        case xt::PortValueType::List:
            return "list";
        case xt::PortValueType::Texture:
            return "texture";
        case xt::PortValueType::Event:
            return "event";
    }
    return "unknown";
}

/// T-401: `text` as a JSON string literal (quotes included).
std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

/// T-401 (43 §43.4): --dump-ports: the Document's declared ports, in PORT order, as JSON (LF line
/// endings, one port per line): lookup name, type, direction, default (Document::portText() before
/// the first frame; "" for struct / list / event) and, where declared, the enum values, a
/// `list<T>`'s entry type or the struct / list entry fields. False (after its stderr line) when
/// the file cannot be written.
bool dumpPorts(const std::string& path, const std::string& uiName, const xt::Document& doc) {
    static const char* const kDirs[] = {"in", "out", "call"};
    std::string json = "{\n  \"ui\": " + jsonQuote(uiName) + ",\n  \"ports\": [";
    const std::vector<xt::PortRecord> ports = doc.ports();
    for (size_t i = 0; i < ports.size(); ++i) {
        const xt::PortRecord& port = ports[i];
        const int dir = static_cast<int>(port.dir);
        json += i == 0 ? "\n    " : ",\n    ";
        json += "{\"name\": " + jsonQuote(port.qualifiedName) + ", \"type\": \"" + portTypeName(port.type) +
                "\", \"dir\": \"" + (dir >= 0 && dir <= 2 ? kDirs[dir] : "unknown") +
                "\", \"default\": " + jsonQuote(doc.portText(port.id));
        if (port.type == xt::PortValueType::List && port.fields.empty()) {
            json += std::string(", \"entry\": \"") + portTypeName(port.entryType) + "\"";
        }
        if (!port.enumValues.empty()) {
            json += ", \"values\": [";
            for (size_t k = 0; k < port.enumValues.size(); ++k) {
                json += (k == 0 ? "" : ", ") + jsonQuote(port.enumValues[k]);
            }
            json += "]";
        }
        if (!port.fields.empty()) {
            json += ", \"fields\": [";
            for (size_t k = 0; k < port.fields.size(); ++k) {
                json += std::string(k == 0 ? "" : ", ") + "[" + jsonQuote(port.fields[k].first) + ", \"" +
                        portTypeName(port.fields[k].second) + "\"]";
            }
            json += "]";
        }
        json += "}";
    }
    json += ports.empty() ? "]\n}\n" : "\n  ]\n}\n";
    std::ofstream file(pathFromUtf8(path), std::ios::binary);
    if (!file || !file.write(json.data(), static_cast<std::streamsize>(json.size()))) {
        std::fprintf(stderr, "xtframe-view: --dump-ports: cannot write '%s'\n", path.c_str());
        return false;
    }
    return true;
}

/// --dump-flow-values: `<node>.<port> = <value>`, one per line, on stdout. T-193: `title` names
/// the flow ("flow" for --flow, "global flow" for the project's).
void printFlowValues(const xt::flow::FlowRuntime& runtime, uint32_t frame, const char* title = "flow") {
    std::span<const xt::flow::PortValue> values = runtime.values();
    const std::vector<std::string>& names = runtime.valueNames();
    std::printf("xtframe-view: %s values (frame %u)\n", title, frame);
    for (size_t i = 0; i < values.size() && i < names.size(); ++i) {
        std::printf("%s = %s\n", names[i].c_str(), formatPortValue(values[i]).c_str());
    }
}

/// Writes `rgba` to `path` (relative paths resolve against the current directory) and prints the
/// absolute path it landed at. False when the file could not be written (already reported).
bool writeScreenshot(const std::string& path, const uint8_t* rgba, int width, int height, int strideBytes) {
    std::string absolute = pathToUtf8(fs::absolute(pathFromUtf8(path)).lexically_normal());
    if (!xt::sw::writePng(absolute, rgba, width, height, strideBytes)) {
        std::fprintf(stderr, "xtframe-view: failed to write screenshot '%s'\n", absolute.c_str());
        return false;
    }
    std::printf("xtframe-view: wrote screenshot %s (%dx%d)\n", absolute.c_str(), width, height);
    return true;
}

/// T-179: --lang. Loads every `<rootDir>/strings/*.csv` into `provider`, binds it and sets the
/// language on `ctx`. Returns false (after its own stderr line) for a table that cannot be read or
/// parsed, or an invalid language tag. No-op without --lang.
bool setupLanguage(const Args& args, const std::string& rootDir,
                   xt::bundle::BundleLocalizationProvider& provider, xt::Context& ctx) {
    if (args.lang.empty()) {
        return true;
    }
    const fs::path dir = pathFromUtf8(rootDir) / "strings";
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec) || it->path().extension() != ".csv") {
                continue;
            }
            std::string error;
            if (provider.loadFile(pathToUtf8(it->path().stem()), pathToUtf8(it->path()), &error) !=
                xt::Status::kOk) {
                std::fprintf(stderr, "xtframe-view: --lang: %s\n", error.c_str());
                return false;
            }
        }
    }
    ctx.setLocalizationProvider(&provider);
    if (ctx.setLanguage(args.lang) != xt::Status::kOk) {
        std::fprintf(stderr, "xtframe-view: --lang: invalid language '%s'\n", args.lang.c_str());
        return false;
    }
    return true;
}

/// T-193: the flows a `--event "system ..."` reaches and --project's bundle.
struct FlowTargets {
    xt::Context* ctx = nullptr;             ///< T-200: the Documents' `system.*` ports.
    xt::flow::FlowRuntime* main = nullptr;  ///< The --flow graph (may be unloaded).
    /// --project only (T-356: the loader the editor's Demo space shares): the other UIs, the
    /// BundleContext and the global flow.
    xt::bundle::ProjectRuntime project;
};

/// T-376 (23 §23.ab): the window `winMinimize` / `winMaximize` / `winClose` out events drive;
/// set in the windowed mode only.
xt::shell::App* g_windowApp = nullptr;

/// T-376: minimizes, maximizes-or-restores or closes g_windowApp for the port names
/// `winMinimize`, `winMaximize` and `winClose`; nothing for any other name or without a window.
void windowControl(std::string_view port) {
    if (g_windowApp == nullptr) {
        return;
    }
    if (port == "winMinimize") {
        g_windowApp->minimize();
    } else if (port == "winMaximize") {
        g_windowApp->toggleMaximize();
    } else if (port == "winClose") {
        g_windowApp->close();
    }
}

/// T-376: a UI's own `emit(winMinimize)` (a Document out event, no flow in between) reaches
/// windowControl() through the Context. T-393 (08.14): a drop of files from the OS
/// (HostEvent::Drop with paths) prints the target and every path.
class WindowControlListener : public xt::ContextListener {
public:
    void onHostEvent(const xt::HostEvent& event) override {
        if (event.kind == xt::HostEventKind::Emit) {
            windowControl(event.name);
        } else if (event.kind == xt::HostEventKind::Drop && !event.paths.empty()) {
            const std::string target(event.targetId);
            std::printf("xtframe-view: drop of %zu file(s) on #%s\n", event.paths.size(), target.c_str());
            for (const std::string& path : event.paths) {
                std::printf("xtframe-view:   %s\n", path.c_str());
            }
            std::fflush(stdout);
        }
    }
};
WindowControlListener g_windowListener;

/// T-193: prints one flow action node event.
void printFlowEvent(const xt::flow::FlowHostEvent& event) {
    static const char* const kKinds[] = {"sound", "haptic", "spawn", "emit"};
    std::printf("xtframe-view: flow event %s %s ui=%s instance=%u\n", kKinds[static_cast<int>(event.kind)],
                event.name.c_str(), event.uiId.c_str(), event.instanceId);
    if (event.kind == xt::flow::FlowHostEventKind::Emit) {
        windowControl(event.name);  // T-376: winMinimize / winMaximize / winClose.
    }
}

/// T-193: `--event "system <kind> [<value>]"`.
void postSystemEvent(FlowTargets& targets, const ScriptedEvent& scripted) {
    if (targets.main != nullptr) {
        targets.main->postSystemEvent(scripted.id, scripted.value);
    }
    // T-200: the BundleContext goes through the Context (every Document's `system.*` port, then
    // its flows); without one the Context reaches the Documents alone.
    if (targets.project.bundle) {
        targets.project.bundle->postSystemEvent(scripted.id, scripted.value);
    } else if (targets.ctx != nullptr) {
        targets.ctx->postSystemEvent(scripted.id, scripted.value);
    }
}

/// T-356: --project's runtime sources and loader options (xt::bundle::loadProjectRuntime()).
xt::bundle::ProjectRuntimeSources projectSources(const Args& args) {
    xt::bundle::ProjectRuntimeSources sources;
    sources.currentUi = args.mainUi;
    for (const std::array<std::string, 3>& ui : args.otherUis) {
        sources.others.push_back(xt::bundle::RuntimeUiSource{ui[0], ui[1], ui[2]});
    }
    sources.globalFlow = args.globalFlow;
    return sources;
}

xt::bundle::ProjectRuntimeOptions projectOptions(const Args& args, const std::string& rootDir) {
    xt::bundle::ProjectRuntimeOptions options;
    options.compile.rootDir = rootDir;
    options.compile.searchDirs = args.assetRoots;
    options.compile.stripEditorOnly = g_stripEditorOnly;  // T-343: --scene shows the published UI.
    options.compile.strict = g_strict;                    // T-392
    options.onDiagnostic = printDiagnostic;
    options.loadFlows = !args.noFlow;
    return options;
}

/// T-193 --project: loads every other UI of the Bundle into `ctx` (before the main UI, so that
/// one stays the foreground Document). False (after its own stderr line) on a load failure.
/// T-555 (Docs/design/22-bundle.md §22.10.3): registers the bundled fonts of the main UI and of
/// --project's other UIs with `shaper` (bundle::addBundleFonts(); an asset registered before is
/// skipped). Called before the system fonts, so a bundled family wins over a system font of the
/// same name; again after R re-packs (new assets only).
void addBundleFonts(const Args& args, xt::TextShaper& shaper) {
    if (!args.bundle) {
        return;
    }
    std::vector<std::string> ids{args.mainUi};
    for (const std::array<std::string, 3>& other : args.otherUis) {
        ids.push_back(other[0]);
    }
    for (const std::string& id : ids) {
        std::vector<std::string> warnings;
        (void)xt::bundle::addBundleFonts(*args.bundle, id, shaper, &warnings);
        for (const std::string& warning : warnings) {
            std::fprintf(stderr, "xtframe-view: %s\n", warning.c_str());
        }
    }
}

bool loadOtherUis(const Args& args, xt::Context& ctx, const std::string& rootDir, FlowTargets& targets) {
    if (!args.wholeProject) {
        return true;
    }
    std::vector<xt::bundle::ProjectRuntimeProblem> problems;
    const xt::Status status = xt::bundle::loadProjectUis(projectSources(args), projectOptions(args, rootDir),
                                                         ctx, targets.project, problems);
    for (const xt::bundle::ProjectRuntimeProblem& problem : problems) {
        std::fprintf(stderr, "xtframe-view: %s\n", problem.message.c_str());
    }
    return status == xt::Status::kOk;
}

/// T-193 --project: (re)builds the BundleContext over the main UI and loadOtherUis()' UIs, loads
/// each other UI's own flow and the global flow. False (after its stderr lines) when a flow is
/// invalid.
bool setupProjectFlows(const Args& args, xt::Context& ctx, xt::DocumentHandle mainHandle,
                       FlowTargets& targets) {
    targets.project.bundle.reset();
    if (!args.wholeProject) {
        return true;
    }
    // T-356: the loader the editor's Demo space shares (xt/bundle/project_runtime.h).
    std::vector<xt::bundle::ProjectRuntimeProblem> problems;
    xt::bundle::ProjectRuntimeOptions options = projectOptions(args, args.root);
    options.hostEventListener = printFlowEvent;
    const xt::Status status = xt::bundle::startProjectFlows(projectSources(args), options, ctx, mainHandle,
                                                            targets.project, problems);
    for (const xt::bundle::ProjectRuntimeProblem& problem : problems) {
        if (problem.ui.empty()) {
            std::fprintf(stderr, "xtframe-view: global flow: %s\n", problem.message.c_str());
        } else {
            std::fprintf(stderr, "xtframe-view: flow (%s): %s\n", problem.ui.c_str(),
                         problem.message.c_str());
        }
    }
    if (status != xt::Status::kOk) {
        return false;
    }
    if (!targets.project.globalFlowPath.empty()) {
        std::fprintf(stderr, "xtframe-view: loaded global flow %s\n", targets.project.globalFlowPath.c_str());
    }
    return true;
}

}  // namespace

// T-261f: `argv` is UTF-8 here on every platform. On Windows <SDL.h> (through xt/shell/app.h)
// renames this function to SDL_main, and SDL2main's own `main` / `wmain` hands it the wide command
// line as UTF-8, so this tool has no `wmain` of its own.
int main(int argc, char** argv) {
    xt::tools::useUtf8Console();  // T-261e
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage(stderr);
        return 2;
    }
    if (args.version) {
        xt::tools::printVersion("xtframe-view", stdout);
        std::printf("renderers: %s\n", xt::view::builtRenderers().c_str());  // T-439
        return 0;
    }
    if (args.listFonts) {
        // T-426: the families addSystemFonts() registers, sorted and deduplicated (byte order).
        // T-429: by default each one with the file it came from, `family<TAB>path`.
#ifndef XT_GL_NO_TEXT
        xt::text::FtTextShaper shaper;
        shaper.addSystemFonts();
        std::vector<xt::FontDesc> fonts;
        shaper.fonts(fonts);
        auto printable = [](const std::string& s) {
            return std::none_of(s.begin(), s.end(),
                                [](char c) { return static_cast<unsigned char>(c) < 0x20; });
        };
        std::set<std::string> lines;
        for (const xt::FontDesc& font : fonts) {
            // A family with a control character (some fonts' name tables hold line breaks)
            // cannot be printed one per line; it is left out.
            if (font.family.empty() || !printable(font.family)) {
                continue;
            }
            if (args.listFontNames) {
                lines.insert(font.family);
                continue;
            }
            const std::string path = shaper.fontPath(font.id);
            if (printable(path)) {
                lines.insert(font.family + "\t" + path);
            }
        }
        for (const std::string& line : lines) {
            std::fprintf(stdout, "%s\n", line.c_str());
        }
#endif
        return 0;
    }
    if (args.help) {
        printUsage(stdout);
        return 0;
    }
    {
        // T-498b: the preset symbol library ships next to the executable (`<exe dir>/symbols`);
        // its default group is what a UI Project's pack resolves the standard symbols in.
        std::error_code ec;
        const fs::path symbols = fs::absolute(pathFromUtf8(argv[0]), ec).parent_path() / "symbols";
        args.standardSymbolDir = xt::bundle::defaultGroupDir(pathToUtf8(symbols));
        // T-527d (22 §22.10.3): the standard fonts ship next to it (`<exe dir>/fonts`).
        args.standardFontDir = pathToUtf8(fs::absolute(pathFromUtf8(argv[0]), ec).parent_path() / "fonts");
    }
    if (!resolveInput(args)) {
        return args.loadFailed ? 1 : 2;
    }
    // T-135: only a flow nobody asked for by name is announced.
    auto announceFlow = [&args]() {
        if (args.flowAuto) {
            std::fprintf(stderr, "xtframe-view: loaded flow %s\n", args.flow.c_str());
        }
    };

    const fs::path inputPath = pathFromUtf8(args.input);
    std::string rootDir = args.root.empty() ? pathToUtf8(inputPath.parent_path()) : args.root;
    // T-179: outlives every Context below (declared before them).
    xt::bundle::BundleLocalizationProvider localization;

    g_stripEditorOnly = !args.scene.empty();  // T-343
    g_strict = args.strict;                   // T-392
    // T-439: --renderer. A renderer other than the shell's (windowed) or other than xt_sw
    // (headless) draws the frame loop below through a FrameTarget; when it cannot be created the
    // run goes on with the default path.
    const xt::view::Renderer chosenRenderer = xt::view::resolveRenderer(args.renderer, args.headless);
    std::unique_ptr<xt::view::FrameTarget> frameTarget;
    if (chosenRenderer != xt::view::defaultRenderer(args.headless)) {
        if (!args.scene.empty() || args.showDamage) {
            std::fprintf(stderr,
                         "xtframe-view: --scene / --show-damage use the default renderer path; "
                         "--renderer %s ignored\n",
                         xt::view::rendererName(chosenRenderer));
        } else {
            xt::view::FrameTargetConfig config;
            config.width = args.width;
            config.height = args.height;
            config.windowed = !args.headless;
            config.hidden = args.hidden;
            config.sampleCount = args.msaa >= 4 ? 4u : 1u;  // T-441: --msaa reaches --renderer gl / wgpu
            config.title = "XTFrame";
            config.clear = args.headless ? (args.transparent ? xt::Color{0, 0, 0, 0} : xt::Color{1, 1, 1, 1})
                                         : xt::Color{0.1f, 0.1f, 0.1f, 1.0f};  // the shell's background
            std::string error;
            frameTarget = xt::view::createFrameTarget(chosenRenderer, config, error);
            if (!frameTarget) {
                std::fprintf(stderr, "xtframe-view: renderer %s unavailable (%s); using %s\n",
                             xt::view::rendererName(chosenRenderer), error.c_str(),
                             xt::view::rendererName(xt::view::defaultRenderer(args.headless)));
            } else {
                std::fprintf(stderr, "xtframe-view: renderer %s\n", frameTarget->description().c_str());
            }
        }
    }
    if (args.headless || frameTarget) {
        if (!args.secondWindow.empty()) {
            std::fprintf(stderr, "xtframe-view: --second-window needs a window; ignored with --headless\n");
        }
        // No window/GL context at all (Docs/tasks/T-56.md): drive a plain xt::Context + xt_sw
        // software renderer directly, the same pattern Examples/engine_integration/main.cpp uses.
        // Text is shaped with the real FreeType shaper over the system fonts (T-131): a headless
        // screenshot used to come out with no text at all because this path had NullTextShaper.
#ifndef XT_GL_NO_TEXT
        xt::text::FtTextShaper shaper;
        addBundleFonts(args, shaper);  // T-555: before the system fonts.
        shaper.addSystemFonts();
#if !defined(XT_BACKEND_NO_TEXT)
        if (frameTarget) {
            frameTarget->setTextShaper(&shaper);  // T-439: the GPU renderers' glyph atlas
        }
#endif
#else
        xt::NullTextShaper shaper;
#endif
        xt::FileAssetResolver assets = args.assetRoots.empty()
                                           ? xt::FileAssetResolver(rootDir, xt::sw::decodeImageBytes)
                                           : xt::FileAssetResolver(args.assetRoots, xt::sw::decodeImageBytes);
        assets.setFileStatusFn(&xt::fileStatusUtf8);  // T-263e: the same function the window path passes.
        xt::Context ctx(xt::ContextConfig{&shaper, &assets});
        // T-191 (25.3.5b): no decoder here -- an unbound <video>/<external> shows its placeholder.
        ctx.setExternalPlaceholders(true);
        if (!bindExternalTextures(args, assets, /*upload=*/false)) {
            return 1;
        }
        if (!setupLanguage(args, rootDir, localization, ctx)) {
            return 1;
        }

        // T-193: --project's other UIs first, so the main UI stays the foreground Document.
        FlowTargets targets;
        if (!loadOtherUis(args, ctx, rootDir, targets)) {
            return 1;
        }
        xt::DocumentHandle handle = 0;
        if (!loadInto(ctx, args.input, rootDir, args.assetRoots, handle)) {
            return 1;
        }
        // T-401: --dump-ports / --log-ports (43 §43.4 corpus); the table holds the declared defaults.
        uint32_t portLogFrame = 0;
        if (xt::Document* doc = ctx.document(handle)) {
            if (!args.dumpPorts.empty() &&
                !dumpPorts(args.dumpPorts, args.ui.empty() ? args.input : args.ui, *doc)) {
                return 1;
            }
            if (args.logPorts) {
                doc->addPortListener([doc, &portLogFrame](const xt::PortEvent& event) {
                    static const char* const kKinds[] = {"changed", "emit", "list"};
                    const int kind = static_cast<int>(event.kind);
                    std::printf("xtframe-view: port %s %s = %s (frame %u)\n",
                                kind >= 0 && kind <= 2 ? kKinds[kind] : "unknown",
                                doc->portInfo(event.port).qualifiedName.c_str(),
                                doc->portText(event.port).c_str(), portLogFrame);
                });
            }
        }
        ctx.setViewport(xt::Rect{0, 0, static_cast<float>(args.width), static_cast<float>(args.height)},
                        1.0f);
        ctx.setDevice(args.device);  // T-176 (validated by parseArgs()).
        if (args.logicalHeight > 0.0f) {
            // T-210: uiscale = output height / the UI Project's design height; T-241: the edge
            // spec.uiscaleRule names (width or the short edge for a portrait phone).
            ctx.setUiScale(xt::bundle::uiScaleFor(args.uiscaleRule, static_cast<float>(args.width),
                                                  static_cast<float>(args.height), args.logicalHeight));
            // T-345 (25 §25.4.2 item 1): the project's clamp and DPI floor.
            (void)ctx.setUiScaleLimits(args.uiscaleLimits.clampMin, args.uiscaleLimits.clampMax,
                                       args.uiscaleLimits.dpiFloor);
        }
        for (uint32_t p = 0; p < args.players; ++p) {
            ctx.setPlayerDevice(static_cast<uint8_t>(p), xt::InputKind::Gamepad);  // T-194.
        }

        xt::flow::FlowRuntime flowRuntime;
        flowRuntime.setHostEventListener(printFlowEvent);
        flowRuntime.setPerfTracker(&ctx.perf());  // T-475 (21 §21.6): the flow stages and counters.
        targets.main = &flowRuntime;
        targets.ctx = &ctx;
        bool hasFlow = !args.flow.empty();
        if (hasFlow && !loadFlow(ctx, handle, args.flow, flowRuntime)) {
            return 2;
        }
        if (hasFlow) {
            announceFlow();
            if (!xt::tools::view::reportFlowContinuity(flowRuntime, args.strict)) {  // T-540
                return 2;
            }
        }
        if (!setupProjectFlows(args, ctx, handle, targets)) {
            return 2;
        }
        if (targets.project.bundle && targets.project.bundle->flowRuntime().isGlobal() &&
            !xt::tools::view::reportFlowContinuity(targets.project.bundle->flowRuntime(), args.strict)) {
            return 2;  // T-546: the global flow's diagnostics, as the main flow's above.
        }
        auto systemEvent = [&targets](const ScriptedEvent& event) { postSystemEvent(targets, event); };

        ctx.perf().setEnabled(args.perf);
        std::unique_ptr<PerfCsvWriter> csv;
        if (!args.perfCsv.empty()) {
            csv = std::make_unique<PerfCsvWriter>(args.perfCsv);
            if (!csv->ok()) {
                std::fprintf(stderr, "xtframe-view: failed to open '%s' for writing\n", args.perfCsv.c_str());
                return 1;
            }
        }

        xt::sw::SwRenderer renderer;
        std::vector<uint8_t> pixels(static_cast<size_t>(args.width) * static_cast<size_t>(args.height) * 4);
        const int strideBytes = args.width * 4;
        xt::sw::SwTarget target{pixels.data(), args.width, args.height, strideBytes};
        // T-343: --scene: the image under the UI, Direct mode (the renderer never clears).
        std::vector<uint8_t> scene;
        int sceneOffset = 0;
        if (!args.scene.empty()) {
            std::vector<uint8_t> raw;
            int sw = 0, sh = 0;
            if (!loadScene(args.scene, raw, sw, sh)) {
                return 1;
            }
            resampleRgba(raw, sw, sh, args.width, args.height, scene);
            ctx.setHostTargetMode(xt::HostTargetMode::Direct);
        }
        auto fillScene = [&]() {
            for (int y = 0; y < args.height; ++y) {
                const uint8_t* src =
                    scene.data() + static_cast<size_t>(y) * static_cast<size_t>(args.width) * 4u;
                uint8_t* dst = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(strideBytes);
                for (int x = 0; x < args.width; ++x) {
                    const int sx = (x + sceneOffset) % args.width;
                    std::memcpy(dst + static_cast<size_t>(x) * 4u, src + static_cast<size_t>(sx) * 4u, 3);
                    dst[static_cast<size_t>(x) * 4u + 3] = 255;
                }
            }
            if (args.sceneAnimate) {
                sceneOffset = (sceneOffset + 1) % args.width;
            }
        };
        xt::CommandList list;
        xt::DamageList damage;
        // T-98: the damage diff needs the previous frame's command list, which Context::render()
        // takes back out of `list` -- so `list` is never cleared here.
        bool trackDamage = args.perf || args.showDamage || !args.scene.empty();  // T-343: bands
        float damageAreaSum = 0.0f;
        const float dt = 1.0f / 60.0f;
        uint32_t frameCount = args.frames > 0 ? args.frames : 1;
        // T-439: a windowed FrameTarget runs until the window closes (or --warmup + 1 frames for a
        // --screenshot), with the platform's events and clock.
        const bool windowLoop = frameTarget && frameTarget->windowed();
        if (windowLoop) {
            frameCount = args.screenshot.empty() ? UINT32_MAX : args.warmup + 1;
        }
        double lastTime = windowLoop ? frameTarget->seconds() : 0.0;
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            float frameDt = frame == 0 ? 0.0f : dt;
            if (windowLoop) {
                if (!frameTarget->pump(ctx)) {
                    frameCount = frame;
                    break;
                }
                const double now = frameTarget->seconds();
                frameDt = frame == 0                 ? 0.0f
                          : !args.screenshot.empty() ? args.warmupDt
                                                     : static_cast<float>(now - lastTime);
                lastTime = now;
            }
            portLogFrame = frame;
            // T-125: queued before update(), which drains and dispatches the queue first.
            applyScriptedInput(args, ctx, handle, frame, systemEvent);
            ctx.update(frameDt);
            if (hasFlow || targets.project.bundle) {
                xt::PerfScope flowScope(ctx.perf(), &xt::FrameStats::flowMs);
                if (hasFlow) {
                    flowRuntime.tick(frameDt);
                }
                if (targets.project.bundle) {
                    targets.project.bundle->tick(frameDt);
                }
            }
            ctx.render(list, trackDamage ? &damage : nullptr);
            if (trackDamage) {
                damageAreaSum += damageAreaFraction(damage, damage.viewport);
            }
            const xt::Document* doc = ctx.document(handle);
            const xt::IrView* irView = doc != nullptr ? &doc->ir() : nullptr;
            if (frameTarget) {
                // T-439: gl / wgpu (or windowed sw) instead of the xt_sw buffer.
                frameTarget->render(list, &shaper, &assets, irView, &ctx.perf(),
                                    trackDamage ? &damage : nullptr);
                const bool keepForShot = windowLoop && !args.screenshot.empty() && frame + 1 == frameCount;
                if (!keepForShot) {
                    frameTarget->present();
                }
            } else if (!scene.empty()) {
                renderer.setTarget(target);
                fillScene();
                renderer.render(list, &shaper, &assets, irView, &ctx.perf(), trackDamage ? &damage : nullptr);
            } else {
                renderer.setTarget(target);
                renderer.clear(args.transparent ? xt::Color{0, 0, 0, 0} : xt::Color{1, 1, 1, 1});
                renderer.render(list, &shaper, &assets, irView, &ctx.perf());
            }
            if (args.showDamage) {
                drawDamageOverlay(pixels.data(), args.width, args.height, strideBytes, damage);
            }
            if (csv) {
                csv->writeNewFrames(ctx.perf());
            }
            if (hasFlow && args.dumpEvery > 0 && (frame + 1) % args.dumpEvery == 0) {
                printFlowValues(flowRuntime, frame);
            }
            if (targets.project.bundle && targets.project.bundle->flowRuntime().isGlobal() &&
                args.dumpEvery > 0 && (frame + 1) % args.dumpEvery == 0) {
                printFlowValues(targets.project.bundle->flowRuntime(), frame, "global flow");
            }
        }
        if (args.perf) {
            printPerfSummary(ctx.perf().summary(frameCount),
                             frameCount > 0 ? 100.0f * damageAreaSum / static_cast<float>(frameCount) : 0.0f);
        }
        // T-125: --screenshot works headless too -- the software buffer above already holds the
        // last frame, so nothing needs a GL context. T-439: a FrameTarget is read back instead.
        if (!args.screenshot.empty() && frameTarget) {
            std::vector<uint8_t> shot;
            int shotW = 0;
            int shotH = 0;
            if (!frameTarget->read(shot, shotW, shotH)) {
                std::fprintf(stderr, "xtframe-view: cannot read the %s frame back for '%s'\n",
                             xt::view::rendererName(chosenRenderer), args.screenshot.c_str());
                return 1;
            }
            if (windowLoop) {
                frameTarget->present();
            }
            if (!writeScreenshot(args.screenshot, shot.data(), shotW, shotH, shotW * 4)) {
                return 1;
            }
        } else if (!args.screenshot.empty() &&
                   !writeScreenshot(args.screenshot, pixels.data(), args.width, args.height, strideBytes)) {
            return 1;
        }
        if (args.strict) {
            // T-392 (27 §27.3.2): the Document's runtime diagnostics (e.g. a virtualized repeat
            // with a zero-height viewport) as warnings.
            if (const xt::Document* doc = ctx.document(handle)) {
                for (const std::string& message : doc->warnings()) {
                    std::fprintf(stderr, "xtframe-view: warning: %s\n", message.c_str());
                }
            }
        }
        for (const std::string& id : args.printText) {
            std::string text;
            const xt::Document* doc = ctx.document(handle);
            if (doc != nullptr) {
                xt::ElementRef ref = doc->byId(id);
                if (ref.valid()) {
                    text = std::string(ref.text());
                } else {
                    std::fprintf(stderr, "xtframe-view: --print-text: no element '%s'\n",
                                 id.c_str());  // T-392
                }
            }
            std::printf("text=%s\n", text.c_str());
        }
        if (hasFlow && args.dumpFlowValues) {
            printFlowValues(flowRuntime, frameCount - 1);
        }
        if (targets.project.bundle && targets.project.bundle->flowRuntime().isGlobal() &&
            args.dumpFlowValues) {
            printFlowValues(targets.project.bundle->flowRuntime(), frameCount - 1, "global flow");
        }
        if (args.dumpCommands) {
            // T-202: the frame the renderer drew last, one line per command (DrawParticles lists
            // its instances).
            std::string dump;
            xt::dumpCommandList(list, dump);
            std::fwrite(dump.data(), 1, dump.size(), stdout);
        }
        if (windowLoop) {
            std::printf("xtframe-view: window run complete (%u frames)\n", frameCount);
            return 0;
        }
        std::printf("xtframe-view: headless run complete (%u frames)\n", frameCount);
        return 0;
    }

    xt::shell::WindowConfig windowConfig;
    windowConfig.width = args.width;
    windowConfig.height = args.height;
    windowConfig.msaa = args.msaa;
    windowConfig.title = "XTFrame";
    windowConfig.hidden = args.hidden;
    windowConfig.borderless = args.borderless;  // T-376.
    windowConfig.resizeBorder = args.resizeBorder;
    windowConfig.liveResize = args.liveResize;                                             // T-377.
    windowConfig.fullscreen = args.fullscreen == 2   ? xt::shell::FullscreenMode::Desktop  // T-398.
                              : args.fullscreen == 1 ? xt::shell::FullscreenMode::Exclusive
                                                     : xt::shell::FullscreenMode::Off;
    if (!args.fit.empty()) {
        // T-156: parseArgs()/resolveInput() only let a valid mode through.
        xt::shell::parseFitMode(args.fit, windowConfig.fit);
        windowConfig.designWidth = args.designWidth > 0 ? args.designWidth : args.width;
        windowConfig.designHeight = args.designHeight > 0 ? args.designHeight : args.height;
    }

    if (args.showDamage) {
        // Docs/design/23-host-integration.md §23.3: the GL backend has no partial repaint and no
        // overlay pass, so the flag is accepted and ignored outside --headless.
        std::fprintf(stderr, "xtframe-view: --show-damage needs --headless (software backend); ignored\n");
    }

    xt::shell::App app(windowConfig);
    if (!app.ok()) {
        std::fprintf(stderr, "xtframe-view: failed to initialize window/GL context: %s\n", app.lastError());
        return 1;
    }
    g_windowApp = &app;  // T-376: the window controls (winMinimize / winMaximize / winClose).
    app.context().addListener(&g_windowListener);
    if (args.assetRoots.empty()) {
        app.setAssetRoot(rootDir);
    } else {
        app.setAssetRoot(args.assetRoots);
    }
    // T-263e: xt_core asks the file system nothing itself; the R key's revalidate() compares the
    // decoded files with the disk through this (after setAssetRoot, which replaces the resolver).
    app.assetResolver().setFileStatusFn(&xt::fileStatusUtf8);
#ifndef XT_GL_NO_TEXT
    addBundleFonts(args, app.textShaper());  // T-555: before the system fonts.
    app.addSystemFonts();
#endif
    // T-150: after setAssetRoot (which replaces the resolver), before the first update().
    if (!bindExternalTextures(args, app.assetResolver(), /*upload=*/true)) {
        return 1;
    }
    if (!setupLanguage(args, rootDir, localization, app.context())) {
        return 1;
    }

    // T-193: --project's other UIs first, so the main UI stays the foreground Document.
    FlowTargets targets;
    if (!loadOtherUis(args, app.context(), rootDir, targets)) {
        return 1;
    }
    xt::DocumentHandle handle = 0;
    if (!loadInto(app.context(), args.input, rootDir, args.assetRoots, handle)) {
        return 1;
    }
    app.setCurrentDocument(handle);
    app.setStatsLog(args.stats);  // T-240.
    if (!args.scene.empty()) {
        // T-343: the scene under the UI, Direct mode; --scene-animate scrolls it 1 px per frame.
        std::vector<uint8_t> raw;
        int sw = 0, sh = 0;
        if (!loadScene(args.scene, raw, sw, sh)) {
            return 1;
        }
        app.setScene(raw.data(), sw, sh);
        app.setSceneScroll(args.sceneAnimate ? 1 : 0);
    }
    app.syncViewport();  // The Document didn't exist yet when App's constructor set the viewport.

    if (args.logicalHeight > 0.0f) {
        // T-210: uiscale follows the window height; T-241: or the edge spec.uiscaleRule names.
        const xt::shell::UiScaleEdge edge =
            args.uiscaleRule == xt::bundle::UiScaleRule::Width       ? xt::shell::UiScaleEdge::Width
            : args.uiscaleRule == xt::bundle::UiScaleRule::ShortEdge ? xt::shell::UiScaleEdge::ShortEdge
            : args.uiscaleRule == xt::bundle::UiScaleRule::Fit       ? xt::shell::UiScaleEdge::Fit
                                                                     : xt::shell::UiScaleEdge::Height;
        // T-345: the clamp and DPI floor first, so the first uiscale already honours them.
        (void)app.context().setUiScaleLimits(args.uiscaleLimits.clampMin, args.uiscaleLimits.clampMax,
                                             args.uiscaleLimits.dpiFloor);
        app.setLogicalHeight(args.logicalHeight, edge);
    }
    app.context().setDevice(args.device);  // T-176 (validated by parseArgs()).
    for (uint32_t p = 0; p < args.players; ++p) {
        app.context().setPlayerDevice(static_cast<uint8_t>(p), xt::InputKind::Gamepad);  // T-194.
    }
    if (args.gamepad &&
        !app.enableGamepad(static_cast<uint8_t>(args.players != 0 ? args.players : xt::kMaxPlayers))) {
        std::fprintf(stderr, "xtframe-view: --gamepad: SDL game controller support failed: %s\n",
                     SDL_GetError());
    }
    app.setTitle(("XTFrame - " + pathToUtf8(inputPath.filename())).c_str());  // UTF-8 (T-261f)

    xt::flow::FlowRuntime flowRuntime;
    flowRuntime.setHostEventListener(printFlowEvent);
    flowRuntime.setPerfTracker(&app.context().perf());  // T-475 (21 §21.6).
    targets.main = &flowRuntime;
    targets.ctx = &app.context();
    bool hasFlow = !args.flow.empty();
    if (hasFlow && !loadFlow(app.context(), handle, args.flow, flowRuntime)) {
        return 2;
    }
    if (hasFlow) {
        announceFlow();
        if (!xt::tools::view::reportFlowContinuity(flowRuntime, args.strict)) {  // T-540
            return 2;
        }
    }
    if (!setupProjectFlows(args, app.context(), handle, targets)) {
        return 2;
    }
    if (targets.project.bundle && targets.project.bundle->flowRuntime().isGlobal() &&
        !xt::tools::view::reportFlowContinuity(targets.project.bundle->flowRuntime(), args.strict)) {
        return 2;  // T-546: the global flow's diagnostics, as the main flow's above.
    }
    auto systemEvent = [&targets](const ScriptedEvent& event) { postSystemEvent(targets, event); };

    bool paused = false;

    // T-244: --stats also reports the rig workload, which comes from the perf tracer.
    app.context().perf().setEnabled(args.perf || args.stats);
    std::unique_ptr<PerfCsvWriter> csv;
    if (!args.perfCsv.empty()) {
        csv = std::make_unique<PerfCsvWriter>(args.perfCsv);
        if (!csv->ok()) {
            std::fprintf(stderr, "xtframe-view: failed to open '%s' for writing\n", args.perfCsv.c_str());
            return 1;
        }
    }
    std::string baseTitle = "XTFrame - " + pathToUtf8(inputPath.filename());
    float titleTimer = 0.0f;

    uint32_t frameIndex = 0;
    auto onFrame = [&](float dt) {
        // T-125: onFrame runs before Context::update(), which drains the queue this fills.
        applyScriptedInput(args, app.context(), handle, frameIndex++, systemEvent);
        // T-139: --dump-every also works in the windowed run (same block as headless), so a
        // windowed interaction can be checked from stdout.
        if (hasFlow && args.dumpEvery > 0 && frameIndex % args.dumpEvery == 0) {
            printFlowValues(flowRuntime, frameIndex);
            std::fflush(stdout);
        }
        if (hasFlow) {
            flowRuntime.tick(dt);
        }
        if (targets.project.bundle) {
            targets.project.bundle->tick(dt);
        }
        if (args.perf) {
            // onFrame runs before Context::update(), so the newest history entry is the frame that
            // has just finished -- exactly what both the CSV row and the title should report.
            if (csv) {
                csv->writeNewFrames(app.context().perf());
            }
            titleTimer += dt;
            if (titleTimer >= 0.5f) {
                titleTimer = 0.0f;
                // §21.3: `fps | update a.b ms | render c.d ms`, over the last 30 frames.
                xt::PerfSummary s = app.context().perf().summary(30);
                char title[256];
                std::snprintf(title, sizeof(title), "%s - %.0f fps | update %.2f ms | render %.2f ms",
                              baseTitle.c_str(), static_cast<double>(s.fpsAvg),
                              static_cast<double>(s.avg.updateMs), static_cast<double>(s.avg.renderMs));
                app.setTitle(title);
            }
        }
        std::vector<xt::InputEvent> events;
        app.context().drainInput(events);
        // T-139 (Docs/design/12-compiler.md 12.6): the viewer's hotkeys (Space, Left, Right, R,
        // Escape) are consumed here; every other event is pushed back so Context::update()
        // dispatches it to the document. Before T-139 the whole queue was drained for the hotkey
        // scan and never forwarded, so pointer input never reached a windowed document.
        // A focused text field owns the keyboard (Docs/design/12-compiler.md 12.6): no hotkeys then.
        const bool textFocus = app.context().wantsTextInput();
        for (const xt::InputEvent& event : events) {
            const bool hotkey = !textFocus && event.kind == xt::InputEvent::KeyDown &&
                                (event.key == xt::KeyCode::Space || event.key == xt::KeyCode::Left ||
                                 event.key == xt::KeyCode::Right || event.key == xt::KeyCode::KeyR ||
                                 event.key == xt::KeyCode::Escape);
            if (!hotkey) {
                app.context().pushInput(event);
                continue;
            }
            xt::Document* doc = app.context().document(handle);
            if (doc == nullptr) {
                continue;
            }
            switch (event.key) {
                case xt::KeyCode::Space: {
                    paused = !paused;
                    for (xt::ElementRef anim : animatedElements(*doc)) {
                        uint32_t id = doc->defaultAnimation(anim);
                        if (paused) {
                            doc->pause(id);
                        } else {
                            doc->resume(id);
                        }
                    }
                    break;
                }
                case xt::KeyCode::Left:
                case xt::KeyCode::Right: {
                    float sign = event.key == xt::KeyCode::Right ? 1.0f : -1.0f;
                    for (xt::ElementRef anim : animatedElements(*doc)) {
                        uint32_t id = doc->defaultAnimation(anim);
                        float frameRate = frameRateFor(*doc, anim);
                        float localT = doc->animator().localTime(id, app.context().clock());
                        doc->seek(id, localT + sign / frameRate);
                    }
                    break;
                }
                case xt::KeyCode::KeyR: {
                    // T-260b: loaded beside the current Document (0: loadInto() unloads nothing),
                    // which goes only after the flow was loaded again below.
                    xt::DocumentHandle reloaded = 0;
                    // T-193: the global flow listens on the main Document; drop it while that
                    // Document is still alive, and rebuild it against the new handle below.
                    targets.project.bundle.reset();
                    // T-213: a UI Project input is packed again, so R shows its edited sources.
                    if (!args.packSource.empty()) {
                        packProject(args.packSource, args.bundleDir, args.standardSymbolDir,
                                    args.standardFontDir);
                    }
#ifndef XT_GL_NO_TEXT
                    if (args.bundle) {
                        (void)args.bundle->reload();  // T-555: a font the re-packed UI newly names.
                        addBundleFonts(args, app.textShaper());
                    }
#endif
                    // T-263c: an image file rewritten under the same name is decoded again.
                    // T-263e: and a file that did not exist when it was first asked for is
                    // looked for again (a failed lookup has no file to revalidate).
                    app.assetResolver().revalidate();
                    app.assetResolver().resetAssetIds();
                    if (loadInto(app.context(), args.input, rootDir, args.assetRoots, reloaded)) {
                        const xt::DocumentHandle previous = handle;
                        handle = reloaded;
                        app.setCurrentDocument(handle);
                        app.syncViewport();
                        paused = false;
                        // T-135: the old graph's listeners are on the previous Document, so a
                        // reload re-reads the flow file too (view_flow.h). A failed reload leaves
                        // the run without a graph rather than killing the window.
                        hasFlow = xt::tools::view::reloadFlow(app.context(), previous, handle, args.flow,
                                                              hasFlow, flowRuntime);
                        if (hasFlow) {
                            // T-546: the reloaded main flow's diagnostics print again, as at the
                            // first load; under --strict they print as errors but never exit
                            // (this is an interactive session), so the result is ignored.
                            (void)xt::tools::view::reportFlowContinuity(flowRuntime, args.strict);
                        }
                    }
                    if (args.wholeProject && !setupProjectFlows(args, app.context(), handle, targets)) {
                        targets.project.bundle.reset();  // Run on without the project flows.
                    }
                    if (targets.project.bundle && targets.project.bundle->flowRuntime().isGlobal()) {
                        // T-546: likewise for the reloaded global flow.
                        (void)xt::tools::view::reportFlowContinuity(targets.project.bundle->flowRuntime(),
                                                                    args.strict);
                    }
                    break;
                }
                case xt::KeyCode::Escape:
                    app.quit();
                    break;
                default:
                    break;
            }
        }
    };

    // T-384b (23 §23.ad): --second-window, another file in a second window with its own Context
    // (same loading code, fonts and asset resolver). Created last, so every main-window call above
    // ran before a second GL context existed. run(onFrame) calls onFrame for the main window only;
    // the second window's input goes straight to its Context.
    xt::DocumentHandle secondHandle = 0;
    if (!args.secondWindow.empty()) {
        const fs::path secondPath = pathFromUtf8(args.secondWindow);
        const std::string secondTitle = "XTFrame - " + pathToUtf8(secondPath.filename()) + " (2)";
        xt::shell::WindowConfig second;
        second.width = args.secondWidth > 0 ? args.secondWidth : args.width;
        second.height = args.secondHeight > 0 ? args.secondHeight : args.height;
        second.title = secondTitle.c_str();
        second.msaa = args.msaa;
        second.hidden = args.hidden;
        second.liveResize = args.liveResize;
        second.vsync = false;  // 23 §23.ad: only the main window waits for vsync.
        if (args.secondDisplay >= 0) {
            if (args.secondDisplay >= app.displayCount()) {
                std::fprintf(stderr, "xtframe-view: --second-display %d: there are %d display(s)\n",
                             args.secondDisplay, app.displayCount());
                return 1;
            }
            const xt::Rect bounds = app.displayBounds(args.secondDisplay);
            second.x = static_cast<int>(bounds.x + (bounds.w - static_cast<float>(second.width)) * 0.5f);
            second.y = static_cast<int>(bounds.y + (bounds.h - static_cast<float>(second.height)) * 0.5f);
        }
        const xt::shell::WindowId secondId = app.createWindow(second);
        if (secondId == xt::shell::kInvalidWindow) {
            std::fprintf(stderr, "xtframe-view: failed to open the second window: %s\n", app.lastError());
            return 1;
        }
        xt::shell::Window& secondWindow = app.window(secondId);
        if (!loadInto(secondWindow.context(), args.secondWindow, pathToUtf8(secondPath.parent_path()),
                      args.assetRoots, secondHandle)) {
            return 1;
        }
        secondWindow.setCurrentDocument(secondHandle);
        secondWindow.setStatsLog(args.stats);
        secondWindow.context().setDevice(args.device);
        secondWindow.syncViewport();
        if (!args.hidden) {
            secondWindow.raise();  // T-398 (23 §23.ad.1): the new window comes to the front.
        }
        app.setWindowClosedHandler([](xt::shell::WindowId id) {
            std::fprintf(stderr, "xtframe-view: window %u closed\n", static_cast<unsigned>(id));
        });
    }

    if (!args.screenshot.empty()) {
        auto capture = [&](const std::string& path) -> bool {
            // T-240: App reads the window back (glReadPixels on the GL path, the SDL_Renderer's
            // pixels on the SW path), top-down.
            std::vector<uint8_t> pixels;
            int width = 0;
            int height = 0;
            if (!app.readPixels(pixels, width, height)) {
                std::fprintf(stderr, "xtframe-view: cannot read the window back for '%s'\n", path.c_str());
                return false;
            }
            return writeScreenshot(path, pixels.data(), width, height, width * 4);
        };

        // T-165: --warmup lets the captured frame be one the GL path has already seen (effect
        // cache store on the second build, hit from the third; flows and animations advance).
        // --screenshot-every N also writes every Nth warm-up frame as <stem>_<frame><ext>.
        const std::filesystem::path shot = pathFromUtf8(args.screenshot);
        for (uint32_t i = 0; i < args.warmup; ++i) {
            app.runFrame(onFrame, args.warmupDt, /*swap=*/false);
            if (args.screenshotEvery > 0 && (i + 1) % args.screenshotEvery == 0) {
                std::filesystem::path p = shot;
                fs::path name = shot.stem();
                name += "_" + std::to_string(i + 1);
                name += shot.extension();
                p.replace_filename(name);
                if (!capture(pathToUtf8(p))) {
                    return 1;
                }
            }
        }
        app.runFrame(onFrame, args.warmup > 0 ? args.warmupDt : 0.0f, /*swap=*/false);
        if (!capture(args.screenshot)) {
            return 1;
        }
        return 0;
    }

    int exitCode = app.run(onFrame);
    if (args.perf) {
        if (csv) {
            csv->writeNewFrames(app.context().perf());
        }
        std::printf("xtframe-view: render/effects/text are GL CPU submission times (no glFinish)\n");
        printPerfSummary(app.context().perf().summary(xt::PerfTracker::kHistoryCapacity));
    }
    return exitCode;
}
