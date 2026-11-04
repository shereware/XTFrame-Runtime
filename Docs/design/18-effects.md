# 18 · 效果與 shader

本章定義「效果」：套用在一個節點（Element 或場景節點）已渲染內容上的影像處理，例如陰影、模糊、掃光。原則：**使用者操作的是效果與參數，shader 只是效果的一種來源。** 三個層次：

| 層次 | 內容 | 執行位置 | 版本 |
|---|---|---|---|
| 內建效果 | 固定的七種影像處理，參數可動畫 | SW、GL、（引擎端可選） | 0.2 |
| 效果模板 | 純資料：內建效果組合 + 參數預設 + 參數動畫 | 編輯器層，展開成內建效果 | 0.2（17 章） |
| 自訂效果 | HLSL 片段函式 + 參數宣告的資產（`.xtfx`） | GL（翻譯子集）、引擎端；SW 依宣告退化 | 0.3。格式於本章定案；0.2 只做 IR/命令清單的預留欄位與「載入時拒絕」 |

相依：19 章（`NodeCommon` 欄位順序、`SimplifyStats`、映射堆疊）。對既有章節的修訂條款集中在 18.10。

## 18.1 語意

- 效果附著在節點上，形成有序的**效果堆疊**。堆疊依序處理：第 1 個效果的輸入是節點內容的渲染結果，第 k 個效果的輸入是第 k−1 個的輸出。
- 效果在**離屏圖層的螢幕空間**執行（同 09 的 Layer）。`EffectInstance` 內的長度、位移、角度、座標一律以**節點平面的局部 px** 宣告；`EffectCmd` 帶該節點「平面 → 螢幕」的 2×3 矩陣 `xf`（3D 投影下取錨點處的雅可比，19.6），後端依 18.5 的規則把局部量換算到螢幕。
- 效果不影響佈局與命中測試（與 CSS `filter` 一致）。
- 效果與節點 opacity/blend/clip 的順序（Painter，修訂 09.3）：

```
paintNode(n) 有效果時：
    若 opacity[n] < 1（或場景節點的 blend != Normal）：PushLayer(opacity, blend)   // Element 也走 Layer；此時 Element 的 opacity 不再折進 paint alpha
    PushEffect(effects, xf, boundsHint)
        Element：背景/邊框/圖片 → [overflow clip：PushClipRect] 子節點 [PopClip]   // 裁切在效果內：陰影不被 overflow 裁掉
        場景節點：既有流程（Path/Image/Group/Mask）
    PopEffect
    若有 Layer：PopLayer
```

- 效果擴大內容的包圍盒；`EffectCmd.boundsHint` = 內容的螢幕座標 AABB 向外擴 `expand`，`expand` 為堆疊中各效果「擴張量」（18.3，換算到螢幕 px 後）的總和，最後夾到視口。內容 AABB 由 Painter 的 `subtreeBounds(NodeId)` 計算：Element → border-box 與其子孫的聯集；Path → 控制點包圍盒（描邊再外擴 `strokeWidth/2 · miterLimit`）；Image → `dst`；Text → 行盒；Group/Mask → 子孫聯集；全部經各自的平面相對變換與外層映射投影到螢幕。`boundsHint` 不允許「全視口」的偷懶值（`LayerCmd` 允許，`EffectCmd` 不允許）；後端的效果緩衝大小 = `boundsHint`。

## 18.2 資料模型（修訂 03）

```cpp
enum class EffectKind : uint8_t {
    None = 0, DropShadow = 1, Glow = 2, Blur = 3, ColorAdjust = 4,
    GradientMask = 5, Shine = 6, Outline = 7, Custom = 128,
};
struct EffectInstance {            // 144 bytes（4 + 4 + 64 + 64 + 8），IR 的 IrEffect 與此同佈局；2026-10-30（T-534，IR 3.0）自 80 bytes（params[8]、colors[2]）擴充
    EffectKind kind;
    uint8_t    enabled;            // paramIndex 0；0 = 略過
    uint8_t    _pad[2];
    AssetId    shader;             // Custom：Shader 資產；否則 kInvalidAsset
    float      params[16];         // paramIndex 1..16，依 kind 的參數表（18.3）；內建效果只用前 8 格
    Color      colors[4];          // paramIndex 32..35；內建效果只用前 2 格
    uint32_t   stopOffset, stopCount;   // GradientMask：漸層停靠點；指向所屬池（NodeArena::effectStops / CommandList::stops / IR 區塊的 stops）
};
```

> 2026-10-30 定案（使用者：「給 shader 預留 slot」「還沒 beta，現在改」）：槽位從 8 float + 2 color 擴成 **16 float + 4 color**，給自訂 shader 用（四個 vec2 + 八個純量 + 四色夠放，再多拆成兩個效果）。`IrEffect`、`PackedEffectInstance`（09 的 `.xtcl`）、`gpu::CustomEffectParams`、CE 的 `SXuiEffect` 同步加大；IR major 2 → 3（02 §2.2；43 §43.1 的 beta 承諾尚未啟動）。

節點資料：`NodeCommon` 在 19.3 的 3D 欄位之後新增 `uint32_t effectOffset, effectCount;`，指向 `NodeArena::effects`（`std::vector<EffectInstance>`）；停靠點在 `NodeArena::effectStops`（`std::vector<GradientStop>`）。上限：每節點 16 個效果（編譯錯誤）、每個 GradientMask 16 個停靠點。

**池的政策**：`effects/effectStops` 只增不減。節點的效果序列變更時（StyleEngine 重解析、`setEffects`）：新序列的 `kind` 逐一與舊序列相同且數量相同 → 原地覆寫參數；否則在池尾追加新區段並改寫 `effectOffset/effectCount`，舊區段成為空洞。`Document::compactEffects()`（編輯器或宿主於非每幀時機呼叫）重排池並更新所有節點的 offset。每幀路徑不配置：池在載入時 `reserve(總效果數 × 2)`，追加超過容量時配置一次並印一次警告（01.7 的允許例外）。

**效果參數的 PropId**：效果是動態清單，不能占固定 PropId。編碼：

```
PropId effectParam(effectIndex, paramIndex) = 0x8000 | (effectIndex << 8) | paramIndex
```

`effectIndex` 0–15、`paramIndex`（**2026-10-30 重編，T-534；舊編碼 1..8／16／17／18 隨 IR 2 一起作廢**）：

| paramIndex | 內容 | 型別 |
|---|---|---|
| 0 | enabled | Float 0/1，離散插值 |
| 1..16 | `params[0..15]` | Float |
| 32..35 | `colors[0..3]` | Color |
| 64..78 | 合成 Vec2 `(params[s], params[s+1])`，s = paramIndex − 64 | Vec2；DropShadow 位移 = 64（空間貝茲用）；自訂 shader 的 vec2 參數整體定址（18.13） |

常數：`prop::kEffectParamEnabled = 0`、`kEffectParamFloatBase = 1`、`kEffectParamColorBase = 32`、`kEffectParamVec2Base = 64`（`kEffectParamColor0`、`kEffectParamOffset` 兩個舊名作廢）。`propertyApplies(kind, prop)` 對 `prop ≥ 0x8000`：節點 `effectCount > effectIndex` 且該效果的 kind 定義了該參數 → true。`getProperty/setProperty` 分派到效果池；Track、transition、override 對這類 PropId 一律照舊。`PropId` 型別維持 `uint16_t`。

## 18.3 內建效果

參數表：`params[i]` 的名稱即 `.xtfxp` 與編輯器使用的正式名稱；長度單位為節點局部 px；角度為度（0 = +x 方向，順時針為正，與 2D 旋轉一致）。半徑類參數換算到螢幕後上限 64 **裝置** px（含 dpr；dpr = 2 時邏輯上限 32 px，這是刻意的成本上限；超過夾住並警告一次；SW 與 GL 相同上限，黃金圖不得超過）。

| kind | 名稱 | params（索引：名稱，範圍，預設） | colors | 擴張量（局部 px） |
|---|---|---|---|---|
| 1 | DropShadow 陰影 | 0 offsetX、1 offsetY、2 blur ≥ 0、3 spread ≥ 0、4 opacity 0..1（預設 1）、5 inset 0/1（預設 0；T-111） | 0 color（預設 #000） | outer：`max(|offsetX|,|offsetY|) + spread + 3σ`，σ = blur/2；inset：0 |
| 2 | Glow 發光 | 0 radius ≥ 0、1 intensity 0..4（預設 1）、2 inner 0/1（預設 0） | 0 color（預設 #fff） | outer：`3σ`（σ = radius/2）；inner：0 |
| 3 | Blur 模糊 | 0 radiusX、1 radiusY（≥ 0；`blur(<len>)` 單值時兩者相同） | — | `3·max(σx, σy)` |
| 4 | ColorAdjust 色彩 | 0 brightness −1..1、1 contrast −1..1、2 saturation −1..1、3 hue 度、4 opacity 0..1（預設 1） | — | 0 |
| 5 | GradientMask 漸層遮罩 | 0 p0x、1 p0y、2 p1x、3 p1y、4 radial 0/1、5 invert 0/1 | — | 0 |
| 6 | Shine 掃光 | 0 position 0..1（預設 0；可由 `shine()` 第六引數或動畫設定，見 18.4）、1 width > 0、2 softness 0..1（預設 0.5）、3 angle 度、4 intensity 0..2（預設 1） | 0 color（預設 #fff） | 0 |
| 7 | Outline 描邊 | 0 radius ≥ 0、1 softness ≥ 0（px）、2 position 0 outer / 1 center / 2 inner | 0 color | outer/center：`radius + softness`；inner：0 |
| 8 | BackdropBlur 背景模糊（T-130，0.4） | 0 radius ≥ 0（σ = radius/2）、1 saturation −1..1（預設 0）、2 brightness −1..1（預設 0） | — | 0（讀取 backdrop 時向外多取 3σ） |

演算法（以 SW 為準，全部在**非預乘** RGBA float 上運算，與 `sw_layer.h` 的 `Surface` 一致；GL 的效果 pass 必須產生相同結果，FBO 內容若為預乘則在 pass 內轉換）。記號：`srcOver(bottom, top)` = 把 `top` 以 source-over 疊在 `bottom` 之上：

- **DropShadow**：`a = alpha(src)`；`a = dilate(a, spread)`；`a = gauss(a, σ)`；`shadow.rgb = color.rgb`、`shadow.a = a · color.a · opacity`；平移 `(offsetX, offsetY)`；`dst = srcOver(shadow, src)`（陰影在下、內容在上）。
- **DropShadow inset**（T-111）：`h = gauss(dilate(1 − alpha(src), spread), σ)`（spread 對反相 alpha 膨脹 = 對形狀收縮）；`h` 依 (offsetX, offsetY) 位移後 `· alpha(src)`；`shadow = (color.rgb, h · color.a · opacity)`；`dst = srcOver(src, shadow)`（陰影在上，只在形狀內；記法同 Glow inner）。反相場在圖層範圍外取 1（圖層外是透明的），否則貼齊緩衝邊的形狀會失去該邊的內陰影。與 Figma/CSS 的 inner shadow 語意一致。
- **Glow outer**：`g = gauss(alpha(src), σ)`；`g = clamp(g · intensity, 0, 1)`；`glow = (color.rgb, g · color.a)`；`dst = srcOver(glow, src)`。**Glow inner**：`h = gauss(1 − alpha(src), σ)`；`h = clamp(h · intensity, 0, 1) · alpha(src)`；`glow = (color.rgb, h · color.a)`；`dst = srcOver(src, glow)`（發光在上）。
- **Blur**：可分離高斯，σx = radiusX/2、σy = radiusY/2；SW 以三次 box 近似（18.6）；alpha 與 RGB 一起模糊（非預乘下先預乘 → 模糊 → 反預乘，避免邊緣變暗）。
- **ColorAdjust**：合成一個 4×5 矩陣 `M = Hue(hue) · Sat(saturation) · Contrast(contrast) · Brightness(brightness)`（先套 brightness）作用於 RGB，再 `a *= opacity`。定義：Brightness：`c + b`；Contrast：`(c − 0.5)·(1 + k) + 0.5`；Saturation：`lerp(luma, c, 1 + s)`，luma 權重 (0.2126, 0.7152, 0.0722)；Hue：YIQ 空間繞 I/Q 旋轉 `hue` 度。結果夾 0..1。
- **GradientMask**：對每個像素取其局部座標 `q = inverse(xf) · pixel`；linear：`t = dot(q − p0, p1 − p0) / |p1 − p0|²`；radial：`t = |q − p0| / |p1 − p0|`；`t` 夾 0..1；`m = stops(t).a`（只用停靠點的 alpha）；`invert` → `1 − m`；`dst.a = src.a · m`。
- **Shine**：方向 `d = (cos angle, sin angle)`（局部）；把內容包圍盒（局部）投影到 `d` 得 `[lo, hi]`；帶中心 `c = lerp(lo, hi, position)`；像素在 `d` 上的投影 `u`；`band = 1 − smoothstep(width·(1−softness)/2, width/2, |u − c|)`；`dst.rgb = clamp(src.rgb + color.rgb · band · intensity · src.a, 0, 1)`（Add），`dst.a = src.a`。
- **BackdropBlur**（T-130）：唯一會讀取「圖層之下」的效果。`PushEffect` 時後端先把目前目標（根或上一層圖層）在 `boundsHint` 外擴 3σ 的區域複製成 backdrop 緩衝，高斯模糊（σ = radius/2，同 Blur 的上限 64 裝置 px）並套 saturation/brightness（同 ColorAdjust 公式）；圖層內容畫完後，`PopEffect` 合成（premultiplied）：`out = src + backdropBlurred · alpha(src) · (1 − alpha(src))`，也就是先以內容的 alpha 為遮罩鋪上模糊背景、內容再蓋在上面；`out` 再以一般 srcOver 進目標。內容的不透明部分不受影響，半透明部分透出模糊後的背景，全透明部分什麼都不畫（背景原樣保留）。SW：backdrop 從 `Target` 直接讀；GL：`glBlitFramebuffer` 目前 FBO 的區域到暫存紋理。效果序列中 BackdropBlur 必須是第一個（編譯錯誤「backdrop-blur must come first」），且只讀取到 push 當時的內容（同一 stack 內後面的兄弟不算）。髒矩形（23.2）：帶 BackdropBlur 的節點其 `boundsHint` 外擴 3σ 已含在 effectExpand 的讀取範圍；此外只要其下方任何命令變動就必須重畫它——`computeDamage` 把 BackdropBlur 節點的矩形與任何相交的髒矩形聯集（保守）。
- **Outline**：`sdf` = alpha ≥ 0.5 的有號距離（內負外正，18.6）；outer：`m = 1 − smoothstep(radius, radius + softness, sdf)` 且 `sdf > 0`；inner：`m = 1 − smoothstep(radius, radius + softness, −sdf)` 且 `sdf ≤ 0`；center：`m = 1 − smoothstep(radius/2, radius/2 + softness, |sdf|)`；`line = (color.rgb, m · color.a)`；outer 與 center：`dst = srcOver(line, src)`（線在下）；inner：`dst = srcOver(src, line)`（線在上）。

所有 Float 參數可動畫（線性插值）；`kind`、`shader`、停靠點不可動畫。內建效果只使用 `params[0..7]`／`colors[0..1]`；`params[8..15]`／`colors[2..3]` 保留給自訂 shader（18.7）。

## 18.4 CSS 語法（修訂 05）

新增樣式屬性 `effect`（StyleProp 187，不繼承，初始 `none`）：

```
effect: none | <effect-fn>+
<effect-fn> =
    shadow( <len> <len> [<len> [<len>]] <color> [<number>] [inset] ) // offsetX offsetY blur spread color opacity [inset]（T-111）
  | glow( <len> <color> [<number>] [inner] )                        // radius color intensity
  | blur( <len> [<len>] )                                           // radiusX [radiusY]
  | color-adjust( [brightness(<number>)] [contrast(<number>)] [saturate(<number>)] [hue-rotate(<angle>)] [opacity(<number>)] )
  | gradient-mask( linear | radial , <len> <len> , <len> <len> , <color-stop>+ [, invert] )   // p0, p1, stops
  | shine( <angle> <len> <color> [<number> [<number> [<number>]]] ) // angle width color intensity softness position（position 0..1，預設 0，超出範圍夾住並警告；可由第六引數或動畫設定）
  | outline( <len> <color> [<len>] [outer | center | inner] )       // radius color softness position
  | backdrop-blur( <len> [<number> [<number>]] )                    // radius saturation brightness（T-130；對應 CSS backdrop-filter: blur() saturate() brightness()）
  | custom( "<asset-name>" [, <ident> <value>]* )                   // 0.3；0.2 的編譯器回錯誤「custom effects are not supported in this version」
```

- 序列順序即堆疊順序。函式引數順序如上，參數名對應 18.3 表。
- `@keyframes` 與 `transition` 對 `effect`：規則同 5.5 的 `transform`：各關鍵幀必須含相同的效果函式序列（kind 相同、順序相同），差異只在參數；編譯器展開為各參數的 Track（PropId 18.2）。`transition: effect <time>` 對堆疊中每個可動畫參數各自建立 transition（規則同 `transform` 各分量）。序列不同（新增/刪除效果）→ 不過渡，直接切換。
- 執行時 `setStyle("effect: ...")` 走 mini_css（13.4），支援完整語法（`custom` 除外）。
- `ElementRef` 新增：`uint32_t effectCount()`、`EffectInstance effect(uint32_t i)`、`Status setEffectParam(uint32_t i, uint32_t paramIndex, Value)`（等同 `setProperty(effectParam(i, paramIndex), v, sticky=false)`）、`Status setEffects(std::span<const EffectInstance>, std::span<const GradientStop>)`（等同 `setStyle` 寫入一筆執行時 `effect` 宣告，走層疊；機制同 T-35 的執行時覆寫宣告清單）。

**IR 編碼（修訂 02）**：STYL 區塊新增兩個池：`IrEffect effects[]`（佈局同 `EffectInstance`）與 `GradientStop effectStops[]`；`effect` 宣告的 `IrDecl.value` 為 `ListRef { offset, count }` 指向 `effects[]`（與 `transitions[]` 同機制），`IrEffect.stopOffset` 指向 `effectStops[]`。`IrDocElement` **不**帶效果欄位（效果由層疊決定）；`IrSceneNode` 帶 `effectOffset/effectCount` 指向 SCEN 區塊的 `effects[]`（Lottie 效果），其停靠點指向場景的 stop pool。

**層疊（修訂 05.7 / StyleEngine）**：`ComputedStyle` 新增 `ListRef effects`（來源：最高特異性的 `effect` 宣告，整個序列取代，不逐效果合併）。解析時把序列複製進節點效果池（18.2 的池政策）；狀態偽類切換導致序列變化時，若 kind 序列相同 → 對每個參數依 `transition` 規則處理（如同 `width` 的變化）；不同 → 直接取代（進行中的效果參數 transition 全部取消）。

## 18.5 命令清單與後端（修訂 09、10）

```cpp
enum class CmdKind : uint8_t { /* 0–16 見 19.7 */ PushEffect = 17, PopEffect = 18 };
struct EffectCmd {
    uint32_t effectOffset, effectCount;   // → CommandList::effects（EffectInstance 複本；GradientMask 的停靠點複製到 CommandList::stops 並改寫 stopOffset）
    Mat2x3   xf;                          // 節點平面 → 螢幕（含 dpr）；3D 下為錨點處的雅可比（19.6）
    Rect     boundsHint;                  // 內容 AABB + 擴張量（螢幕座標）
};
struct ShaderParamDesc { uint32_t name; uint8_t type; uint8_t _pad[3]; float defaultValue[4]; float minValue, maxValue; };  // name = StringId；type 0 Float 1 Vec2 2 Color
struct ShaderRef {                        // 0.3 使用；0.2 定義結構、序列化與空實作
    AssetId  asset;
    uint32_t sourceOffset, sourceLength;  // → CommandList::shaderText（UTF-8，無 NUL）
    uint32_t paramOffset, paramCount;     // → CommandList::shaderParams
    float    expandPx;
    EffectInstance fallback;              // kind None = 不套用
};
// CommandList 新增：effectCmds, effects, shaders, shaderParams, shaderText(std::vector<char>)
```

**局部量換算**（後端共用工具 `effectToScreen(const EffectInstance&, const Mat2x3& xf) → EffectInstance`）：`s = sqrt(|det(xf.linear)|)`；半徑、blur、spread、width、softness(px) × `s`；位移向量 `(offsetX, offsetY)` 乘 `xf` 的線性部分；角度：把方向向量 `(cos a, sin a)` 乘線性部分後取 `atan2`；GradientMask 的 `p0/p1` 走完整 `xf`（含平移）。換算後的 `EffectInstance` 即後端實際使用的參數；上限 64 px 在換算後檢查。

後端語意：`PushEffect` 等同開一個離屏圖層（範圍 `boundsHint` 夾到視口）；`PopEffect` 對圖層內容依序執行效果，再以 src-over（opacity 1、Normal）合成回上一層。節點自身的 opacity/blend 由外層 `PushLayer` 處理（18.1）。

- **SW**（`Code/Runtime/sw/src/effects.cpp`）：`applyEffect(const EffectInstance& screenParams, Surface& src, Surface& scratch, Surface& dst)`，緩衝取自既有池；演算法 18.3/18.6。
- **GL**（`Code/Runtime/gl/src/effect_pass.cpp`）：每個效果一到兩個 fragment shader；模糊為兩趟 FBO 乒乓；膨脹與距離場以多點取樣（上限 64 px，與 SW 同）；色彩矩陣為 uniform。Custom（0.3）：18.7 的翻譯子集產生 GLSL 330；編譯失敗 → 一次性警告，並改用 `fallback`（`fallback.kind == None` → 不套用）。
- **引擎最小子集（09.5）**：`SimplifyOptions.effects = true`（預設）→ 移除 `PushEffect/PopEffect`，內容原樣保留，`SimplifyStats.droppedEffects` 累計。宿主要支援效果時直接處理 `EffectCmd`（引擎插件：自訂渲染元素，一個離屏 pass 一個效果；內建效果由宿主用自己的 shader 依 18.3 實作；自訂效果用 18.7 的 HLSL 包裝）。
- 烘焙（12.5）：`.xtcl` 序列化 `effectCmds/effects/shaders/shaderParams/shaderText`；`xtframe-bake --simplify` 時走簡化器（效果丟棄）。

## 18.6 SW 演算法細節

- **高斯近似**：三次 box blur。`w = floor(sqrt(12σ²/3 + 1))`；`w` 為偶數時 `w -= 1`；`w < 1` 時直接複製。分離為水平/垂直各三趟，每趟滑動視窗和 O(n)，邊界取 clamp。
- **膨脹（spread）**：對 alpha 做可分離最大值濾波（van Herk/Gil-Werman，O(n)），視窗半徑 `round(spread)`。
- **有號距離場（Outline）**：alpha ≥ 0.5 二值化；對前景與背景各做 8SSEDT（Danielsson 兩趟掃描）得 `dOut`（背景像素到前景的距離）與 `dIn`；`sdf = dOut − dIn`（外正內負）。緩衝大小 = `boundsHint`。
- **色彩矩陣**：18.3 定義的四個矩陣相乘為一個 4×5，逐像素套用。
- 全部效果對 `boundsHint` 內的像素運算；緩衝以池重用，首次用到某尺寸時配置（01.7 的允許例外）。

## 18.7 自訂效果資產（`.xtfx`）— 0.3 實作、格式 0.2 定案

新增資產種類 `AssetKind::Shader = 2`（02.5）。0.2：編譯器遇到 `.xtfx` 資產或 `custom()` 一律回錯誤（見 18.4），`ShaderRef` 只在資料結構與序列化中存在。

`.xtfx` 為 UTF-8 文字：

```
// header：每行一個宣告；以 "---" 結束
name: Ripple
blurb: 水波紋扭曲                         // 可選（T-532）：效果目錄的一句話；缺則顯示檔名
param amplitude float 4 [0, 32]          // param <name> <float|vec2|color> <default> [<min>, <max>]
param speed float 1.5 [0, 10]
param tint color #ffffff
fallback: blur(2px)                      // SW 無法執行時的替代：一個內建效果函式，或 none
expand: 8px                              // 擴張量
---
// body：HLSL 片段函式，簽名固定
float4 xtMain(XtInput i)
{
    float2 uv = i.uv + sin(i.uv.yx * 40 + xtTime * speed) * (amplitude * i.texelSize);
    return xtSample(uv) * tint;
}
```

固定介面（由包裝提供，所有後端一致；顏色皆**非預乘**）：

```hlsl
struct XtInput { float2 uv; float2 texelSize; float2 layerSize; };
float4 xtSample(float2 uv);            // 取本效果的輸入圖層（非預乘 RGBA，uv 超出 [0,1] 取 0）
float  xtTime;                         // Context 時鐘（秒）
// 參數依宣告順序注入為同名全域變數（float / float2 / color → float4）
```

**允許的 HLSL 子集**：型別 `float, float2, float3, float4, int, bool`；算術、比較、邏輯運算子與 swizzle；內建函式 `abs sin cos tan atan2 exp log pow sqrt floor ceil frac fmod min max clamp saturate lerp step smoothstep length distance normalize dot cross`；`if/else`；`for` 迴圈上下限為整數常數且 ≤ 64 次；區域變數；不允許自訂結構、陣列、貼圖物件與 `Sample`、`discard`、指標、遞迴、全域可變狀態。`xtframe-compile` 以自有的子集解析器驗證，違反者為編譯錯誤並指出行列。

- **GL 翻譯**：子集解析器輸出 GLSL 330：`float2/3/4 → vec2/3/4`、`lerp → mix`、`frac → fract`、`saturate(x) → clamp(x, 0.0, 1.0)`、`atan2(y, x) → atan(y, x)`、`fmod(a, b) → a - b * trunc(a / b)`（HLSL 語意，輔助函式）、`xtSample(uv) → texture(uLayer, uv)`；整數字面值補 `.0` 於浮點語境由解析器的型別推導處理。
- **HLSL 宿主（引擎插件）**：宿主把 body 包進自己固定的四邊形 shader（`Texture2D uLayer`、`cbuffer XtParams`），用引擎的 shader 管線編譯；`ShaderRef` 提供 `source`、參數表、`expandPx`、`fallback`。
- **SW**：不執行 body，一律套用 `fallback`；`xt_golden` 對含 custom 的 fixture 只以 GL 比對。
- 安全：body 只能讀輸入圖層與參數，無 I/O，迴圈有界；載入時視為不受信任輸入（子集解析器拒絕未知語法）。

## 18.8 效果模板（編輯器層，資料格式）

`.xtfxp`（JSON）：

```json
{ "name": "Soft Card", "thumbnail": "soft_card.png",
  "effects": "shadow(0 4px 12px #00000040) outline(1px #ffffff80 inner)",
  "animations": [ { "param": "0.blur", "keyframes": [[0, 12], [1, 20]], "easing": "ease-in-out", "duration": 2, "loop": true } ] }
```

- `effects`：18.4 的 `effect:` 值。
- `animations[].param`：`"<堆疊索引>.<參數名>"`，參數名為 18.3 表的名稱（`color` 為 `color0`、第二色 `color1`）；`keyframes` 為 `[offset 0..1, value]`，`value` 為數字或（顏色參數）`"#rrggbb[aa]"` 字串（2026-10-30 起，T-535；之前只收數字，顏色動畫被略過）；~~展開為 `@keyframes <模板名>-<n>` + `animation: <name> <duration>s <easing> [infinite]` 寫入樣式表~~ → 18.12「模板動畫」：寫成具名時間軸。
- 核心不知道模板；編輯器的模板庫（內建 + 專案）於 17 章。

## 18.9 Lottie 效果映射（修訂 11）

圖層 `ef[]` 依 `ty`：

- `ty:29` Gaussian Blur：`ef[0]` Blurriness `b`、`ef[1]` Blur Dimensions（1 both / 2 horizontal / 3 vertical）→ `Blur(radiusX, radiusY)`，`radius = b / 2`（AE 的 blurriness ≈ 直徑；記錄為近似），依 dimensions 把另一軸設 0。
- `ty:25` Drop Shadow：`ef[0]` Shadow Color、`ef[1]` Opacity（0–255）、`ef[2]` Direction（度，AE 定義 0° 朝上、順時針）、`ef[3]` Distance、`ef[4]` Softness → `DropShadow(offsetX = dist·sin(dir), offsetY = −dist·cos(dir), blur = softness, spread = 0, opacity = op/255, color)`。
- 其他 `ty`（Tint、Fill、Tritone、Stroke…）→ 警告「unsupported effect」並忽略。
- 效果參數若為動畫（`k` 為關鍵幀）→ 照 11 章既有規則產生 Track，目標 PropId 用 18.2 編碼。

## 18.10 對既有章節的修訂條款

| 章節 | 修訂 |
|---|---|
| 02 | `AssetKind::Shader = 2`；STYL 新增 `effects[]`、`effectStops[]` 池與 `ListRef` 型別的 `effect` 宣告；SCEN 新增 `effects[]`；`IrSceneNode` 的 `effectOffset/effectCount`（位置見 19.10）；與 19 章一起升 `major = 2`。 |
| 03 | `NodeCommon` 效果欄位（接在 19.3 欄位後）、`EffectInstance`、PropId 0x8000+ 編碼（3.9 新節「效果參數」）；3.10 效果參數插值規則。 |
| 05 | 5.4 新增 `effect`（StyleProp 187）；5.5 之後新增 5.5b「effect 序列規則」；5.7 層疊：`effect` 整序列取代。 |
| 09 | `PushEffect/PopEffect` 與 payload；9.3 依 18.1 的順序（Element 有效果且 opacity < 1 時改走 Layer）；9.5 最小子集丟棄效果。 |
| 10 | 10.2.8 SW 效果、10.3.7 GL 效果 pass；GL/SW 互比沿用 15.3 的 16/255、3%。 |
| 11 | 依 18.9。 |
| 12 | `xtframe-compile` 解析 `effect`；`custom()`/`.xtfx` 於 0.2 回錯誤；`.xtcl` 序列化效果與 `ShaderRef`。 |
| 13 | `ElementRef` 效果 API（18.4）；`Document::compactEffects()`。 |
| 15 | 黃金圖 `ui_fx_<effect>`（七種各一張靜態）、`ui_fx_shadow_anim`（位移動畫中間幀）、`ui_fx_stack`（陰影 + 描邊 + 色彩）、`ui_fx_3d`（3D 圖層上的陰影，驗證 xf 換算）；`lottie_fx_blur`、`lottie_fx_shadow`。 |
| 16 | M8 任務 T-50～T-55（16.2.2）；`.xtfx` 子集解析器與 GL 翻譯為 0.3 任務。 |

## 18.11 參考

- CSS Filter Effects Module Level 1：`drop-shadow()`/`blur()` 語意與色彩矩陣定義；`filter` 與 `opacity`/`overflow` 的順序（18.1 採用：效果在裁切之外、opacity 之內）。
- After Effects 的效果堆疊：概念；本章改為螢幕空間執行並以 `xf` 換算局部量，是刻意的簡化。
- CryEngine `Engine/Shaders/HWScripts/CryFX/AuxGeom.cfx` 的 technique 結構與 `RenderDll/XRenderD3D9/D3DHWShaderCompiling.cpp` 的 `D3DCompile` 字串編譯路徑：18.7 的包裝方式據此設計；只取結構。
- Kovesi, "Fast Almost-Gaussian Filtering"；Danielsson 8SSEDT；van Herk/Gil-Werman 最大值濾波：演算法來源。

## 18.x T-160 修訂

只有 `background-gradient` 的元素算一個可繪項（`m_drawableCount`），效果（shadow 等）才會套用。

## 18.12 編輯器藍圖新增（2026-09-27 定案）

- **效果參數定址**：`@timeline` 與 `bind:` 可寫 `effect.<索引或效果名>.<參數名>`（18.3 表的名稱；同名效果多個時用索引），對應既有 PropId 0x8000+；序列本身不可動畫（18.4 規則不變）。
- **模板動畫**：`.xtfxp` 的 `animations` 套用時改寫成一條具名時間軸 `<id>-fx-<模板名>`（可 `loop`），不再產生匿名 `@keyframes`；模板縮圖以動畫中段渲染。**T-533 實作細則**：時間軸名 `<id>-fx-<slug(模板名)>`，寫入目前範圍；每個 `animations[i]` 的 `param`（`"<堆疊索引>.<參數名>"`，`color0`→`color`）→ 該元素的一條 `effect.<堆疊索引>.<參數名>` 軌，`keyframes` 的 offset × duration 為絕對時間，`easing` 寫在每個到達幀；`loop` 時模板套用同時寫一條觸發 `on:show="play(<時間軸名>, loop)"`（27.2.3 的播放動作；若元素已有 `on:show` 則不寫並回報）。同名時間軸已存在 → 整條取代（一個 undo 步）。
- **自訂效果 `.xtfx`**（18.7）分兩步：M23 先做核心子集——**格式與子集以 18.7 為準**（HLSL 子集、`xtMain(XtInput)`、header 參數表、`fallback`、`expand`；自有子集解析器驗證，GL 翻譯為 GLSL 330，SW 一律套 `fallback`，CE 直接包 HLSL body），參數表由檔案宣告、效果面板自動生成 UI；UX（節點式或即時編輯）之後。（本條原寫的 GLSL ES 子集與 `uParams[8]` 作廢，2026-09-27 修訂。）
- **`mix-blend-mode`**（0.6；**2026-10-14 M32 T-326 改為全部**）：`normal | multiply | screen | overlay | darken | lighten | color-dodge | color-burn | hard-light | soft-light | difference | exclusion | hue | saturation | color | luminosity | add`（`additive` 為 `add` 別名），元素層級，不繼承、不可動畫；走 18.1 的 `PushLayer(opacity, blend)`（blend != Normal 才多一層離屏）；damage 比照 18.3 BackdropBlur 與下方內容聯集；SW 與 GL 後端的 Layer 合成已有全部模式（場景節點既用）。
- **`clip-path`**（0.6）：`inset() | circle() | polygon()`；繪製走 stencil／mask，命中測試用多邊形（08）。
- **`render-cache: auto | always | never`**（0.6）：`always` 把靜態子樹當效果區段烘成快取貼圖（T-162 機制），`never` 停用。
- **粒子**：3.12.5；09 章新命令 `DrawParticles`（實例化，每粒子 pos/size/rot/color/uv）；GPU 更新在 CPU（M23）或 compute（之後）。→ T-202（#202）完成（03 §3.12.9、09 §9.7、10 §10.7）；3D 投影內不繪製粒子。

## 18.13 T-203 實作記錄（#201）：`.xtfx` 核心子集

- **解析**：`Code/Runtime/compile/src/xtfx/{xtfx_parse, hlsl_subset, glsl_emit}.cpp`、`Code/Runtime/compile/include/xt/compile/xtfx.h`；`custom()` 載入資產、寫 ASST kind 2 與 SHDR（02 §2.v）；參數預設值寫進 `EffectInstance`，`custom()` 覆寫並檢查型別。
- **強制的子集**（18.7 之外的細則）：`%` 只限 int；swizzle 檢查元件範圍且 xyzw 與 rgba 不混用；內建函式逐一檢查參數個數與型別；`for` 計數器必須 `int n = 常數`、條件 `n <|<=|>|>= 常數`、步進 `++/--/+=c/-=c` 非 0、模擬次數 ≤ 64；拒絕 struct、陣列與索引、Texture/Sampler 與 `.Sample`、discard、while/do/switch/break/continue、**三元運算子**、`xtMain` 以外的函式、全域宣告、對參數或 `xtTime` 賦值、迴圈本體改計數器、與 GLSL 保留字或 wrapper 名相撞的識別字。錯誤附整檔行列。~~已知缺口：純量 swizzle（`f.x`）能過檢查但 GLSL 330 不支援~~ → T-208（#206）：純量 swizzle 為附行列的編譯錯誤。
- **翻譯**：18.7 表逐條有測試；`fmod → xtFmod`（float 與 vecN 重載）、`xtSample → texture(uLayer, xtTexCoord(uv))`（[0,1] 外回 0，FBO 預乘色轉回非預乘）；int 變數包 `float()`、純量在向量語境包 `vecN()`。module 自行宣告 `uTexelSize/uLayerSize`；wrapper 定義 `xtTexCoord`（y 翻轉）並把輸出 clamp 後預乘；`XtInput.uv` 由上往下。
- **參數槽位與 PropId**：依宣告順序，float 佔 `params[s]`、vec2 佔 `params[s..s+1]`、color 佔 `colors[c]`；上限 8 個 float 分量與 2 個顏色（超過為 header 錯誤）。PropId（**2026-10-30 第二次定案，T-534；取代 T-532 的「不擴充編碼」**）：float 槽 s → `kEffectParamFloatBase + s`（1..16）、color 槽 c → `kEffectParamColorBase + c`（32..35）、vec2 自槽 s 起 → **整體** `kEffectParamVec2Base + s`（64..78，Vec2 軌）或 **分量** `.x` → `1+s`、`.y` → `2+s`（Float 軌）。上限 **16 個 float 分量與 4 個顏色**（超過為 header 錯誤）。

**AE 式的預設（使用者 2026-10-30：「AE 有這種麻煩嗎？」→ 照 AE）**：vec2 參數預設是一個屬性、一個關鍵幀、一條緩動（AE 的 Point 參數）；`.x`／`.y` 是「Separate Dimensions」，要 x、y 不同緩動時手動用。同一條時間軸對同一個 vec2 同時用整體與分量是編譯錯誤「vec2 parameter addressed both whole and by component」（T-532 已做）。效果面板上 vec2 是一列一顆菱形，記錄模式寫一筆 Vec2 鍵（T-535）。

**定址語法**（`@timeline`、`.xtanim.json`、`bind:`；`parseEffectAddress`）：
- 名字：`effect.<效果>.<參數名>[.x|.y]`（T-532）。值的型別在 **emit 階段**依 header 決定（解析器先存原文 token；T-532 時「只有名字叫 `color` 才當顏色」的檢查作廢）：float／分量 → 一個數字；color → 顏色；vec2 整體 → 兩個數字（`<x> <y>`，可帶 px）。
- 槽位：`effect.<效果>.p<n>`（float 槽 n，0..15）、`effect.<效果>.c<n>`（color 槽 n，0..3）、`effect.<效果>.v<n>`（自槽 n 起的 vec2 整體，0..14）。型別由前綴決定，不需 header；對內建效果也合法（`effect.shadow.v0` = 位移）。slot 名與 header 參數名撞名時 header 名優先（header 參數名不得是 `p<n>`／`c<n>`／`v<n>` 形式，否則 header 錯誤「reserved parameter name」）。
- `.x`／`.y` 只對 vec2 合法（float／color → 錯誤「parameter has no components」）。`effect.<名>` 取 header `name:`，缺則檔名主幹。
- **fallback 限制**：不可為 gradient-mask（停靠點無處存）或 backdrop-blur（必須排堆疊第一）。
- **ShaderRef**（18.5）尾端新增 `glslOffset, glslLength, namesOffset, namesLength`（names 為 NUL 分隔的參數名，放 `shaderText`，供 GL 當 uniform 名）；`PackedShaderRef` 同步；`CommandList::time` 尾端新增、不序列化；Painter 把 ShaderRef 複製進 CommandList（每資產一份）；Custom 的 boundsHint = `max(expand×s, fallback 自身擴張)`。
- **GPU／GL**：`EffectProgram::Custom = 13`、`CustomEffectParams`、`GpuDevice::prepareCustomEffect`（預設回 0）接介面尾端；實作在 `Code/Runtime/gpu/src/{frame_builder, gpu_effects}.cpp` 與 `Code/Runtime/gl/src/{gl_effects, gl_device}.cpp`（任務單寫的 `effect_pass.cpp` 不存在）；program 以 GLSL 文字為快取 key；編譯失敗由 `GlRenderer::lastWarning()` 警告一次後套 fallback；含 custom 的區段不進 T-162 快取；時鐘前進時 custom 效果矩形列入 damage（23 §23.2）。~~已知缺口：transition 比較效果序列只比 kind~~ → T-208（#206）：`style_engine.cpp` 的 `applyEffectCascade()` 對 Custom 一併比 `shader`，不同 shader 整段替換（`NodeArena::assignEffects()` 的 `sameSequence` 只管原地覆寫，本就複製 shader，未改）。
- **SW**：一律 fallback；找不到 ShaderRef 或 fallback 為 `none` 時跳過。黃金 manifest 新增 `gl-only` 標記：`xt_golden` 跳過、`xt_gl_tests` 比對（期望圖缺時由 GL 測試寫出並判該次失敗）；fixture `ui_fx_custom`（Ripple + 用到 vec2 與 for 的 Swirl）。
- **CE 契約**：`Document::shaderRef(asset)`、`shaderParams()`、`shaderText()`，由公開的 `ShaderTable`（`Code/Runtime/core/include/xt/shader_table.h`）自 SHDR 建立。
- **編輯器**：`EffectStack` 來回轉換 `custom("a", p v…)`；效果面板依 header 生成 float 滑桿+spin、vec2 兩欄、color 色票，另一行顯示 fallback 與 expand；`AssetKind::CustomEffect`，`<assetRoot>/fx/*.xtfx` 列在 Effect Templates 分類、可拖進堆疊（路徑相對於文件目錄）；lint `fx.customUnsupportedOnSw`（info）。執行期 `setStyle` 遇 `custom()` 仍回 kUnsupported（18.4）。
- **bundle**：`assets/<hash>.xtfx`，manifest `kind: shader`；打包後 `.xtir` 帶 SHDR。

> T-211（#210）：`effect.<n>.<param>` 定址到不存在的效果（元素無靜態 effect、或動畫中被 class／狀態移除）時，`setEffectProperty` 在所有建置下為 no-op（不再 `XT_ASSERT`），`NodeArena::noteEffectMissingWritten()` 記錄，`Document::update()` 轉為每份文件一次的警告 `xt::kWarnEffectMissing`（`document.h`）。

## 18.14 現況說明：小半徑模糊（2026-10-07 審查修正鏈合併；T-263f #303；待修）

`boxRadius`（`Code/Runtime/sw/src/sw_effect_math.cpp`）以 `w = floor(sqrt(12σ²/3 + 1))` 取盒寬、偶數減一，寬 1 為 identity；σ = radius / 2，故半徑低於約 2.83 **裝置** px（經 dpr 與變換縮放後）的模糊沒有效果（`blur(2px)` 在 dpr 1 無作用、dpr 2 才模糊），三個 pass 都用向下捨入的寬度，整體偏淡；GPU 刻意鏡射（`swEffectiveSigma`、`gaussIsIdentity`）。另：SW 模糊在 alpha 0 下殘留顏色（自訂效果與一般模糊路徑有 12 px 差）。修正會改變現有專案的小模糊與陰影外觀（估 10～13 張 UI golden、11 張 GPU 快照），列為合併後的獨立任務，由主模型逐張審圖。字形效果序列的 atlas 上限見 10 §10.8。

## 18.15 降解析度模糊與共用 backdrop 金字塔（2026-10-15；23 §23.aa；T-343）

- **一般 `blur`／`glow`／`shadow` 的大 σ**：σ（裝置 px）> 4 時先把來源降到 1/2，σ > 8 降到 1/4，σ > 16 降到 1/8（每層 σ 折半），在該層做高斯（或 dual-Kawase 近似，誤差 ≤ 1/255 於 golden 容差內）後雙線性放回；小 σ 維持現狀。golden 以新容差重產，回報差異。
- **backdrop 金字塔**：一幀內所有 `backdrop-blur` 共用：合併讀取區域 → 降採樣鏈（層數依最大 σ）→ 每個面板依自己的 σ 取層（相鄰兩層線性混合）→ 面板自己的 saturation／brightness。`EffectInstance` 參數不變。
- **場景靜止快取**：宿主 `setSceneStatic(true)` 時金字塔與面板結果快取到帶圖層，直到場景或面板變動。
- damage：S 元素的讀取外擴照 `effectReadExpand`；金字塔每層的邊界多取一個 texel。

## 18.16 效果參數的資料驅動寫入走 Override 層（2026-10-30 定案；T-545）

T-544 把 `bind:effect.<效果>.<參數>` 做在第 0 層（`setProperty` 直寫 + `effectPending` 補套一次），結果 restyle 重建效果堆疊時綁定值遺失。使用者裁決只走正解：**綁定與 flow 對效果參數的寫入一律走 04 §4.7 的第 4 層 Override**（`sticky = true`），`| tween(d)` 以「Override 帶補間」實作（04 §4.7 補充），`effectPending` 機制移除（Animator 每幀重套本來就涵蓋「載入時效果尚未建立」的情況）。`@timeline`／`animation` 對效果參數仍是第 2／3 層，被綁定的 Override 蓋掉——與一般屬性相同的規則，不另例外。宿主 `Document::setEffectParam()` 維持第 0 層（文件註解已說明等價於 `sticky = false`）；要持久的宿主寫入自己用 `setProperty(effectParam(...), v, true)`。

## 18.13 渲染一致性修正（2026-11-01 定案；改進報告 R11、F32；T-572）

- SW 的 `box-shadow: inset` 必須與 GL 相同：畫環，不塗滿盒子；加 SW／GL 共用黃金圖。
- 父層 `border-radius` 加 `overflow: hidden` 時，圓角裁切套用到子元素（SW 與 GL）；dialect.md 的承諾維持。
- 3D 翻轉：`backface-visibility: hidden` 以**合成後**的變換判定背面（父層 `rotateY(180deg)` 時正面剔除、背面顯示）。
- `brightness()`／`hue-rotate()` 維持 §18.3 的定義（不改成 CSS 矩陣）；匯入器對 CSS 寫法發 `degraded: brightness is additive in XTFrame (18 §18.3)`。

**T-572 實作記錄（#544，2026-11-04；偏差主模型核准）**：F24 根因＝SW inset 陰影寫非預乘像素到預乘 src-over（既有黃金圖全黑看不出），改走 `writeTinted()`；F25 圓角 overflow 用既有 `PushClipPath`（09 章 `ClipRectCmd` 無半徑欄位，命令格式不動），方角仍 `PushClipRect`；F32 根因＝`rotateY(0deg)` 的正面不是 3D 圖層、2D 節點從不做背面判定，修法在 `Painter::enterNode` 對空間根子樹內 `backfaceHidden` 的 2D 節點以合成矩陣判定。新黃金圖 5 張（`ui_3d_flip_90` 取 0.48 s 因恰好 90° 全黑），既有零張改變；SW／GL 對照全過。未做（候選）：命中測試仍用方形 padding box、翻到背面的 2D 正面仍可點、無空間根的 flattened 3D 不判背面。

## 18.17 效果段與 shader 編輯體驗（2026-10-30 定案；T-532／T-533；雲端分支原編號 §18.16，主線 2026-11-04 移植時改號，§18.16 已是 T-545；文中「18.13 的 AE 式預設」指 §18.7 的「AE 式的預設」段）

使用者 2026-10-30：shader 參數要能用關鍵幀控制；效果面板「很不好看，像是被什麼關著一樣」。研究結論：核心（18.2 PropId）、編譯器（`@timeline` 的 `effect.<名|索引>.<參數>`，含 `.xtfx` header 名）、GL／CE 後端（`CustomEffectParams` 每幀上傳）都已支援，缺的是編輯器。現況的病根：`PropertyPanel::addEffectsSection()` 把整個 `EffectsPanel`（內層堆疊 `QListWidget` + 參數 `QScrollArea` + 範本 `QListWidget` + 兩顆鈕，各 stretch 1）當一列塞進屬性面板的效果群組框並硬設 360 px 高——捲動區裡的群組框裡再包三個框。本節取代 17 §17.5「效果」列與 46 §46.5 對效果段的描述。

### 18.17.1 效果段 = 堆疊式單位（46 §46.5 的規則真正套到效果）

- 效果鏈裡每個效果一個**單位**：標頭列 + 參數列。沒有內層清單、內層捲動區、固定高度；高度隨內容，捲動交給屬性面板。
- **標頭列**：啟用勾選 ＋ 目錄圖示（46 §46.13.4 的 `icon`）＋「目錄名 · 摘要」（T-466 的 `effectName`／`effectSummary`）＋ 右端「⋯」選單：上移、下移、複製、重設為預設、另存為範本…、刪除。拖曳標頭列重排（既有 InternalMove 的語意搬到標頭）。摺疊 `▸／▾` 狀態記 `properties/collapsed/effects.<索引>`。
- **參數列**：走 17 §17.26.2 的三欄格線（碼表欄 | 標籤 | 值），由屬性面板的 `addRow()` 建列，列名用 T-466 的 `kEffectParamRows`。碼表欄放關鍵幀菱形與前後鍵（同 `property_panel_keyframes.cpp` 的三顆鈕），只在記錄開或時間軸選中時出現（同屬性列規則）。
- **自訂 shader 單位**：標頭顯示 header `name:`（缺則檔名主幹）與檔名（暗色小字）；參數列由 header 生成：float → `DragSpinBox`（範圍來自 header 的 `[min, max]`；不再另放 `QSlider`）、vec2 → 兩欄 X／Y、color → `ColorButton`；之後兩列暗色唯讀：「SW 替代」= fallback 原文、「擴張」= expand。「⋯」多兩項：「在原始碼面板開啟」（`EditorWorkspace::openFile`）、「重新載入」（重讀 header、重建參數列；header 解析失敗時單位顯示錯誤列而非整段消失）。
- **範本**不在效果段：套用走效果目錄的 `template:<name>`，另存走「⋯」。`EffectsPanel` 的範本清單、Apply／Save 鈕刪除；`templateList()` 介面改為 `templates()`（名稱清單）供目錄使用。
- **獨立效果停靠窗**（17 §17.5）顯示同一個 widget（不嵌群組框），上方多一列「目標：<圖層名>」。

### 18.17.2 關鍵幀

- 菱形行為同屬性面板：點一下在播放頭加／刪該參數的關鍵幀（`SetKeyframe`），軌道名 `effect.<定址>.<參數名>`；`<定址>` 用效果名（內建：18.4 函式名；自訂：header name），同種效果在鏈上出現兩次以上時改用堆疊索引（與 `planPaste` 的路徑規則一致）。
- **記錄模式**（`recordKeyframes()` 且狀態 Normal）下改參數：推 `SetKeyframe` 而不是整串 `SetStyle`；vec2 推 `.x`／`.y` 兩筆（一個 macro）；拖曳中仍 `applyLive`。啟用勾選、重排、增刪效果不受記錄影響，永遠寫靜態 `effect:`（序列不可動畫，18.4）。
- `effect` 這個 StyleProp 本身不加 `kAnimatable`（整串效果不能進 `@timeline`）。
- **vec2 的整體／分量選擇（2026-10-30，T-535 回報 1 的裁決）**：面板預設寫整體鍵（18.13 的 AE 式預設）。但若選中的時間軸對該元素、該 vec2 **已有任一分量軌**（自訂 `<名>.x`／`.y`，或投影的 `offsetX`／`offsetY`——Lottie 匯入器寫的形式），面板改寫分量鍵（兩筆一個 macro），菱形狀態也看分量軌；反之已有整體軌時一律寫整體。與 AE 一致：一旦 Separate Dimensions 就維持分開。不自動轉換既有軌道。
- 編輯器讀 `.xts` 的 `@timeline` 時，`effect.` 開頭的屬性名**只有第一段**小寫化，其餘保留大小寫（header name 區分大小寫；`timeline_ops.cpp` 現在把整個名字小寫，是 bug，T-533 修）。

### 18.17.3 加 shader 的入口

- 效果目錄（46 §46.13.4）`custom` 項分成兩項：**自訂 shader…**（開檔）、**新建 shader…**（在 `<assetRoot>/fx/<名>.xtfx` 寫起手檔——`Assets/Fx/starter.xtfx`，問名字、檢查不重名——加進鏈尾並在原始碼面板開啟）。
- **內建範例** `Assets/Fx/`：`ripple.xtfx`（18.7 範例）、`swirl.xtfx`（T-203 fixture 的 vec2＋for）、`starter.xtfx`（最小：一個 `strength` float，`return xtSample(i.uv)` 乘 tint）。目錄「效果」分類列為 `shader:<檔名主幹>`，名稱取 header `name:`，一句話取 header 新增的可選行 `blurb: <文字>`（18.7 header 多一個可選宣告；缺則顯示檔名）。套用 = 複製到 `<assetRoot>/fx/`（T-273 既有規則）+ 加進鏈尾。隨編輯器安裝（`xt_copy_standard_*` 同款，`<exe dir>/fx`）。
- 拖 `.xtfx` 到效果段（既有 T-203）保留。
- **開啟 shader 原始碼**（使用者 2026-10-30：「先用外部」）：自訂單位的「在原始碼面板開啟」改為 `QDesktopServices::openUrl`（系統預設程式），存檔回來按「重新載入」。編輯器內的 shader 原始碼分頁列入之後的 shader UX 單；屆時先檢查其他分支既有的文字編輯器／Markdown 閱讀器能否重用（使用者提示）。
