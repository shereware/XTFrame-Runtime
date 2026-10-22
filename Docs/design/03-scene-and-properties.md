# 03 · 場景圖、節點與屬性

## 3.1 基本型別

```cpp
namespace xt {
using NodeId     = uint32_t;  constexpr NodeId     kInvalidNode = 0xFFFFFFFFu;
using PropId     = uint16_t;
using StringId   = uint32_t;  constexpr StringId   kInvalidString = 0xFFFFFFFFu;
using AssetId    = uint32_t;  constexpr AssetId    kInvalidAsset = 0xFFFFFFFFu;
using SceneId    = uint32_t;  constexpr SceneId    kInvalidScene = 0xFFFFFFFFu;
using TimelineId = uint32_t;
using FontId     = uint32_t;  constexpr FontId     kInvalidFont = 0xFFFFFFFFu;
constexpr NodeId kSelfNode = 0xFFFFFFFEu;   // Track.target 的特殊值，見 04.4

struct Vec2  { float x, y; };
struct Color { float r, g, b, a; };          // 非預乘，0..1
struct Rect  { float x, y, w, h; };
struct Mat2x3 { float a, b, c, d, e, f; };   // [a c e; b d f]，列向量右乘：x' = a*x + c*y + e
}
```

`Mat2x3` 乘法順序：`world = parent * local`，先套 local 再套 parent。

## 3.2 節點種類

```cpp
enum class NodeKind : uint8_t {
    Free    = 0,  // 墓碑
    Group   = 1,  // 純容器，有變換與不透明度
    Path    = 2,  // 向量形狀：路徑 + 填充 + 描邊
    Image   = 3,  // 點陣圖，繪製於 (0,0,w,h)
    Text    = 4,  // 純文字節點（Element 的子節點，內容為一段字串）
    Element = 5,  // UI 元素：有佈局盒、計算樣式、可被選擇器命中
    Mask    = 6,  // 遮罩節點：其子樹為遮罩內容，作用於其後續兄弟（見 3.7）
};
```

只有 `Element` 參與佈局與樣式層疊。`Group/Path/Image/Mask` 是「場景節點」，來自 Scene 模板實例化，位置完全由變換屬性決定。`Text` 只能是 `Element` 的子節點。

Element 的視覺（背景、邊框）不透過子 Path 節點表現，而是 Painter 依 `ElementData` 直接產生命令（見 09）。

## 3.3 NodeCommon（所有節點共有）

```cpp
struct NodeCommon {
    // 可動畫
    float opacity;        // 0..1，乘到子樹
    Vec2  anchor;         // 變換原點（節點局部座標）
    Vec2  translate;
    Vec2  scale;          // 預設 (1,1)
    float rotation;       // 度，順時針
    Vec2  skew;           // 度：x = 傾斜角（Lottie sk），y = 傾斜軸角（Lottie sa）；見 11.3.1
    uint8_t visible;      // 0 = 不繪製也不命中（可由 PropId 13 動畫）
    // 不可動畫
    uint8_t blend;        // BlendMode
    uint8_t maskMode;     // 僅 Mask 節點使用
    uint8_t _pad;
    // 快取
    Mat2x3  local;        // 由上面欄位算出
    Mat2x3  world;
    float   worldOpacity;
};
```

局部變換計算（固定順序，與 Lottie 一致）：

```
local = T(translate) * R(rotation) * Sk(skew) * S(scale) * T(-anchor)
Sk(skew) = R(-skew.y) * ShearX(tan(skew.x)) * R(skew.y)
```

對 Element 節點，`translate` 由佈局盒位置加上 CSS `transform` 的 translate 合成，見 06.7。

```cpp
enum class BlendMode : uint8_t {
    Normal=0, Multiply, Screen, Overlay, Darken, Lighten,
    ColorDodge, ColorBurn, HardLight, SoftLight, Difference, Exclusion,
    Hue, Saturation, Color, Luminosity, Add
};
```

## 3.4 PathNodeData

```cpp
enum class PathVerb : uint8_t { Move=0, Line=1, Cubic=2, Close=3 };
enum class FillRule : uint8_t { NonZero=0, EvenOdd=1 };
enum class LineCap  : uint8_t { Butt=0, Round=1, Square=2 };
enum class LineJoin : uint8_t { Miter=0, Round=1, Bevel=2 };
enum class PaintKind: uint8_t { None=0, Solid=1, Linear=2, Radial=3 };

struct GradientStop { float offset; Color color; };   // offset 0..1，依序遞增

struct Paint {
    PaintKind kind;
    Color     color;               // Solid
    Vec2      p0, p1;              // Linear: 起點/終點；Radial: 中心/邊緣點（半徑 = |p1-p0|）
    float     highlightLength;     // Radial 的 Lottie "h"，0..1，第一版讀入但後端忽略
    float     highlightAngle;
    uint16_t  stopOffset, stopCount;   // 指向 PathNodeData::stops
};

struct Stroke {
    float    width;
    LineCap  cap;
    LineJoin join;
    float    miterLimit;           // 預設 4
    uint16_t dashOffset, dashCount;   // 指向 PathNodeData::dashes；count 0 = 實線
    float    dashPhase;
};

struct PathNodeData {
    std::vector<PathVerb> verbs;
    std::vector<float>    points;     // 每個 Move/Line 2 個 float，Cubic 6 個，Close 0 個
    FillRule fillRule;
    Paint    fill;
    Paint    stroke;                  // stroke.kind == None 表示不描邊
    Stroke   strokeStyle;
    float    trimStart, trimEnd, trimOffset;   // 0..1，0..1，任意；見 3.8
    std::vector<GradientStop> stops;  // fill 與 stroke 共用池
    std::vector<float> dashes;
};
// 註：trim 不在節點上快取。Painter 為 const 遍歷（1.5：render 不改狀態），每幀在 Painter 自身的暫存 vector 中以 trimPath 重算；
// trim 為 (0,1,0) 或區間 >= 1 時直接引用原路徑。
```

`points` 一律是節點局部座標；Painter 不在 CPU 端套變換，變換隨命令送到後端（見 09）。

## 3.5 ImageNodeData

```cpp
struct ImageNodeData {
    AssetId asset;
    float   width, height;   // 繪製尺寸；0 表示使用資產原始尺寸
};
```

## 3.6 TextNodeData 與 ElementData

```cpp
struct TextNodeData {
    StringId text;                       // 原始字串（IR 字串表）
    // 佈局輸出：由 LayoutEngine 填入
    std::vector<GlyphRun> runs;          // 見 07
};

struct ElementData {
    StringId tag;
    StringId id;                          // kInvalidString 表示無
    std::vector<StringId> classes;        // 已排序，便於二分搜尋
    uint16_t stateBits;                   // ElementState 位元
    ComputedStyle style;                  // 見 05，含所有可動畫與不可動畫的樣式值
    LayoutBox box;                        // 見 06
    Vec2      cssTranslate;               // PropId 3/4/11 對 Element 的寫入目標（06.7）
    Vec2      cssAnchor;                  // PropId 1/2/10 對 Element 的寫入目標，單位 px 或 %（依 style）
    Length    originX, originY;           // transform-origin 原始值
    SceneId   scene;                      // 若 tag == "anim"：實例化的場景，否則 kInvalidScene
    NodeId    sceneRoot;                  // 實例化後場景子樹的根（Group 節點，為此 Element 的子節點）
    uint32_t  listenerHead;               // 事件監聽器鏈表頭，見 08
    StringId  inlineStyle;                // 原始 style 屬性字串（僅供偵錯）
    std::vector<std::pair<StringId, StringId>> attributes;  // 其他屬性 name→value
};

enum ElementState : uint16_t {
    kStateHover    = 1 << 0,
    kStateActive   = 1 << 1,
    kStateFocus    = 1 << 2,
    kStateDisabled = 1 << 3,
    kStateChecked  = 1 << 4,
};
```

## 3.7 Mask 節點語意

`Mask` 節點的子樹是遮罩內容；遮罩作用於 **Mask 節點之後的所有兄弟節點**，直到下一個 Mask 節點或父節點結束。`maskMode`：

```cpp
enum class MaskMode : uint8_t {
    Alpha = 0,          // 遮罩內容的 alpha 決定可見度
    AlphaInverted = 1,
    Luma = 2,           // 遮罩內容的亮度決定可見度
    LumaInverted = 3,
    Add = 4,            // Lottie 形狀遮罩的 a 模式：多個 Mask 節點取聯集
    Subtract = 5,
    Intersect = 6,
};
```

Lottie 的 track matte 映射為 `Alpha/AlphaInverted/Luma/LumaInverted`；Lottie 圖層自帶的 `masksProperties` 映射為 `Add/Subtract/Intersect`（見 11）。Painter 輸出 `PushMask ... EndMaskContent ... PopMask`（見 09）。

## 3.8 Trim Path

`trimStart`、`trimEnd` 為 0..1 的弧長比例（超出者夾到範圍內；`end < start` 時交換兩值，與 lottie-web 一致），`trimOffset` 為任意實數（圈數）。有效區間為 `[start+offset, end+offset] mod 1`。若區間跨越 1，路徑分成兩段。多個子路徑（多個 Move）依 Lottie 預設「individually = false」處理：**視為一條連續路徑計算總弧長**。弧長計算：三次貝茲以 16 段線性近似。實作在 `core/path_trim.cpp`，輸入 verbs/points，輸出 trimmedVerbs/trimmedPoints。

## 3.9 屬性表（PropId）

每個 PropId 對應一個節點欄位。`型別` 見 3.10。`L` = 影響佈局，`T` = 影響變換，`P` = 只影響繪製。`適用` 指哪些 NodeKind 擁有此屬性。

### 通用（0–15）

| PropId | 名稱 | 型別 | 髒 | 適用 |
|---|---|---|---|---|
| 0 | Opacity | Float | P | 全部 |
| 1 | AnchorX | Float | T | 全部 |
| 2 | AnchorY | Float | T | 全部 |
| 3 | TranslateX | Float | T | 全部 |
| 4 | TranslateY | Float | T | 全部 |
| 5 | ScaleX | Float | T | 全部 |
| 6 | ScaleY | Float | T | 全部 |
| 7 | Rotation | Float | T | 全部 |
| 8 | SkewX | Float | T | 全部（傾斜角） |
| 9 | SkewY | Float | T | 全部（傾斜軸角） |
| 10 | Anchor | Vec2 | T | 全部（同時寫 1,2） |
| 11 | Translate | Vec2 | T | 全部（同時寫 3,4） |
| 12 | Scale | Vec2 | T | 全部（同時寫 5,6） |
| 13 | Visible | Enum(0/1) | T | 全部；寫入 `NodeCommon::visible`，供 Lottie ip/op 動畫 |

Vec2 版本存在是為了 Lottie 空間貝茲插值（位置在 2D 上走曲線）。同一節點同一幀若 Vec2 與分量版本都被寫入，**後寫者勝**（依 04 的層疊順序）。

### Path 節點（32–63）

| PropId | 名稱 | 型別 | 髒 |
|---|---|---|---|
| 32 | PathData | Path | P |
| 33 | FillColor | Color | P |
| 34 | FillOpacity | Float | P |
| 35 | StrokeColor | Color | P |
| 36 | StrokeOpacity | Float | P |
| 37 | StrokeWidth | Float | P |
| 38 | TrimStart | Float | P |
| 39 | TrimEnd | Float | P |
| 40 | TrimOffset | Float | P |
| 41 | FillGradientP0 | Vec2 | P |
| 42 | FillGradientP1 | Vec2 | P |
| 43 | FillGradientStops | Stops | P |
| 44 | StrokeGradientP0 | Vec2 | P |
| 45 | StrokeGradientP1 | Vec2 | P |
| 46 | StrokeGradientStops | Stops | P |
| 47 | DashOffset | Float | P |
| 48 | DashArray | FloatArray | P |
| 49 | MiterLimit | Float | P |

`FillOpacity` 乘進 `fill.color.a`，Painter 送出前計算 `effectiveAlpha = color.a * fillOpacity`。

### Element 節點（64–159）

| PropId | 名稱 | 型別 | 髒 |
|---|---|---|---|
| 64 | Width | Length | L |
| 65 | Height | Length | L |
| 66 | MinWidth | Length | L |
| 67 | MaxWidth | Length | L |
| 68 | MinHeight | Length | L |
| 69 | MaxHeight | Length | L |
| 70 | MarginTop | Length | L |
| 71 | MarginRight | Length | L |
| 72 | MarginBottom | Length | L |
| 73 | MarginLeft | Length | L |
| 74 | PaddingTop | Length | L |
| 75 | PaddingRight | Length | L |
| 76 | PaddingBottom | Length | L |
| 77 | PaddingLeft | Length | L |
| 78 | BorderTopWidth | Length | L |
| 79 | BorderRightWidth | Length | L |
| 80 | BorderBottomWidth | Length | L |
| 81 | BorderLeftWidth | Length | L |
| 82 | Left | Length | L |
| 83 | Top | Length | L |
| 84 | Right | Length | L |
| 85 | Bottom | Length | L |
| 86 | FlexGrow | Float | L |
| 87 | FlexShrink | Float | L |
| 88 | FlexBasis | Length | L |
| 89 | RowGap | Length | L |
| 90 | ColumnGap | Length | L |
| 91 | FontSize | Length | L |
| 92 | LineHeight | Length | L |
| 93 | LetterSpacing | Length | L |
| 96 | BackgroundColor | Color | P |
| 97 | BorderColor | Color | P |
| 98 | BorderRadiusTL | Length | P |
| 99 | BorderRadiusTR | Length | P |
| 100 | BorderRadiusBR | Length | P |
| 101 | BorderRadiusBL | Length | P |
| 102 | Color | Color | P |
| 103 | ImageOpacity | Float | P |
| 104 | ScrollX | Float | P |
| 105 | ScrollY | Float | P |

Element 的 CSS `transform`（translate/rotate/scale）與 `opacity` 直接對應通用 PropId 0–12；`transform-origin` 對應 Anchor。

不可動畫的樣式屬性（`display`、`position`、`flex-direction` 等）不在 PropId 表，只存在 `ComputedStyle`，改變時走 `kDirtyStyle`。

## 3.9b 頂層元素（T-158 定案）

`<body>` 下可以有多個頂層元素，語意同瀏覽器：依序排版、全部參與樣式重算、動畫、繪製、命中與選擇器查詢（`Document::root()` 是 `<xt>`，`<body>` 為其子節點，所有走訪從 root 開始）。編譯器不警告；DC 匯入器對「多頂層元素」發 lint warning 只是提醒畫板結構可能不是作者本意（T-157）。

## 3.10 值型別與插值

```cpp
enum class ValueType : uint8_t {
    Float=0, Vec2=1, Color=2, Length=3, Path=4, Stops=5, FloatArray=6, Enum=7
};

enum class LengthUnit : uint8_t { Px=0, Percent=1, Em=2, Auto=3 };
struct Length { float value; LengthUnit unit; };
```

插值規則 `lerp(a, b, t)`，`t` 已經過緩動：

| 型別 | 規則 |
|---|---|
| Float | `a + (b-a)*t` |
| Vec2 | 逐分量；若關鍵幀帶空間切線，走三次貝茲（見 04.3） |
| Color | 逐分量線性，於非預乘 RGBA 空間 |
| Length | 單位相同且非 Auto：線性；否則離散（`t < 0.5 ? a : b`） |
| Path | 要求 verbs 完全相同（編譯器保證）；points 逐一線性；若不同則離散 |
| Stops | 要求 stop 數相同；offset 與 color 逐一線性；否則離散 |
| FloatArray | 長度相同逐一線性；否則離散 |
| Enum | 離散 |

`Value` 執行時表示：

```cpp
struct Value {
    ValueType type;
    union { float f; Vec2 v2; Color c; Length len; uint32_t e; };
    // Path / Stops / FloatArray 用外部儲存：
    uint32_t dataOffset, dataCount;   // 指向 Timeline 的 floatPool / stopPool
};
```

## 3.11 節點屬性讀寫介面（核心內部）

```cpp
// core/property_access.h
Value  getProperty(const NodeArena&, NodeId, PropId);
void   setProperty(NodeArena&, NodeId, PropId, const Value&);   // 設定對應髒旗標
bool   propertyApplies(NodeKind, PropId);
ValueType propertyType(PropId);
uint8_t propertyDirtyBits(PropId);
```

`setProperty` 是 Animator、StyleEngine、公開 API 的唯一寫入路徑。實作為一個對 PropId 的 `switch`，不用查表函式指標。

## 3.12 編輯器藍圖新增（2026-09-27 定案；對應 26～30 章）

實作前以本節為準；各任務單引用小節編號。

### 3.12.1 節點種類

- 新增 `NodeKind::Particles`（3.12.5）、`NodeKind::External`（T-191 實作：如 Camera 的子節點；`ExternalNodeData`、`ElementData::externalNode/imageSubUv`；`ExternalSourceKind`、`MediaState` 於 `node.h`；未綁定時固有尺寸 320×180 佔位）、行內物件（T-178 實作）：**不是** arena 節點，住在 `TextNodeData::inlines`（`TextInline{kind, action, asset, height, box}`），`TextStyleRun::inlineNode` = 1 + 索引；獨立 `<glyph>` 是一個元素、其文字為單一行內字形；貼圖由 `IGlyphProvider` 依輸入裝置與 `action` 解析（`AssetRef{name}` 須為 IR `ASST` 內的圖片名，否則顯示 `[key]`）。
- 符號（`<symbol>`／`<use>`）與 `<repeat>` **在 IR 不存在**：符號於編譯時展開（12.7），`<repeat>` 編譯成「模板子樹 + RepeatController」（3.12.4）。

### 3.12.2 NodeCommon 尾端新增欄位

| 欄位 | 型別 | 說明 |
|---|---|---|
| `triggerTable` | u32 | 觸發表索引（0 = 無）；表格式見 3.12.3 |
| `bindingTable` | u32 | 綁定表索引 |
| `customStates` | u32 bitset + 名稱表索引 | `:state(Name)`，每文件最多 32 個自訂狀態名（超過編譯錯誤）；`stateGroups` 索引描述互斥群組 |
| `flags2` | u32 | `kPointerWatch`（08.9）、`kLayerOverlay`（全域彈出層，繪製於根之後、命中優先）、`kDraggable`、`kDropTarget`、`kFocusTrap`（modal） |
| `worldAnchorPort` | u16 | 0 = 無；否則該 port 的 vec2/vec3 值驅動位置（螢幕座標由宿主換算，13.8） |
| `navUp/Down/Left/Right` | u32 ×4 | 顯式焦點導覽目標（0 = 自動） |
| （`symbolPath` 取消，T-171：路徑 id 直接由 `id` 屬性承載，對照表在 `SYMB` 區塊） | | |

### 3.12.3 新表（IR 區塊，12.7）

- **TriggerTable**：`{ event: u8, eventArg: StringId, condition: ConverterChain, actions: Action[] }`；`Action { kind: u8, arg0: u32, arg1: u32, arg2: f32, then: u16 }`（kind 對應 27.2.3 的動作，`then` 指向序列中的下一個動作）。
- **BindingTable**：`{ targetNode: u32, targetProp: u16 (PropId 或 StyleProp 或 kText/kSrc/kVisible/kState), sources: PortRef[], chain: ConverterChain }`；`ConverterChain` = `{ op: u8, args: f32/StringId ×4 }[]`，op 取自 27.3.3 目錄（固定編號，13.8 允許宿主註冊 0x80 起的自訂 op）。
- **PortTable**：`{ name: StringId, type: u8, dir: u8, node: u32, expose: bool, structFields: FieldDesc[] }`。
- **SpawnPool**：`{ templateSubtree: NodeRange, maxInstances: u16, lifetimeDefault: f32 }`；`spawn` 動作從池取實例掛到指定父節點，壽命到期回收；實例節點不寫回文件。

### 3.12.4 RepeatController

`{ templateSubtree, listPort, itemEnterTimeline, itemExitTimeline, virtualize: bool, viewport: NodeId }`。清單變化（13.8 的 listInsert/Remove/Update）→ 產生／回收實例（路徑 id `Item/<index>/…`），實例的綁定以 `item(field)` 取當前項；虛擬化時只實例化捲動視窗內 + 前後各一頁。

### 3.12.5 Particles

`ParticleNodeData { emitterShape (point|rect|circle|edge), rate, burst, lifetime[min,max], speed[min,max], direction, spread, gravity, drag, size[start,end], color[start,end] (或 4 stop 曲線), rotation[min,max], texture: AssetRef, blend: normal|additive, maxParticles (預設 256), simulateInEditor: bool }`。GPU 路徑實例化繪製（09 章新命令 `DrawParticles`）；SW 路徑上限 256 且無混合以外的效果；全部參數皆 PropId（可動畫、可綁定）。

### 3.12.6 屬性與插值

- 新 PropId：`TextReveal` = 109（`kElementPropCount` = 110；Float 0..1 初始 1、不繼承、只影響繪製，顯示前 `floor(r·簇數)` 個字素簇）、`ClipStart`、`ClipRate`（片段）、`PathTrimStart/End/Offset`（既有 Trim 對 `path-trim` 語法）、`MixBlend`（Enum，不可動畫）、`AspectRatio`、粒子參數群（3.12.5）、`TranslateX/Y/Z`／`Rotate`／`ScaleX/Y` 已存在——`translate/rotate/scale` 獨立屬性直接映射到它們。
- 新單位（T-176 實作 `LengthUnit`）：Vw=4、Vh=5、Vmin=6、Vmax=7、SafeTop=8、SafeRight=9、SafeBottom=10、SafeLeft=11、UiScale=12、Calc=13（`value` = 程式索引；`kRuntimeCalcBase` 1<<22 起為執行時池）；`lerpValue` 不插值 Calc。
- 插值：`color-interpolation: oklab | srgb`（文件級，預設 `oklab`；`.xtir` 標頭旗標）；`Path` 型別軌道（3.4 的 PathNodeData 頂點陣列）頂點數與閉合相同才插值，否則跳變；漸層 stop 數相同才插值。
- 自訂狀態參與層疊：`:state(Name)` 的特異性同偽類（05.7）。

- T-190：Path 值可帶自身 verbs（`Value::ofPathWithVerbs`、`pathTopologyEquals`），點數與 verbs 相同才插值、否則於 0.5 跳變並 `setPathProperty` 換拓撲；關鍵幀槽 2/3 存 verbs（舊 IR 為零）。新向量屬性（僅時間軸內）：`d: path("…")`、`fill`、`fill-opacity`、`stroke`、`stroke-opacity`、`stroke-width`、`stroke-dashoffset`、`path-trim` 與 `path-trim-start/end/offset`。

### 3.12.7 實作記錄（T-200 #198）

`worldAnchorPort`（3.12.2）**未做成 NodeCommon 欄位**，改以 BIND 記錄 `BindTarget::WorldAnchor`（02 §2.s.1）表達；執行期用 run-time translate 覆蓋位置（不重排），vec3 存 depth，`onScreen=false` 開自訂狀態 `Offscreen`（文件有宣告才開）。

### 3.12.8 實作記錄（T-201 #199）：SpawnPool、Set、State target

- **spawn**（TriggerAction 13，`spawn(<symbol>, at=<#id|pointer|self>, lifetime=<t>[, timeline=<n>])`）：在符號展開**之前**降階為 `<repeat spawn>` 子節點（RPT 旗標 `kRepeatSpawn`、無 list port），符號內的 spawn 每個實例各自一池；池做在 `repeat_controller.cpp`（沒有獨立 `spawn_pool.cpp`）。每池最多同時 32 個，超過回收最舊；生出的實例綁定只求值一次即凍結、強制 `pointer-events: none`；`at` 指實例左上角，`#id` 在符號範圍內解析，`pointer` 用 EventDispatcher 保存的最後指標位置。
- **set**（TriggerAction 9）：`IrTriggerAction` 原 padding 改名 `extra`（Set 放轉換器步數）；拒絕 tween/delay 形式；對未宣告、call、list、struct port，以及非 bool port 用 `toggle`，皆為編譯錯誤。
- **state target**：action flags 設 `kStateTarget`、路徑 id 放 arg1，IR 結構不變；實例外的 `#id` 解析為文件 id。
- **TriggerEvent** 尾端：`DragValue = 29`、`NavValue = 30`。

### 3.12.9 實作記錄（T-202 #202）：粒子

- `NodeKind::Particles = 9`，`ElementData::particlesNode`；`ParticleNodeData` 實際欄位：color 固定 4 stop（t = 0、1/3、2/3、1；兩色寫法編成直線上四點）、`textureSubUv`、執行期狀態（粒子陣列、累積器、發射序號）。
- **PropId 110～128**：`ParticleRate=110`、`ParticleBurst=111`、`ParticleLifetimeMin/Max=112/113`、`ParticleSpeedMin/Max=114/115`、`ParticleDirection=116`、`ParticleSpread=117`、`ParticleGravity=118`、`ParticleDrag=119`、`ParticleSizeStart/End=120/121`、`ParticleColor0..3=122..125`、`ParticleRotationMin/Max=126/127`、`ParticleMax=128`；`kElementPropCount = 129`。
- 模擬（`Code/Runtime/core/src/particles.cpp`）：固定步長 1/120 s，每次 update 最多補算 1 s；發射率小數累積、burst、壽命回收、semi-implicit Euler 的 gravity/drag、size 與 4 stop 顏色插值、`max` 上限；xorshift 決定性亂數，種子 = 元素 id FNV-1a + 發射序號（`<repeat>`／spawn 實例另加索引）；在版面之後推進、不影響版面；發射器盒 = border box；`rotVel` 固定 0（無屬性）。
- 預設值：rate 10/s、lifetime 1、speed 100、direction −90、size 8、白色淡出、max 256、其餘 0。`visibility: hidden` 不畫；3D 投影內 SW／GPU 都不畫、simplifier 丟棄。

### 3.12.10 實作記錄（T-218 #219）：場景覆寫與公開貝茲工具

- `StyleEngine::setSceneInstances()` 由 `document_load` 綁定；含場景目標的規則宣告移出元素層疊，`decodeSceneValue` 把 var／calc／em 解成節點 Value；`SceneOverrideSet = vector<SceneOverrideDecl{targetKind, target, prop, value, specificity, order, important}>`；`SceneOverrideState{sceneRoot, scene, base[]{node, prop, value, verbs}, applied[]{node, prop, local, value}}` 存於 `unordered_map<NodeId,…>`——每 (node, prop) 第一次覆寫時自 IR 取基準值，不再命中時寫回，全還原後刪除；實例根變更直接丟棄；寫入點為樣式重算 step 5c。名稱以 StringId 比對；`*` 不含場景根。
- `Code/Runtime/core/include/xt/geom.h` 新 `namespace xt::geom`（`Code/Runtime/core/src/geom_bezier.cpp`）：`splitCubic`、`subCubic`、`CubicTable{kSteps=16, pts, cum, length}`、`buildCubicTable`、`lengthFractionToT`（自 `path_trim.cpp` 搬出，行為不變）、`pathBounds`（導數求根極值）、`pathContains`（0.25 px 攤平 winding）。

### 3.12.11 實作記錄（T-225a #229、T-225b #233、T-228 #234）：修改器、參數形狀、auto-orient

`NodeKind::Modifier=10`（`ModifierNodeData{kind, mode, enabled, params[8], opacityA, opacityB}`）、`NodeKind::Shape=11`（`ShapeNodeData{kind, params[12], paint 欄位同 Path}`）；PropId 修改器 208–231、形狀 232–245（對應表 `Code/Runtime/core/src/modifier_props.h`；`styleprop::kLast < 208` 靜態斷言）、通用 14 `AutoOrient`；params 布局與演算法見 31 §31.6.1 與 §31.11a。Painter 對含修改器的群組走 `emitModifiedGroup`（收集直接子 Path／Shape → 依序套修改器 → `emitPathCommands`），repeat 為最後有效修改器時以命令複製；`FrameStats::modifierVertices`。

> T-233a（#244）：`Stroke` 加 `taperStart/taperEnd`（0–1 弧長比例）與寬度剖面（`WidthPoint{t, w}`，`PathNodeData::widthProfile`）；PropId 50／51；有 taper／剖面的描邊由 Painter 以 `geom::strokeToFill(WidthFn)` 轉填色命令；閉合輪廓忽略 taper；**不與虛線疊加**（有剖面時 dash 忽略）。IR minor 4。

> T-243（#251，32 章）：`NodeKind::Bone = 12`（`BoneNodeData{length}`）、`Constraint = 13`（`ConstraintNodeData{kind, space, enabled, target, params[8], mix}`）、`Mesh = 14`（`MeshNodeData{asset, positions, uvs, indices, opacity, smooth, skin}`）；`PathNodeData::skin`（`SkinData{skeleton, tendons, weights}`，每頂點 p／in／out 三筆）。PropId 246–255：`BoneLength`、`IkStrength`、`ConstraintMix`、`ConstraintParam0…5`、`ConstraintEnabled`（皆 Float，髒位 transform）。Bone 接受 transform 區 + 246 + Visible；Constraint 只 247–255；Mesh 接受 Opacity、Visible、transform。求值與繪製見 T-244／T-245。

## 3.13 執行期數值上限（2026-10-02 審查修正；T-258a #269）

來自檔案、綁定、宿主呼叫的數字在進入迴圈次數、陣列大小、float→int 轉換前一律經過 `Code/Runtime/core/include/xt/num_guard.h`（`finiteOr`、`clampFinite`、`toInt32Sat`、`toUint32Sat`、`toUint16Sat`、`toInt64Sat`；規則見 14 §14.9）。合法內容的結果不變（golden 逐 byte 相同）。

| 項目 | 常數 | 超限時 |
|---|---|---|
| 粒子發射率 | `kParticleMaxRate` 100000 | 夾限；每步最多發射剩餘空位數 |
| star／zigzag／wiggle 產生的點數 | `kMaxGeneratedPoints` 1024 | 夾限；非有限＝修改器關閉 |
| repeat 份數 | `kMaxRepeatCopies` 1024 | 夾限 |
| twist 角度 | `kMaxTwistDegrees` ±36000° | 夾限 |
| border-image repeat | `kMinBorderImageTile` 1 px、`kMaxBorderImageTiles` 每軸 1024、`kMaxBorderImageRegionTiles` 每區 65536 | 該區改為 stretch |
| 虛線 | `kMaxDashCycles` 每個 contour 100000 週期 | 該 contour 畫成實線（非有限亦同） |
| 圖片解碼 | `kMaxImageDimension` 16384、`kMaxImagePixels` 67108864 | 解碼失敗（先讀標頭，不配置）；`xtframe-pack` 收資產時發 warning，見 22 §22.13 |
| UI 尺寸 | `kMaxUiSize` 16384（每邊 1..16384、須有限） | 載入錯誤 |
| 縮圖 | `kMaxThumbnailSize` 1024 | 夾限 |
| `iterations` | 65535 | 飽和；執行期 0 是無限的標記，65535 是普通次數 |

- 綁定寫入 Entity／enum 屬性時，負值或非有限值被拒絕（不寫入）。
- SW 光柵器丟棄非有限線段，欄切分夾到目標範圍內；對合法輸入逐 byte 相同（有等價測試）。

## 3.14 補記：迭代通知上限（2026-10-07 審查修正鏈合併；fuzz 修正 2386c026）

每個 animation instance 每次 `Document::update` 最多發 `kMaxIterationNotesPerUpdate` = 64 則 iteration 通知，其餘合併成一則（`iteration` = 最後到達的索引、`Note::skipped` = 略過數）；加進 §3.13 的上限表。零長度 timeline 仍為合法輸出，不在載入時拒絕。

## 3.13 只在編輯器顯示的元素（`editor-only`；2026-10-15 定案，T-342）

使用者需求：HUD 這類無背景 UI 在編輯時需要一張遊戲截圖或參考圖墊在底下看效果，但遊戲裡不能有它。

- **語法**：任何元素可加布林屬性 `editor-only`（`.xt`：`<img src="ref/screenshot.png" editor-only>`；可裸寫）。繼承到整個子樹（子項不必再寫）。
- **語意**：編譯器照常編進 IR（編輯器畫布與預覽要看得到）；**發佈時移除**（22 §22.3：`xtframe-pack` 在編譯前把帶 `editor-only` 的子樹從文件移除，IR 與資產表都不含它，素材若只被它引用則不打包）；編輯器的**展示空間**同樣不顯示（17 §17.6：展示空間的預覽以「發佈視角」編譯，`CompileOptions.stripEditorOnly = true`）。執行時不認得這個屬性（永遠看不到），零成本。
- **編輯器**：圖層面板該圖層前有「只在編輯器顯示」圖示（灰色眼睛加 E），屬性面板「位置與尺寸」段「+」可加「只在編輯器顯示」核取列；畫布以淡化邊框提示（不改內容）。新圖層預設關。
- **匯入**：Claude Design／HTML 畫板元素帶 `data-xt-editor-only`（24 章標籤字典）→ 匯入成 `editor-only`；Figma 無對應。
- **lint**：發佈時若 `editor-only` 子樹內有被外部引用的 id（流圖、綁定、`<use>` 目標）→ 警告「發佈後找不到」。
