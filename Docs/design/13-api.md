# 13 · 公開 C++ API

所有公開標頭位於 `include/xt/`。宿主只需 `#include <xt/xt.h>`。

## 13.1 Context

```cpp
namespace xt {

struct ContextConfig {
    FontProvider*  fonts  = nullptr;   // nullptr → 內建 NullFontProvider
    AssetResolver* assets = nullptr;   // nullptr → 內建 FileAssetResolver(".")；FileAssetResolver 亦可帶多個根目錄依序試（T-138，22 §22.2）
};

using DocumentHandle = uint32_t;   // 0 = 無效

struct TimelineDesc {              // 程式建立時間軸；語意同 04.4
    std::string_view name; float duration;
    std::vector<Track> tracks; std::vector<Keyframe> keyframes;
    std::vector<float> floatPool; std::vector<GradientStop> stopPool; std::vector<Marker> markers;
};

class Context {
public:
    explicit Context(const ContextConfig&);
    ~Context();
    Context(const Context&) = delete;

    // 文件
    Result<DocumentHandle> loadDocument(IrBlob, bool takeOwnership);   // takeOwnership=false 時宿主須維持 blob 存活
    Result<DocumentHandle> loadDocumentFile(std::string_view path);
    void      unloadDocument(DocumentHandle);
    // 0.3（22.5 / T-93）：同一 handle 換新 IR；視口、時鐘、輸入佇列不動，文件狀態全部重置。
    // 呼叫端持有的 Document*、ElementRef、listener id 全部失效，必須重取。
    Result<DocumentHandle> reloadDocument(DocumentHandle, IrBlob, bool takeOwnership);
    Document& document(DocumentHandle);

    // 每幀
    void setViewport(Rect logical, float devicePixelRatio);
    void pushInput(const InputEvent&);
    void update(float dt);
    void render(CommandList& out);
    float clock() const;
    Cursor cursor() const;
};
}
```

## 13.2 Document 與 Element

```cpp
class Document {
public:
    // 查詢
    ElementRef root();
    ElementRef byId(std::string_view id);                 // O(n) 第一版；找不到回傳無效 ref
    void       query(std::string_view selector, std::vector<ElementRef>& out);   // 支援 05.3 選擇器
    // 建立/刪除
    ElementRef createElement(std::string_view tag);       // 未附加；tag 必須存在於 IR 字串表，否則 intern 到執行時字串池
    void       appendChild(ElementRef parent, ElementRef child);
    void       insertBefore(ElementRef parent, ElementRef child, ElementRef before);
    void       remove(ElementRef);                        // 含子樹
    // 事件
    uint32_t addListener(ElementRef, EventType, EventListener);
    void     removeListener(uint32_t);
    // 動畫（程式 API）
    TimelineId createTimeline(const TimelineDesc&);      // 執行時建立，格式同 04.4
    uint32_t   play(const PlayParams&);
    void stop(uint32_t); void pause(uint32_t); void resume(uint32_t);
    void seek(uint32_t, float localT); void setSpeed(uint32_t, float);
    bool isPlaying(uint32_t) const;
    void onAnimationFinished(uint32_t, std::function<void()>);
    // 執行時字串（例外規則 1.7）
    StringId intern(std::string_view);
    std::string_view string(StringId) const;
};

struct PlayParams {
    TimelineId timeline;
    ElementRef target;         // anim 元素（Lottie 時間軸）或任一元素（kSelfNode 時間軸）
    float delay = 0, speed = 1, durationScale = 1;
    float segStart = 0, segEnd = -1;    // -1 = duration
    LoopMode loop = LoopMode::Once; uint16_t iterations = 1;
    FillMode fill = FillMode::Forwards;
    uint8_t layer = 0;
};
```

## 13.3 ElementRef

輕量控制代碼 `(Document*, NodeId, generation)`；節點被刪除後 `valid()` 為 false（NodeArena 每個 slot 帶 `generation` 計數，重用時 +1，此為 01.3 的補充欄位）。

```cpp
class ElementRef {
public:
    bool valid() const;
    std::string_view tag() const;  std::string_view id() const;
    bool hasClass(std::string_view) const;
    void addClass(std::string_view); void removeClass(std::string_view); void toggleClass(std::string_view);
    std::string_view attribute(std::string_view name) const;
    void setAttribute(std::string_view name, std::string_view value);   // "value"、"src"、"disabled" 等有語意的屬性會觸發對應行為
    std::string_view text() const;  void setText(std::string_view);
    // 樣式：直接寫 inline 宣告（等同 style 屬性），觸發 kDirtyStyle 與 transition
    void setStyle(std::string_view cssDeclarations);                    // 執行時解析器：只支援單一 `prop: value` 清單，實作於 core 的迷你解析器（05.3 值語法子集，不含選擇器）
    void setProperty(PropId, const Value&, bool sticky = false);        // sticky=true → Override 層（04.7）
    void clearOverride(PropId);
    Value property(PropId) const;                                       // 最終值
    // 狀態
    bool  hasState(ElementState) const;  void setState(ElementState, bool);   // disabled/checked 可由宿主設定；hover/active/focus 唯讀（設定被忽略）
    void  focus();
    // 幾何
    Rect  boundingBox() const;         // 視口座標 AABB（含變換）
    LayoutBox layoutBox() const;
    // anim 專用
    NodeId  sceneNode(std::string_view lottieLayerName) const;          // 找場景內命名節點，供 setProperty 於場景節點
    uint32_t defaultAnimation() const;                                  // autoplay 建立的 instance id
    ElementRef parent() const; ElementRef firstChild() const; ElementRef nextSibling() const;
};
```

場景節點的屬性寫入：`Document::setNodeProperty(NodeId, PropId, const Value&, bool sticky)`，與 ElementRef 版本相同語意。

## 13.4 Value 建構輔助

```cpp
Value  makeFloat(float);  Value makeVec2(float, float);  Value makeColor(float r, float g, float b, float a);
Value  makeLength(float, LengthUnit);  Value makeEnum(uint32_t);
Result<Value> parseValue(PropId, std::string_view css);   // "12px"、"#ff0000"、"0.5"
```

## 13.5 Shell（`xt_shell`，可選）

```cpp
namespace xt::shell {
struct WindowConfig { int width = 1280, height = 720; const char* title = "XTFrame"; bool vsync = true; int msaa = 4; };
class App {
public:
    App(const WindowConfig&);
    Context& context();                     // 已接好 xt_text 與 FileAssetResolver
    void addFontDir(std::string_view);
    int  run(std::function<void(float dt)> onFrame);   // 主迴圈：SDL 事件 → pushInput；onFrame；update；render；gl；swap
    void quit();
};
}
```

T-376（23 §23.ab，無邊框宿主）：`WindowConfig` 加 `bool borderless = false`（`SDL_WINDOW_BORDERLESS`）、`bool borderlessShadow = true`（只在 borderless 且 Windows 生效：`DwmExtendFrameIntoClientArea` 1 px 取得 DWM 陰影；其他平台 no-op）、`int resizeBorder = 8`（邏輯像素；0 = 不做邊框調整大小）。`enum class HitRegion { Normal, Draggable, ResizeTopLeft, ResizeTop, ResizeTopRight, ResizeRight, ResizeBottomRight, ResizeBottom, ResizeBottomLeft, ResizeLeft }` 一對一對應 `SDL_HitTestResult`；`App::setHitTest(std::function<HitRegion(int x, int y)>)` 覆蓋預設實作（預設 = 邊框區 → `Document::hitTest` 找元素 → 祖先鏈第一個非 `none` 的 `app-region`，05 §5.9.10）；回呼只能讀不變資料（SDL 在平台訊息處理中呼叫）。純函式 `hitRegionFor(x, y, windowW, windowH, resizeBorder, maximized, appRegionAt)` 供測試。視窗控制：`App::minimize()`、`maximize()`、`restore()`、`toggleMaximize()`、`bool isMaximized() const`、`close()`（送 `SDL_QUIT` 走既有 quit 流程）；最大化／還原事件由 `setEventHook` 看 `SDL_WINDOWEVENT_MAXIMIZED / RESTORED`。

T-377（23 §23.ab live resize）：`WindowConfig::liveResize = true`。開著時 `App` 建構期 `SDL_AddEventWatch` 裝一個 watch：在 `run()` 期間、不在 `runFrame()` 之內收到 `SDL_WINDOWEVENT_SIZE_CHANGED`（或 `EXPOSED`）時，更新 drawable 尺寸、`syncViewport()`，並以實際經過時間（steady_clock）為 dt 執行一次 `runFrame(最近一次 onFrame, dt, swap=true)`。這是因為 Windows／macOS 拖曳邊框期間 OS 進入 modal size/move loop，`SDL_PollEvent` 不返回；watch 在主執行緒該 loop 內同步被呼叫，可安全繪製。**宿主的 `onFrame` 因此可能在 `SDL_PollEvent` 之外被呼叫**（仍在主執行緒、仍不重入）。GL 與 SW 呈現路徑皆適用。

T-384（23 §23.ad 多視窗宿主）：`App` 成為視窗管理者。`using WindowId = uint32_t`（0 = 主視窗，建構時建立）。`WindowId createWindow(const WindowConfig&)`（失敗回 `kInvalidWindow` 並設 `lastError`）、`Window& window(WindowId)`、`Window* findWindow(WindowId)`、`void closeWindow(WindowId)`（主視窗 → 等同 `quit()`）、`std::vector<WindowId> windows() const`、`setWindowClosedHandler(std::function<void(WindowId)>)`（副視窗被使用者關閉時呼叫，之後銷毀）。`class Window`：每視窗一個 `Context`、渲染器（GL：自己的 GL context + `GlRenderer` + `GlyphAtlas`；SW：自己的 `SDL_Renderer` + texture）、fit 目標、scene、文字輸入狀態、hit-test、`currentDocument`，方法與 `App` 既有的逐視窗方法同名（`context()`、`setCurrentDocument`、`setTitle`、`setFit`、`setLogicalHeight`、`syncViewport`、`setHitTest`、`minimize/maximize/restore/toggleMaximize/isMaximized/close`、`setScene`、`readPixels`、`devicePixelRatio`、`drawableWidth/Height`），另加 `bounds()/setBounds(x,y,w,h)`、`displayIndex()`、`id()`、`sdlWindow()`。`App` 的同名既有方法**轉呼叫視窗 0**，舊宿主不改。共用：`FtTextShaper`、`AssetResolver`（兩者在 `App`）、gamepad。顯示器查詢：`App::displayCount()`、`App::displayBounds(index) → Rect`、`displayUsableBounds(index)`。事件：一次 `SDL_PollEvent`，依 `event.window.windowID`／`event.*.windowID` 派到該視窗的 `Context::pushInput`；焦點、IME、指標捕捉逐視窗；gamepad 事件送到有鍵盤焦點的視窗（無則主視窗）。出幀：`runFrame` 逐視窗 `onFrame(dt, windowId)`（新 overload；舊的 `onFrame(dt)` 只對主視窗呼叫一次，副視窗照常 update/render）→ update → render → swap；live resize watch 以 windowID 找視窗。`WindowConfig::vsync=false` + `App::setTargetFps(int)`（0 = 不限）給多視窗固定節拍。

T-156：`xt::shell` 加 `enum class FitMode { Native, Stretch, Contain, Cover, Center }`、`parseFitMode`、`fitPlacement(mode, designW, designH, windowW, windowH)`、`WindowConfig::fit/designWidth/designHeight`、`App::setFit/fit`。

## 13.6 穩定介面（0.3 起，T-100）

`#include "xt/xt.h"` 匯總核心的宿主契約：`context.h`（含 `DamageList`）、`document.h`/`element_ref.h`、`input.h`、`asset_resolver.h`、`text_shaper.h`、`command_list.h`、`perf.h`、`damage.h`、`version.h`。非核心的由宿主自行包含：`xt/sw/sw_renderer.h`、`xt/bundle/bundle.h`（`Bundle::open`、`loadUi`、`reloadUi`，見 22.5；`ContextConfig.assets` 必須指向 `bundle.assets()`）、`xt/flow/flow_runtime.h`、`xt/flow/host_ports.h`（T-144：`HostPorts`——宿主不經 `.xtflow` 直接對 17 §17.6 的文件連接點送值/收事件：`bind/rebind`、`setInput(port, PortValue, param)`、`addOutputListener(port, fn)`/`removeOutputListener`、`tick(dt)`、`ports()`、`lastError()`；型別規則與 `FlowRuntime` 同源（值型輸入須同型，不轉換；事件型輸出在 `Context::update()` 派發時送出，`clock/viewport*/localTime` 由 `tick()` 輪詢）。宿主插件生成的節點建在它之上。）

規則：這些標頭 major 版本內只加不改；新增成員加在結尾且有預設值；不得包含 `*_internal.h` 或 `src/` 私有標頭、不得暴露第三方型別；刪除或改語意 → major +1。`Tools/Scripts/check_public_headers.sh` 在 CI 檢查，`Examples/engine_integration` 只能包含這份清單（外加明列的範例用工具標頭）。完整表與理由見 23 §23.6。

## 13.7 版本與 ABI

`xt/version.h` 提供 `XT_VERSION_MAJOR/MINOR/PATCH`。公開類別以 pimpl 隱藏實作（`Context`、`Document`），`ElementRef`/`Value`/`CommandList` 等資料型別為公開佈局且承諾在 major 版本內不變。

## 13.8 編輯器藍圖新增（2026-09-27 定案；對應 27、28、30 章與 23.10）

尾端新增、預設實作回 `kUnsupported`（23.6 穩定介面規則）。

### 13.8.1 HostPorts 升級

- 型別：`Number, String, Bool, Color, Vec2, Vec3, Entity, Enum, Struct, List, Texture, Event`；方向 `In, Out, Call`。
- `PortId lookup(std::string_view name)`（含路徑 id 與 `ui#id.port`）；`set(PortId, Value)`、`Value get(PortId)`（讀回）、`call(PortId, args) → Value`、`emit(PortId, args)`（UI → 宿主的 out event 以 listener 收）。
- 清單：`listSet(PortId, span<Struct>)`、`listInsert(PortId, index, Struct)`、`listRemove(PortId, index)`、`listUpdate(PortId, index, Struct)`。
- 多實例：`Context` 的 `instanceId` 由宿主指定；事件回呼帶 `instanceId`。
- 自訂轉換器：`registerConverter(name, fn(Value, args) → Value)`，op 編號 0x80 起。
- 連接點表：`portTable()` 供宿主生成節點（23.10）與強型別標頭（`xtframe-pack --gen-ports cpp|cs`）。
- 2026-10-20 補記（Aux Duty XF-023）：`xt::flow::HostPorts` 兩代 API 並存：宣告的 `port:` 以本節的新一代為準（`lookup`／`set(PortId, Value)`／`setText`／`list*`），13.6 的 `setInput("#id.port", PortValue)`／`addOutputListener` 保留相容，並仍是 17 §17.6 元素內建連接點（`#title.setText` 等）的入口。`setText`／`Document::setPortText` 對 struct port 接受 JSON 物件、`list<struct>` 接受物件的 JSON 陣列（T-177）、`list<基本型別>` 接受值的 JSON 陣列（T-381）；標頭註解已對齊。

### 13.8.2 時間軸、狀態、主題、縮放、語言

- `playTimeline(scopePath, name, PlayOptions)`、`stopTimeline`、`seekTimeline(t)`、`timelineEvents(listener)`（end、marker）。
- `setCustomState(ElementRef, name, on)`、`setElementState`（既有）。
- `setTheme(name)`（換 `VARS` 值不重編）、`setUiScale(float)`、`setLanguage(lang)` + `ILocalizationProvider { resolve(key, lang) → utf8 }`。
- `setDevice(DeviceInfo{ aspect, safeArea, dpr rule, inputDevice, platform })` → `@media` 重新評估。

### 13.8.3 外部內容、字形、音效、震動、生命週期、世界座標

- `IExternalTexture`：宿主對 `External` 節點 `bindTexture(nodeId, textureHandle, w, h)`、`setPlayback(nodeId, state, time)`；核心送 `play/pause/seek/loop/rate` 請求與 `ended` 事件。
- `IGlyphProvider { resolve(action, device) → AssetRef }`、`setInputDevice(kind)`。
- out 事件：`sound(cue, params)`、`haptic(cue)`、`spawnRequest`（宿主要在世界中生成時）。
- `requestHide(surface)` → 觸發 `hide` → `OnHidden` 回呼；`isHiding()`。
- `setWorldAnchor(PortId, screenX, screenY[, depth])`：宿主換算世界座標後餵入。
- `hotReload(bundleDelta)`：manifest／樣式／字串／字型／符號變更不重建 Context，保留 port 值與狀態（22.10）。
- flow：`FlowRuntime` 可由宿主載入 `bundle.xtflow` 與各 UI 的 `.xtflow`，開關 `enableFlow(bool)`。

### 13.8.4 T-172 實作簽名（#171）

`Context::playTimeline(std::string_view scopePath, std::string_view name, const PlayOptions&) → Status`、`stopTimeline(scopePath, name)`、`seekTimeline(scopePath, name, float t)`（作用於前景起第一個含該群組的文件）；`Document::timelineTime(name)`、`isTimelinePlaying(name)`、`setTimelineListener(TimelineListener*)`（單一 listener；`removeTimelineListener` 之後補）。`PlayOptions { bool reverse, loop, once, hold; std::string_view fromMarker; float rate = 1; }`。

### 13.8.5 T-170 實作簽名（#172）

`Context::defineTheme(name, std::span<const TokenDef>)`、`setTheme(name)`（`""` = 基底；未知 → `kNotFound`）、`theme()`、`setToken(name, Value)`（Float/Length/Color/Enum）。查找順序：`setToken` 覆寫 → 作用中主題 → IR `VARS` → fallback。變更只把上次解析命中 `var()` 的元素及其子樹標 `kDirtyStyle`。`ElementRef::setStyle` 支援整值 `var()`。

### 13.8.6 T-176 實作簽名（#174）

`Context::setDevice(const DeviceInfo&) → Status`／`device()`、`setUiScale(float) → Status`／`uiScale()`（非法 → `kInvalidArgument`）；`DeviceInfo{ float aspect (0 = 視口 w/h); Insets safeArea (視口比例); float dpr; DeviceKind platform {Desktop, Console, Handheld}; InputKind input {Mouse, Gamepad, Touch}; }`。`setDevice` 會把 Context 視口重新套到每個文件（`loadDocument` 尚不自動給新文件視口）。`xt_shell`：`parseDeviceAspect/parseSafeArea/parseDeviceKind/parseInputKind`；`xtframe-view --device W:H --safe F|T,R,B,L --platform --input`。

### 13.8.7 T-179 實作簽名（#176）

`ILocalizationProvider { virtual std::string_view resolve(key, lang); }`（空 = 顯示鍵；定義於 `xt/document.h`）；`Context::setLocalizationProvider(p)`、`localizationProvider()`、`setLanguage(lang) → Status`（只准 `[A-Za-z0-9_-]*`，`""` 清除）、`language()`；`Document::setLocalization(p, lang)`、`localizationKey(ElementRef)`。`:lang()` 優先文件語言。manifest 的 `fonts.fallback` 僅資訊用，執行時用各 `.xtir` 內編譯的鏈。CE 的 `ILocalizationManager` 對接待 CE 單。

### 13.8.8 T-178 實作簽名（#177）

`IGlyphProvider { AssetRef resolve(std::string_view action, InputKind); }`（`AssetRef{name}`）；`Context::setGlyphProvider(p)`、`setInputDevice(InputKind)`（只換 `DeviceInfo.input` 後呼叫 `setDevice`）；`Document::setGlyphProvider`、`hitTestTextLink(pos) → link index`（run 級命中）。

### 13.8.9 T-175 實作簽名（#178）

`HostPorts`（尾端新增）：`lookup(name)`、`set(PortId, Value)`、`setText`、`get`、`call(PortId, span<Value>) → Result<Value>`、`setCallHandler(PortId, PortCallHandler)`、`addEmitListener((PortId, Value, instanceId))`／`removeEmitListener`、`listSet/listInsert/listRemove/listUpdate`、`registerConverter(name, fn) → Result<u32>`、`portTable() → vector<PortRecord>`、`setInstanceId`。Document：`lookupPort/ports/portInfo/setPort/setPortText/getPort/portText/callPort/setPortCallHandler/list*/registerConverter/addPortListener/removePortListener`（`PortEvent{kind: Changed|Emit|ListChanged, port, value, index, instanceId}`）、`setInstanceId/instanceId`；Context：`setInstanceId(h, id)`、`instanceId(h)`；`HostEvent` 尾端加 `value, port, instanceId`。`Value`：`Struct = 11`、`List = 12`（尾端；`sizeof` 不變，資料重用既有槽；`ofStruct/ofList`；`valueEquals` 逐項）。port 值編碼：Bool = Float 0/1、Entity/Enum = Enum、String/Texture = StringRef。`call` 設值、跑一次 `port()` 觸發、回最後 `emit` 值；UI 端 `emit(<call port>)` 呼叫宿主處理器並存回傳。`.xtflow` 可列 struct/list port 但不可接線。

- T-187：`Context::clearToken(name) → Status`（名稱非法 `kInvalidArgument`、無覆寫 `kNotFound`）、`clearTokens()`。

### 13.8.10 T-191 實作簽名（#188）

`IExternalTexture`（`event.h`，`Document` 實作）：`bindTexture(NodeId node, u32 handle, int w, int h) → Status`、`setPlayback(NodeId, MediaState, float time)`（node 可為元素或其 External 子節點；走 T-150 `engine://` 路徑，GpuFrame 不變）。另：`Document::bindImage(node, const ImageData*)`（SW 用 CPU 像素）、`unbindExternal`、`playMedia/pauseMedia/stopMedia/seekMedia/setMediaRate/setMediaLoop/setMediaDuration`、`setExternalRequestListener`、`setExternalPlaceholders`、`setSequencePreload(N=8)`、`externalOf/externalData/externalElements/takeExternalDamage`；`ContextListener::onExternalRequest(const ExternalRequest&)`（尾端；`ExternalRequestKind` Play/Pause/Seek/Loop/Rate）、`Context::setExternalPlaceholders`；`EventType::MediaEnd`；`AssetResolver::setExternalPixels`、`ExternalTexture::pixels`（SW 繪製）。序列由核心自播：顯示幀變更時對後 N 幀 `imageView` 預載（loop 時環繞）；逐幀換 `imageAsset`，圖集換 `imageSubUv`；`FileAssetResolver` 不淘汰（長序列記憶體增長）。重綁貼圖標該元素區域 damage。

### 13.8.11 T-194 簽名（#192）

Document：`focus(player)`、`setFocus(ref, player)`、`navigate(dir, player)`、`setFocusTrap`、`setNavRepeat`、`dragSource/dragOver/dragGhostOffset/cancelDrag/dropTargetAt/itemIndexOf/contentHitTest`；Context：`setSurfaceZ/surfaceZ`、`setSurfaceInput/surfaceInput`（`SurfaceInput` Block/Pass/Watch）、`setPlayerDevice/playerDevice`、`DragCross`、`ContextListener::onDragCross`；shell：`App::enableGamepad(maxPlayers)`、`App::setGestureConfig`；`xtframe-view --players N`、`--event "press A <player>"`、黃金腳本 `pad <player> <button>`。

### 13.8.12 T-193 簽名（#193）

`xt::flow::BundleContext { registerUi(uiId, handle), enableFlow(bool), postSystemEvent(kind, value), setHostEventListener(FlowHostEvent), lifecycle listener }`；`FlowRuntime::loadGlobal(json|graph, BundleContext&, errors)`、`postSystemEvent(kind, value) → Status`（未知 kind `kInvalidArgument`）；`FlowHostEvent { kind: Sound|Haptic|Spawn|Emit, uiId, instanceId, nodeId, … }`。`Bundle::globalFlow()`。

### 13.8.13 T-200 簽名（#198）：世界錨點、系統事件、`hotReload`

- `Document::setWorldAnchor(PortId, float x, float y, float depth = 0, bool onScreen = true)`、`Context::setWorldAnchor(DocumentHandle, PortId, x, y, depth = 0, onScreen = true)`（PortId 屬於單一文件，故加 handle）。語意：以 `left/top` 語意把 margin box 左上角放到該點（只看版面盒，忽略 transform 與捲動），寫入 run-time translate 並在每次版面後重套；**會取代該元素的 CSS translate**。回 `kNotFound`／`kInvalidArgument`。
- `Context::postSystemEvent(kind, value)`：接受 flow 的 kind 名（`language` 為 `languageChanged` 別名；未知回 `kInvalidArgument`），對每個已載入文件設隱含 `system.*` port，再呼叫尾端新增的 `ContextListener::onSystemEvent(kind, value)`。`BundleContext::postSystemEvent` 改經 Context 再由 `onSystemEvent` 轉給各流圖，宿主直接對 Context 發也會到流圖。字串 port 只在值變更時通知（一般 port 語意），流圖字串 kind 每次 post 皆觸發。 2026-10-01（T-240）：新增 kind `back`（隱含 port `system.back`，flow System 節點 `back` 輸出；Android 返回鍵、桌面未用），既有 IR 不受影響。
- `Status Context::hotReload(const BundleDelta&, HotReloadReport* = nullptr)`；`BundleDelta{optional<vector<TokenDef>> tokens; vector<ThemeDef> themes; optional<string> theme; optional<ILocalizationProvider*> strings; bool fonts; optional<IGlyphProvider*> glyphs; bool symbols; vector<IrReload{handle, blob, takeOwnership}> ir}`。非 IR 差異不重建文件：tokens → `setToken`（疊在主題之上）、themes → `defineTheme`、theme → `setTheme`、strings → `setLocalizationProvider`（同指標重傳即重解 `@key`）、fonts → 全文字重排、glyphs → `setGlyphProvider`、symbols 僅資訊。`ir`：新文件承接舊時鐘，並依名稱搬移 port 值（enum 依值名、struct 依欄位、list 逐項並重建 `<repeat>`）、自訂狀態（元素 id + 狀態名，靜默）、各玩家焦點（`setFocus`，會發 Focus 事件）、所有作用中時間軸群組的播放頭／播放狀態／loop／reverse／hold／rate。還原期間不觸發 port 監聽、port/state 觸發、時間軸事件與 transition。舊狀態引用的東西消失（元素、狀態、時間軸、port、型別改變、enum 值）→ 該文件全新開始並回尾端新增的 `Status::kIncompatible`，`HotReloadReport{documents[{handle, status, missing[]}]}` 列 `"element A"`、`"port hp"` 等；載入失敗保留舊文件。未涵蓋：新文件的 autoplay 仍從時鐘 0 起；surface 可見性不承接。

### 13.8.14 T-204（#203）：`BundleGlyphProvider`

`BundleGlyphProvider(const Bundle&, IGlyphProvider* fallback = nullptr)`：先查 manifest `glyphs`（`InputKind::Mouse` 查 "mouse" 後退到 "keyboard"，核心 InputKind 無 Keyboard），查不到才問宿主 fallback。`FileAssetResolver` 未改（glyph 值是普通資產名，本來就能服務）。宿主可改用它，不必再反查 `assets[].sources`。

> T-215（#214）：`FileAssetResolver::resetAssetIds()`（AssetId 對照與 header 尺寸快取重置，依路徑解碼快取保留）；嵌入資產解碼快取以內容雜湊為鍵。

> 2026-09-30 裁決（T-216 發現）：`Context::loadDocument()` 應對新載入的文件自動套用 Context 目前的 viewport（如同它已套用 device）；目前 `setViewport()` 只作用在已載入文件，先 `setViewport` 後 `loadDocument` 的宿主會得到 0×0 視口（填滿視口的根畫出 0 像素）。歸下一張核心小單。

## 13.9 重入規則：callback 內卸載／重載文件（2026-10-02 審查修正；T-260a #273）

**dispatching**：`Context` 的任一個「會執行 callback 的進入點」在呼叫堆疊上時，Context 處於 dispatching。這些進入點是 `Context::update`、`postSystemEvent`、`setLanguage`、`setLocalizationProvider`、`setDevice`、`setGlyphProvider`、輸入派送，以及所有可能同步執行 trigger、listener 或宿主 callback 的公開 `Document`／`ElementRef` 呼叫。`Context::render` 刻意不是：繪製與 damage 比對不呼叫任何宿主介面。

dispatching 期間：

- **`unloadDocument(handle)`**：該文件**立刻對宿主而言已死**。`document(handle)` 回 `nullptr`；所有走訪已載入文件的地方（update、輸入路由、render、surface 查詢、viewport／device／語言／token 的散播）把它當成空槽跳過；它不再發出任何宿主 callback（元素／port／timeline listener、host event、OnShown／OnHidden、動畫事件與 finished callback、影片請求、call handler、converter、在地化查詢），連「正在它裡面執行的那次呼叫」的剩餘部分也不發。只有記憶體延後到最外層的 dispatching 呼叫返回時才釋放。重複卸載是 no-op。
- **`reloadDocument()`／`hotReload()`**：載入、驗證、狀態快照仍然同步完成並回傳狀態；只有「安裝」（換上新文件、銷毀舊文件）排入佇列，安裝前舊文件維持存活、可定址。對已排入卸載的 handle 重載回 `kInvalidArgument`。
- **`loadDocument()`** 維持同步並回傳新 handle；handle 永不重用。在 `Context::update` 期間載入的文件從下一幀才開始 update；`postSystemEvent` 期間載入的文件不收到該事件。
- 排入的卸載與安裝依呼叫順序執行；被先卸載的槽，其後的重載安裝被丟棄。

不在 dispatching 時，三個呼叫的行為與之前相同。沒有新增公開函式。

**不支援但記憶體安全**：在 callback 內巢狀呼叫 `Context::update()`。

待辦（T-260b）：`BundleContext`、shell、`xtframe-view` 與宿主插件裡「unload → 檢查 → load」的序列要依上述語意檢查一遍；`EventDispatcher::removeListener` 每 1024 次移除壓縮一次 listener 池並重編 id，同一次派送內約 1024 次移除會讓外層 `callListeners` 拿到舊 id。

## 13.10 重入規則補充：節點移除、surface 狀態、flow 與文件生命週期（2026-10-02 審查修正；T-260b #277）

- **`Document::remove()`**（含 `ElementRef` 移除）會一併放掉住在被移除子樹裡的東西：`<repeat>` 實例（live 與 exiting；其綁定與 trigger 節點）、整個 `<repeat>`（元素在子樹內時標為失效，之後忽略清單變更；pool 內的實例銷毀）、spawn 出來的實例的綁定、以及骨架在子樹內的 rig／形狀在子樹內的 skin 項。Blur／Leave listener 已經把節點移除時，`remove()` 直接返回。
- **實例綁定的 callback 內改同一個清單**是支援的（走訪以索引進行，每次求值後重新查找）。
- **槽釋放時**（立即或由佇列），Context 不再把該 handle 當作 pointer capture、hover、鍵盤 surface，其 surface z／輸入模式回到預設值。
- **同一次 dispatch 內「重載 handle 之後又卸載同一個 handle」**：新文件永不安裝，不發出任何宿主 callback。
- 卸載中的文件在同一次呼叫後段所做的動作，不會到達其他文件。
- 補正 §13.9 的待辦：`EventDispatcher` 的 listener 池壓縮只重連鏈結、不重編 id，外層 `callListeners` 仍有效（1100 次移除的測試留作防護）。

**`BundleContext` 與 flow**：

- `BundleContext` 擁有的全域與每 UI flow runtime 記的是 `DocumentHandle`，每次使用都經 `Context::document(handle)` 重新解析；被重載或卸載的 UI 直接跳過，直到 flow 再次載入。重載後呼叫 `loadGlobalFlow`／`loadUiFlow`、或卸載後銷毀 `BundleContext`，都不再碰已釋放的文件。
- 新增 **`BundleContext::removeUi(uiId)`**（未知 id 回 `kInvalidArgument`）：全域 flow 停止對它收送；只從 Context 在該 handle 仍持有的文件上移除 listener；它自己的 per-UI runtime 立刻停用、於下一次 `BundleContext::tick()` 銷毀。可在 `unloadDocument()` 之前或之後、以及任何 callback 內呼叫。全域 flow 保留指向 `ui#<uiId>` 的邊（到不了任何東西）；要綁替代品就重新載入 flow。
- 宿主**直接**對某個 `Document` 載入的 `FlowRuntime` 維持舊契約：文件必須活得比它久。換文件的順序是：載入新文件 → 對新文件載入 flow → 卸載舊文件（`xtframe-view` 的 R 鍵原本順序相反，是 use-after-free，已改）。
- **仍不安全**：在某個 flow 自己的 callback 內呼叫 `loadGlobalFlow`（或對同一個 runtime 呼叫 `loadUiFlow`）。等 dispatch 返回後再重載 flow。
- 已知未處理：`HostPorts`（`host_ports.cpp`）有同樣的 `removePortListener` 模式；`liveDocument()` 以指標比對（同一 handle 重載兩次而未重載 flow、且配置器重用位址時會誤判）。

## 13.11 審查修正鏈補記：宿主契約（2026-10-07 審查修正鏈合併；T-260c #316、T-263c～g、T-270a～f、T-255b）

**資產解析器**：
- `AssetResolver::releaseSource(const IrView&)`（預設 no-op）：Context 在卸載、重載安裝（含延後形式）、熱重載替換時呼叫；`~Context` 不呼叫——跨多個 Context 共用一個 resolver 的宿主（例如引擎插件）在銷毀 Context 前先卸載其文件，或呼叫 `resetAssetIds()`。
- `FileAssetResolver` 的解碼與標頭尺寸快取以資產名（非內嵌）或內嵌位元組範圍＋內容雜湊（每次 IR 載入確認一次；每槽最多 4 個已驗證序號）為鍵，不以 AssetId。具名解碼按存活來源（IrView 載入序號）計數；沒有存活來源的解碼（具名與內嵌皆是）進入以位元組為上限的閒置區（`setIdleDecodeBudget(bytes)`，預設 32 MiB，0 = 立即釋放），最近釋放者最後出；`resetAssetIds()`／`releaseSource()` 把解碼移到閒置區而非釋放（未變的內嵌圖片不重新解碼）。若存活文件的解碼本身超過預算，`resetAssetIds()` 會讓超出部分在下一幀重新解碼。`resetAssetIds()` 後所有 id 與 `imageView()` 指標失效。
- `setFileStatusFn(FileStatusFn)`：xt_core 不做檔案狀態呼叫；`xt::fileStatusUtf8` 在 xt_bundle（`Code/Runtime/bundle/src/file_status.cpp`），`Bundle::open` 設定；未設時 `revalidate()` 回 0。`revalidate()` 丟棄已變更／消失／被較近根遮蔽的檔案解碼（不含內嵌）。
- 路徑參數一律 UTF-8（`FileAssetResolver`、`Context::loadDocumentFile`、`BakedWriter::open`、`sw::writePng`／`readPng`、`FtTextShaper::addFontFile`／`addFontDir`、`readFontNameTable`）；非合法 UTF-8 視為不存在。

**序號**：`IrView::loadSerial()`（每次成功 `open()` 的行程內計數，隨 view 複製）、`Document::loadSerial()`、`Context::documentSerial(DocumentHandle)`。

**flow**：`FlowRuntime::unload()`；載入失敗後 runtime 為空；在 runtime 自己的派送內呼叫 `load`／`loadGlobal`／`BundleContext::loadGlobalFlow`／`loadUiFlow` 延後到派送結束（回 `kOk`，最後一次勝出，期間現有圖靜默；對已替換文件的延後 UI 載入讓 runtime 為空）；`BundleContext` 的 runtime 以載入序號辨識文件。`HostPorts` 仍以指標比對（需要新 API，待排程）。

**節點與文字**：回收中（在 pool 內等待）的 `<repeat>` 實例節點，所有會修改的 `ElementRef` 呼叫行為與已移除節點相同（`setAttribute`／`setStyle`／`setEffects` → `kInvalidArgument`；`setText`／`setProperty`／`clearOverride`／`setEffectParam`／`focus`／`setValue` 無作用；`addClass`／`removeClass`／`toggleClass`／`setState` → false），唯讀呼叫照常；實作為 `NodeFlags2::kPooled`。回收的實例重新取得模板的文字、失去其 `setStyle` 宣告與 id 直到再利用；宿主不得跨回收保留實例的 StringId。文字視圖與組字字串的生命週期見 07 §7.9。

**渲染**：`lastFrameReusable()` 每幀至多呼叫一次（它推進重試計數；靜態畫面上答 false 可能表示「建置一次，重試到期」）；`invalidateAsset(id, source)`；清單來源表與 `frameSerial`（09 §9.8）：宿主每幀交回同一份未動的清單，要簡化就用不改原清單的形式。

**深度**：`appendChild`／`insertBefore` 到達 `kMaxElementDepth` 為靜默 no-op。

## 13.9 捲動 API、repeat 查詢、shell 小工具（2026-10-20 定案；Aux Duty XF-059／060／063／064；T-387）

- `ElementRef::scrollTo(float x, float y)`、`scrollBy(float dx, float dy)`、`scrollPos() → Vec2`、`maxScroll() → Vec2`：只對 `overflow: scroll`／`auto` 元素有效（否則 `kUnsupported`），自動夾在 [0, max]，派 `Scroll` 事件；`scrollIntoView(ElementRef child, Align = Nearest|Start|Center|End)`。
- `Document::repeatIndexAt(std::string_view repeatId, Vec2 rootPos) → uint32_t`（0xFFFFFFFF = 無）：虛擬化下也可用（以項高與捲動位置推算，不需實例存在）。
- `xt::shell::App::openUrl(std::string_view)`（`SDL_OpenURL`）、`App::modifierState() → uint8_t`（`InputEvent::k*` 位元，`SDL_GetModState` 轉換）。`app.h` 仍公開 `<SDL.h>`（`setEventHook` 需要），不改。
- **路徑 id 慣例（總整理，原散在 26／27 章）**：`<實例id>/<內部元素id>`（符號內元素）、`<repeat id>/<i>/<模板相對id>`（清單項）、`<實例id>/<repeat id>/<i>/<模板相對id>`（符號內清單）、port 路徑 `<實例id>.<port>`／`<repeat id>/<i>/<實例id>.<port>`；`byId` 與 HostPorts `lookup` 都接受；`#` 選擇器不接受 `/`。**內部 id 是扁平的**（2026-10-21 補，Aux Duty XF-069）：`<實例id>/<內部元素id>` 不照 DOM 巢狀——`Player/Seek/Root` 與 `Player/Seek/Track` 都是 `Seek` 實例的直接內部 id，即使 `Track` 在 DOM 裡是 `Root` 的子元素；只有巢狀**符號實例**與 `<repeat>` 才多一段。 T-390 實作記錄：扁平路徑 id 即現行行為（符號展開把每個內部 id 改寫為 `<實例路徑>/<id>`，不含 DOM 祖先），本任務未改程式碼，只補文件與 skill `not-like-web.md`。
- **T-387 實作記錄**：
  - `enum class ScrollAlign : uint8_t { Nearest, Start, Center, End }`（`xt/document.h`）；`Status scrollTo(float, float)`、`Status scrollBy(float, float)`、`Vec2 scrollPos() const`、`Vec2 maxScroll() const`、`Status scrollIntoView(ElementRef child, ScrollAlign = Nearest)`。無效 ref → `kInvalidArgument`；沒有 scroll／auto 軸 → `kUnsupported`；`scrollIntoView` 的 child 不是後代 → `kInvalidArgument`。夾住後位置沒變仍回 `kOk`、不派事件。`scrollIntoView` 以 layout box 計算（中間層捲動容器的捲動量扣掉，不看 transform），可見區 = 容器 content box；Nearest：在上方或比可見區大 → 對齊頂端，在下方 → 對齊底端，否則不動。讀的是上一次 layout，在 `update()` 之後呼叫。
  - `Document::repeatIndexAt`：非虛擬化 = 存活實例根的 border box（世界變換反算）含該點者；虛擬化且已量到項高 = repeat 元素本身的本地座標（世界變換已含 viewport 捲動）`floor((y − content.y) / itemHeight)`，x 超出 repeat 盒或索引 ≥ 清單長度 → 0xFFFFFFFF；未知 id、spawn pool → 0xFFFFFFFF。
  - `bool App::openUrl(std::string_view)`（失敗記 log 回 false）、`uint8_t App::modifierState() const`；轉換函式 `xt::shell::sdlModifiers(uint16_t)` 在 `sdl_input.cpp`（無標頭，同 `translateSdlEvent`）。`SDL_OpenURL` 在 SDL 的 misc 子系統，`Tools/CMake/xt_sdl2.cmake` 改為 `SDL_MISC ON`。
