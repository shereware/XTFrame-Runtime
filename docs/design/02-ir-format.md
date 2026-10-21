# 02 · `.xtir` 二進位格式

## 2.1 總則

- 小端序（little-endian）。所有多位元組整數與 `float`（IEEE 754 binary32）皆是。
- 檔案由**標頭**加上一串**區塊（chunk）**組成。每個區塊起點 4 位元組對齊：payload 尾端補 0 到 4 的倍數，**填充計入 `payloadLength` 並受 CRC 涵蓋**；下一區塊緊接其後。讀取器在消耗完 `chunkCount` 個區塊後若仍有剩餘位元組，視為無效。
- 區塊內所有「偏移」皆相對於**該區塊 payload 起點**，單位位元組。
- 所有陣列以「count (uint32) + 元素」形式存放，元素緊密排列，結構欄位無隱含填充（規格中明確標出 `_pad`）。
- 讀取器對每個偏移與 count 做邊界檢查；失敗回傳 `Status::kInvalidIr`。
- 讀取器**不修改**IR 記憶體；執行時資料另外複製到 NodeArena/Timeline。字串表則直接借用。
- 版本：`major` 不同拒絕載入；`minor` 較新則忽略未知區塊型別。**1.x 檔案不再支援**（`major` 必須為 2，否則 `IrView::open` 回傳 `kInvalidIr`；T-42c 起）。

## 2.2 標頭（16 位元組）

| 偏移 | 型別 | 內容 |
|---|---|---|
| 0 | char[4] | `"XTIR"` |
| 4 | uint16 | major = 2 |
| 6 | uint16 | minor = **6**（T-335 #368，2026-10-15：STYL 尾端 `easingPoints[]` 池 + `LengthUnit::Rem = 14`）；歷史：0 無 gradientPool → 1 gradientPool（T-115）→ … → 5（M31 前）→ 6。讀取端對較舊 minor 以「缺少的尾段視為空」處理；較新 minor 依本節規則忽略未知區塊，但含 `Rem` 的宣告會被舊驗證器拒收 |
| 8 | uint32 | 區塊數 |
| 12 | uint32 | 檔案總長度（含標頭） |

## 2.3 區塊標頭（12 位元組）

| 偏移 | 型別 | 內容 |
|---|---|---|
| 0 | char[4] | 區塊型別 |
| 4 | uint32 | payload 長度 |
| 8 | uint32 | CRC32（payload），讀取器可選擇驗證，`IrReadOptions::verifyCrc` |

區塊型別與必要性（順序固定如下，缺必要區塊即無效）：

| 型別 | 必要 | 內容 |
|---|---|---|
| `STRT` | 是 | 字串表 |
| `ASST` | 是 | 資產表（可為 0 筆） |
| `STYL` | 是 | 樣式表 |
| `SCEN` | 否 | 場景模板（多個） |
| `TLIN` | 否 | 時間軸（多個） |
| `DOCT` | 是 | 文件元素樹 |
| `SRCM` | 否 | 來源映射（元素/文字 → 來源檔/行/列），位置在 `DOCT` 之後、`META` 之前；T-42c 起 |
| `META` | 否 | 來源檔名、編譯器版本等偵錯資訊 |

## 2.4 `STRT` 字串表

```
uint32 count
uint32 offsets[count]        // 每個字串在本區塊的偏移
uint32 lengths[count]        // 位元組長度，不含結尾 NUL
uint8  data[]                // UTF-8，每個字串後接一個 NUL（方便 C 字串），NUL 不計入 length
```

`StringId` = 索引。編譯器**去重**字串。

## 2.5 `ASST` 資產表

> T-199（#197）：新增資產種類 **3 = 圖集子矩形**，名稱形如 `atlas:<name>#<asset>`；核心載入時由 `AssetResolver::imageMeta` 取子矩形填 `imageSubUv`。

```
uint32 count
Asset  assets[count]

Asset (16 bytes):
  uint32   name        // StringId，邏輯名稱（例如 "images/logo.png"）
  uint8    kind        // 0 = Image, 1 = Font, 2 = Shader（T-42c 起；語意由 18 章效果實作陸續補上）
  uint8    embedded    // 1 = 位元組內嵌於本區塊，0 = 由 AssetResolver 依 name 解析
  uint16   _pad
  uint32   dataOffset  // embedded 時有效
  uint32   dataLength
```

內嵌資料緊接在陣列之後。圖片內嵌格式為原始檔案位元組（PNG/JPEG），解碼由宿主的 `AssetResolver` 負責（`xt_sw`/`xt_gl` 用 vendored `stb_image`）。

## 2.6 `STYL` 樣式表

樣式表已在編譯期展開成「規則清單」。每條規則 = 一個選擇器鏈 + 一組宣告 + 特異性。

```
uint32 ruleCount;          Rule           rules[ruleCount]
uint32 selectorPartCount;  SelPart        selectorParts[selectorPartCount]
uint32 declCount;          Decl           decls[declCount]
uint32 keyframesCount;     KeyframesRef   keyframesRefs[keyframesCount]
uint32 classIdCount;       uint32         classIds[classIdCount]        // StringId
uint32 transitionCount;    TransitionSpec transitions[transitionCount]
uint32 animationCount;     AnimationSpec  animations[animationCount]
uint32 fontFamilyCount;    uint32         fontFamilies[fontFamilyCount] // StringId，font-family 清單池
uint32 fontFaceCount;      IrFontFace     fontFaces[fontFaceCount]      // @font-face 宣告
uint32 effectCount;        IrEffect       effects[effectCount]          // T-42c 起；由 Decl 的
                                                                         // ListRef 引用（prop == 187）
uint32 effectStopCount;    GradientStop   effectStops[effectStopCount]  // T-42c 起；IrEffect.stopOffset 指向此池
uint32 gradientCount;      IrGradient     gradientPool[gradientCount]   // minor 1（T-115）起；Decl prop 190 的 ListRef 引用；讀取端在 effectStops 後剩餘不足 4 bytes 視為 minor 0（空池）
uint32 easingPointCount;   EasingPoint    easingPoints[easingPointCount]   // minor 6（T-335）起，位於 floatPool 之後；{f32 in, f32 out}；STYL 的 transition/animation 與 TLIN 關鍵影格的 IrEasing（kind LinearPoints=4：stepCount=點數、x1 位元組=起點索引）引用；舊檔無此段讀成空池

IrGradient (32 bytes):
  uint8  kind         // 1 linear、2 radial
  uint8  units[4]     // params[i] 的 LengthUnit（linear：只用 [0]，deg）
  uint8  _pad[3]
  f32    params[4]    // linear：angle；radial：rx ry cx cy
  uint32 stopOffset, stopCount   // 指向 effectStops[]（同一個 stop 池）

Rule (20 bytes):
  uint32 selOffset, selCount    // 指向 selectorParts；順序為由右到左（主體在前，祖先在後）
  uint32 declOffset, declCount  // 指向 decls
  uint32 specificity            // (idCount << 16) | ((classCount + pseudoCount) << 8) | tagCount
  // 來源順序 = 在 rules 陣列中的索引

SelPart (16 bytes):
  uint8  combinator      // 0 = 主體（第一個 part）, 1 = descendant（空白）, 2 = child（>）
  uint8  classCount
  uint16 stateMask       // 需要的 ElementState 位元（:hover 等）
  uint32 tag             // StringId 或 kInvalidString（萬用）
  uint32 id              // StringId 或 kInvalidString
  uint32 classOffset     // 指向 classIds[]

Decl (24 bytes):
  uint16 prop            // StyleProp（見 05.4；可動畫 PropId 與不可動畫樣式屬性共用同一編號空間）
  uint8  valueType       // ValueType；StyleProp 專用型別見下
  uint8  important       // 1 = !important
  uint32 _pad
  uint8  value[16]       // 依 valueType 解讀：
                         //   Float:      f32 at 0
                         //   Vec2:       f32 x, f32 y
                         //   Color:      f32 r, g, b, a
                         //   Length:     f32 value, u8 unit
                         //   Enum:       u32
                         //   StringId:   u32
                         //   ListRef:    u32 offset, u32 count（transition→transitions[]、
                         //               animation→animations[]、font-family→fontFamilies[]）

Easing 序列化 (20 bytes):
  u8 kind; u8 stepsJumpEnd; u16 stepCount; f32 x1, y1, x2, y2

TransitionSpec (32 bytes):
  u16 prop               // PropId；0xFFFF = all
  u16 _pad
  f32 duration, delay
  Easing easing

AnimationSpec (48 bytes):
  u32 timeline           // TimelineId
  f32 duration, delay
  Easing easing
  u16 iterations         // 0 = infinite
  u8  direction          // 0 normal, 1 reverse, 2 alternate, 3 alternate-reverse
  u8  fill               // FillMode
  u8  playState          // 0 running, 1 paused
  u8  _pad[3]
  u32 _pad2[2]

KeyframesRef (8 bytes):
  u32 name               // StringId
  u32 timeline           // TimelineId

IrFontFace (12 bytes):
  u32 family             // StringId
  u32 asset              // AssetId（ASST kind = Font）
  u16 weight             // 100..900
  u8  italic
  u8  _pad

IrEffect (80 bytes，T-42c 起；佈局同 18-effects.md §18.2 EffectInstance，STYL/SCEN 共用此型別):
  u8  kind               // EffectKind，合法範圍 {0..7, 128}
  u8  enabled             // 0 或 1
  u8  _pad[2]
  u32 shader              // AssetId（ASST kind = Shader）或 0xFFFFFFFF
  f32 params[8]
  Color colors[2]         // 32 bytes
  u32 stopOffset, stopCount  // 指向所屬 chunk 的 stop pool（STYL 為 effectStops[]，SCEN 為 stopPool[]）
```

`valueType` 在 Decl 中額外允許兩個值：`ValueType::StringRef = 8`、`ValueType::ListRef = 9`，僅用於樣式宣告，不出現在 Keyframe。

## 2.7 `SCEN` 場景模板

一個區塊含一個場景。

```
uint32 name              // StringId
float  width, height     // 設計尺寸（Lottie 的 w/h）
uint32 nodeCount
SceneNode nodes[nodeCount]      // 索引 = 場景局部 NodeId；index 0 為根（Group）
uint32 pathDataCount
PathData paths[pathDataCount]
uint32 imageDataCount
ImageData images[imageDataCount]
uint32 cameraCount;   IrCameraData cameras[cameraCount]   // T-42c 起
uint32 verbPoolSize;  uint8  verbPool[]   (4 對齊填充)
uint32 floatPoolSize; float  floatPool[]
uint32 stopPoolSize;  GradientStop stopPool[]   (20 bytes each)
uint32 effectCount;   IrEffect effects[effectCount]       // T-42c 起；stopOffset 指向上面的 stopPool

SceneNode (96 bytes):
  uint8  kind            // NodeKind（Group/Path/Image/Mask/Camera；Camera 為 T-42c 起新增）
  uint8  visible
  uint8  blend
  uint8  maskMode
  uint32 parent          // 局部 NodeId，根為 0xFFFFFFFF
  uint32 kindIndex       // Path → paths[]；Image → images[]；Camera → cameras[]；其他 0
  float  opacity
  Vec2   anchor, translate, scale
  float  rotation
  Vec2   skew
  uint32 name            // StringId（Lottie 圖層名，供 API 查詢）
  float  translateZ, anchorZ, rotationX, rotationY   // T-42c 起；3D 變換擴充，2D 節點皆為 0
  Vec3   orientation      // T-42c 起；額外自由軸向，未使用時為 {0,0,0}
  uint8  is3D             // T-42c 起；1 = 此節點參與 3D 變換/合成
  uint8  backfaceHidden   // T-42c 起；1 = 背面朝向攝影機時隱藏
  uint8  _pad3d[2]
  uint32 effectOffset, effectCount   // T-42c 起；指向本區塊的 effects[]；無效果時為 0, 0

IrCameraData (48 bytes，T-42c 起，T-47a 起加大到 48 bytes；kind == Camera 的節點/DOCT 的 <camera>
元素以 kindIndex/cameraIndex 指向此陣列):
  u8  kind                // 0 = 透視, 1 = 正交
  u8  useTarget           // 1 = 朝向 target；0 = 沿自身 -z 軸看
  u8  _pad[2]
  f32 zoom                // 視野縮放（透視）或正交縮放
  Vec3 position            // T-47a 起；攝影機位置（根座標）；省略時為 NaN，由 refreshCameraDefaults() 補
  Vec3 target              // useTarget 時有效
  f32 nearZ, farZ         // 近/遠裁切面
  u32 _pad2
  u32 _pad3               // T-47a 起；保留，湊足 48 bytes

PathData (136 bytes):
  uint32 verbOffset, verbCount
  uint32 pointOffset, pointCount        // floatPool
  uint8  fillRule
  uint8  fillKind, strokeKind           // PaintKind
  uint8  cap, join
  uint8  _pad[3]
  Color  fillColor, strokeColor         // 32
  Vec2   fillP0, fillP1, strokeP0, strokeP1   // 32
  uint32 fillStopOffset, fillStopCount, strokeStopOffset, strokeStopCount   // 16 → stopPool
  float  strokeWidth, miterLimit
  float  trimStart, trimEnd, trimOffset
  uint32 dashOffset, dashCount          // floatPool
  float  dashPhase
  
ImageData (16 bytes):
  uint32 asset
  float  width, height
  uint32 _pad
```

子節點順序：`nodes[]` 依**深度優先、前序**排列，父節點必在子節點之前。同一父的子節點在陣列中的相對順序即繪製順序（先者在下）。讀取器據此重建 `firstChild/nextSibling`。

## 2.8 `TLIN` 時間軸

一個區塊含一個時間軸。

```
uint32 name
float  duration
float  frameRate
uint32 trackCount;    Track    tracks[]
uint32 keyframeCount; Keyframe keyframes[]
uint32 floatPoolSize; float    floatPool[]
uint32 stopPoolSize;  GradientStop stopPool[]
uint32 markerCount;   Marker   markers[]

Track (16 bytes):
  uint32 target        // 場景局部 NodeId 或 kSelfNode
  uint16 prop
  uint8  type          // ValueType
  uint8  _pad
  uint32 kfOffset, kfCount

Keyframe (72 bytes):
  float  time
  Easing easing (20)
  Vec3   tanOut, tanIn // T-42c 起：原 Vec2 擴為 Vec3（z 對 2D/Vec2 軌道恆為 0）
  uint8  hasSpatial
  uint8  _pad[3]
  uint8  value[16]     // 同 Decl 的 value 編碼；Path/Stops/FloatArray 為 u32 offset, u32 count 指向對應 pool
  uint32 _pad2

Marker (12 bytes):
  uint32 name; float start; float duration
```

## 2.9 `DOCT` 文件樹

```
uint32 elementCount
DocElement elements[elementCount]     // 深度優先前序，索引 = 文件局部 ElementIndex
uint32 textCount
DocText texts[textCount]
uint32 attrPoolCount
Attr   attrPool[]                      // (u32 name, u32 value)
uint32 classPoolCount
uint32 classPool[]
uint32 inlineDeclCount
Decl   inlineDecls[]                   // 與 STYL 的 Decl 相同格式
uint32 cameraCount;   IrCameraData cameras[cameraCount]   // T-42c 起，文件層級攝影機

DocElement (48 bytes):
  uint32 parent            // 元素索引，根為 0xFFFFFFFF
  uint32 tag               // StringId
  uint32 id                // StringId 或 kInvalidString
  uint32 classOffset, classCount
  uint32 attrOffset, attrCount
  uint32 inlineDeclOffset, inlineDeclCount
  uint32 scene             // tag == "anim" 時為 SceneId（SCEN 區塊序號），否則 0xFFFFFFFF
  uint32 firstTextChild    // 指向 texts[] 或 0xFFFFFFFF（第一版一個元素最多一個文字子節點，見 05.2）
  uint32 cameraIndex       // T-42c 起（原 _pad 改名）；指向本區塊的 cameras[]，0xFFFFFFFF = 非攝影機選取元素

DocText (8 bytes):
  uint32 parent            // 元素索引
  uint32 text              // StringId
```

### 2.6.1 場景目標選擇器（T-218 #219）

`IrSelPart` combinator `4 = kIrSelSceneTarget`：一律在 `parts[1]`（subject 之後、condition parts 之前）；`classCount` = `IrSceneTarget{Name=0, Tag=1, Any=2}`、`tag` = 名稱或 tag 的 StringId（Any 為 kInvalidString）、`id`=kInvalidString、`stateMask`=0、`classOffset`=0；IrView 驗證並拒絕 index ≥ 2 的 scene part。`IrSceneNode` 新增 `uint8_t svgTag`（自 `_pad3d[2]` 取一 byte；碼表 `IrSvgTag`：0 none、1 path、2 rect、3 circle、4 ellipse、5 line、6 polygon、7 polyline、8 g，`parseSvg` 寫入）。STYL 末尾新增 `floatPool[]`（舊檔缺 → 空）；`d` 宣告 valueType = ListRef，value = u32 pointOffset、pointCount、verbOffset、verbCount（verbs 以 float 存）。`kIrFormatMinor = 2`。

## 2.9a `SRCM` 來源映射（T-42c 起，可選）

```
uint32 fileCount;  uint32 fileNames[fileCount]   // StringId；index 0 固定為主 .xt 檔
uint32 entryCount; IrSourceEntry entries[entryCount]

IrSourceEntry (12 bytes):
  uint32 fileId    // 指向 fileNames[]
  uint32 line      // 1-based
  uint32 col       // 1-based
```

`entries[]` 依序對應 `DOCT` 的 `elements[]` 再接 `texts[]`：`entryCount` 必須等於 `elementCount + textCount`，否則視為無效 IR。缺席時 `IrView::hasSourceMap()` 回傳 `false`。`<link>` 引入的 `.xts` 樣式檔不產生條目（樣式宣告目前不逐條記來源）。

## 2.10 `META`

```
uint32 sourceFile      // StringId
uint32 compilerVersion // StringId
uint64 compileTimeUnix
```

## 2.11 讀寫 API

```cpp
// core/ir.h
struct IrBlob { const uint8_t* data; size_t size; };
struct IrReadOptions { bool verifyCrc = false; };

class IrView {                 // 零複製的區塊索引
public:
    static Result<IrView> open(IrBlob, IrReadOptions);
    std::string_view string(StringId) const;
    uint32_t sceneCount() const;   const SceneChunk& scene(SceneId) const;
    uint32_t timelineCount() const; const TimelineChunk& timeline(TimelineId) const;
    const StyleChunk& style() const;
    const DocChunk& doc() const;
    // 各 Chunk 型別提供 span 存取器，皆已邊界檢查（SceneChunk::cameras()/effects()、
    // StyleChunk::effects()/effectStops()、DocChunk::cameras() 為 T-42c 起新增）
    bool hasSourceMap() const;                    // T-42c 起
    IrSourceEntry sourceOf(uint32_t index) const;  // T-42c 起；index < elementCount 為元素，之後為文字
    std::string_view sourceFileName(uint32_t fileId) const;  // T-42c 起
};

// compile/ir_writer.h（只在 xt_compile 使用）
class IrWriter {
public:
    StringId   intern(std::string_view);
    AssetId    addAsset(...);
    SceneId    addScene(const SceneBuilder&);
    TimelineId addTimeline(const TimelineBuilder&);
    void       setStyle(const StyleBuilder&);
    void       setDoc(const DocBuilder&);
    void       setSourceMap(std::vector<StringId> files, std::vector<IrSourceEntry> entries);  // T-42c 起
    std::vector<uint8_t> finish();
};
```

`IrWriter::finish()` 之後 `IrView::open()` 再 `IrWriter` 重寫，必須產生**位元組相同**的輸出（round-trip 測試，15.2）。

## 2.x `SYMB` 區塊（T-171 #170，2026-09-27）

位置：DOCT 之後、檔案最後；只在有符號實例時寫出；舊讀取器忽略。內容：`u32 count`，每個實例 `{ u32 symbolNameStringId, u32 instanceElement, u32 pathCount, IrSymbolPath{ u32 pathStringId, u32 element }[pathCount] }`。`IrView::symbols()` 讀取並驗證元素索引。IR 次版本未變。

## 2.y `TGRP` 區塊（T-172 #171，2026-09-27）

位置：DOCT 之後（與 SYMB 並列，順序不拘，各出現一次）；舊讀取器忽略。內容：`u32 count`，每群 `{ u32 nameStringId, u32 scopeElement(0=root), f32 duration, u32 markerCount, [f32 t, u32 nameId]…, u32 soundCount, [f32 t, u32 cueId]…, u32 memberCount, [u32 element, u32 timelineId, f32 delay, f32 duration]… }`。成員的 `@keyframes` 正規化為長度 1，所以成員記錄帶自己的 `duration`。IR 次版本未變。

## 2.z `VARS` 區塊（T-170 #172，2026-09-27）

尾端區塊，STYL 之後任何位置、至多一次；只在有 token 或 fallback 時寫出。內容：`u32 tokenCount; IrVar tokens[]`（`IrVar` 24 B：`u32 name StringId, u8 valueType (Float|Length|Color|StringRef 文字), u8 pad[3], u8 value[16]` IrDecl 編碼）、`u32 fallbackCount; IrDecl fallbacks[]`。引用宣告：`IrDecl.valueType = 11 (kDeclVarRef)`，值為 `u32 nameStringId + u32 fallbackIndex`（或 `kNoVarFallback`；編譯器一律提供 fallback，缺者用初始值）。尾端區塊順序：SYMB、TGRP、VARS（各至多一次）。無 token 的文件位元組與之前相同。

- T-173：STYL 的 `IrSelPart` 組合子值 3 = `kIrSelCondition`（條件部件跟在其複合之後；碼在 `classCount`，0x80 = 否定；運算元在 tag/id）；`IrRule::selCount` 含條件部件。舊執行時會把值 3 讀成後代組合子（未升版）。

## 2.w `CALC` 與 `MDIA` 區塊（T-176 #174，2026-09-27）

- **CALC**（尾端、可選）：`u32 n, IrCalc[n]{opOffset, opCount}`（8 B），`u32 m, IrCalcOp[m]{u8 op, u8 unit, u8 hasFallback, u8 pad, f32 value, u32 name}`（12 B）；`CalcOpKind`：PushLength、PushNumber、PushVar、Add、Sub、Mul、Div；數字 fallback 單位碼 `0xFF`。宣告以 `IrDecl::valueType = 12 (kDeclCalc)`、值槽 0 = 程式索引。
- **MDIA**（`@media`；fourcc 只有四字）：`IrMedia{queryOffset, queryCount}`（OR）、`IrMediaQuery{condOffset, condCount}`（AND）、`IrMediaCond{u8 feature, pad[3], f32 value}`、`IrMediaRule{rule, media}`（依 rule 升冪），各 8 B。`MediaFeature`：MinWidth、MaxWidth、MinHeight、MaxHeight（px）、MinAspect、MaxAspect、Orientation（0 = landscape）、Platform、Input。

## 2.v `STAT` 與 `TRIG` 區塊（T-174 #175，2026-09-27）

- **STAT**：`u32 n, u32 names[n]`（自訂狀態名 StringId；bit i = names[i]，≤ 32），`u32 n, IrStateGroup{u32 name, u32 mask}[n]`。
- **TRIG**：`u32 n, IrTriggerTable{element, triggerOffset, triggerCount}[n]`（12 B）；`u32 n, IrTrigger{u8 event, pad3, u32 arg0, u32 arg1, u32 actionOffset, u16 actionCount, u16 headCount}[n]`（20 B）；`u32 n, IrTriggerAction{u8 kind, u8 flags, u16 next, u32 arg0, u32 arg1, f32 arg2, u16 alt, u16 pad}[n]`（20 B）；`u32 n, IrConverterStep{u8 op, pad3, f32 args[4]}[n]`（20 B）。前 `headCount` 個動作是 `;` 分隔的陳述；`next`（then）與 `alt`（else）為觸發內 1-based 索引，`If` 的 `next` 為真分支。
- 事件碼：Hover 1、Leave 2、Press 3、Release 4、Click 5、Focus 6、Blur 7、Check 8、Uncheck 9、Show 10、Hide 11、Port 12、TimelineEnd 13、TimelineMarker 14、StateEnter 15、StateExit 16、Key 20；17–19、21、22 保留。動作碼：Play 1、Stop 2、Seek 3、State 4、Class 5、Show 6、Hide 7、Sound 8、Emit 10、If 11、Focus 12、Haptic 14；9 `set`、13 `spawn`、15 `layer` 保留。旗標：play `kPlayReverse/Loop/Once/Hold` = 1/2/4/8；state／class 用 `TriggerMode` On/Off/Toggle。轉換器 op（27.3.3 目錄序）：Lt 7、Le 8、Gt 9、Ge 10、Eq 11、Ne 12、Between 13——**T-175 必須沿用此編號**。
- `:state(Name)` 是 T-173 條件部件的新碼 `IrSelCond::CustomState = 16`（`tag` 為狀態名 StringId），可用於 `:not()`。
- NodeCommon 尾端：`triggerTable`（表索引 + 1）、`customStates`（u32 bitset；名稱在 STAT）、`flags2`（`kPointerWatch = 1`）。

## 2.u `STRS` 區塊（T-179 #176，2026-09-27）

尾端可選、STYL 之後至多一次。`u32 n, u32 keys[]`（鍵 StringId，不含 `@`）；`u32 n, u32 textKeys[]`（空或每個 DOCT `texts[]` 一筆，1-based 指向 keys，0 = 無）；`u32 n, u32 attrKeys[]`（同法對 `attrPool[]`）；`u32 n, IrFontFallback{lang, familyOffset, familyCount}[]`（12 B）；`u32 n, u32 fallbackFamilies[]`。DOCT 的文字／屬性值存裸鍵（未解析時顯示）；DOCT 格式不變。`IrView::localization()`。

## 2.t DOCT 尾端的富文本表（T-178 #177，2026-09-27）

`cameras[]` 之後：`u32 n, IrTextRun[n]`（32 B：`text, start, end (UTF-8 位元組), declOffset, declCount, inlineObject, link, pad`；依 text 與 start 升冪、不重疊；`inlineObject`／`link` 0xFFFFFFFF = 無）、`u32 m, IrTextInline[m]`（16 B：`kind 1 glyph/2 image, ref (StringId action 或 AssetId), f32 height (0 = run 字級), pad`；物件文字為 U+FFFC）、`u32 k, IrTextLink[k]`（4 B：`onClick` StringId）。只在有 run 時寫出；舊讀取器忽略。

## 2.s `PORT` 與 `BIND` 區塊（T-175 #178，2026-09-27）

- **PORT**：`u32 n, IrPort{u32 name, u8 type, u8 dir, u8 flags(kPortExpose=1), u8 pad, u32 element, u32 scope (實例根或 0xFFFFFFFF), u32 fieldOffset, u32 fieldCount}[n]`（24 B）；`u32 m, IrPortField{u32 name, u8 type, pad3}[m]`（struct/list 欄位與 enum 值名）。`PortValueType`：Number 1、String 2、Bool 3、Color 4、Vec2 5、Vec3 6、Entity 7、Enum 8、Struct 9、List 10、Texture 11、Event 12；`PortDir` In 0、Out 1、Call 2。
- **BIND**：`u32 n, IrBinding{u32 element, u8 target, u8 flags, u16 targetId, u32 targetName, u32 sourceOffset, u16 sourceCount, u16 stepCount, u32 stepOffset}[n]`（24 B）；`u32 m, u32 sources[]`（PORT 索引，輸入 port 在前）；`u32 k, IrConverterStep[]`。`BindTarget`：Prop 1（targetId = PropId/StyleProp、targetName = CSS 名）、Text 2、Src 3、Visible 4、State 5（targetId = 狀態 bit）、Class 6、Variant 7（targetName = `v-<Prop>-` 前綴）；flags `kBindAfterLayout` 1、`kBindNoPrimary` 2。NodeCommon 未加 `bindingTable`（BIND 以元素索引）。
- `IrConverterStep` 仍 20 B：T-174 的 pad 改為 `u8 argCount` + `u16 argKinds`（每引數 4 bit：Number 0、String 1、Port 2、Percent 3、None 4；T-174 的零值讀為全 Number）。op 0 = `Operands`（前一步的續行引數）；宿主自訂 = 0x80 並在 args[0] 帶名，執行時註冊回 0x80+i。TRIG：Port 事件與 Emit 動作的 `arg1` = PORT 索引 + 1（0 = 未宣告）。
- **ConverterOp**：range 1、clamp 2、round 3、abs 4、scale 5、offset 6、lt 7、le 8、gt 9、ge 10、eq 11、ne 12、between 13、not 14、and 15、or 16、format 17、thousands 18、fixed 19、percent 20、time 21、upper 22、lower 23、select 24、step 25、lerpColor 26、fromToken 27、localize 28、concat 29、glyph 30、tween 31、delay 32、count 33、index 34、item 35、elemProp 36。

### 2.s.1 T-200 追加（#198）

- `ConverterOp` 尾端：`x=37`、`y=38`、`z=39`、`w=40`、`Vec=41`（`vec2`／`vec3` 共用 41，`argCount` 2 或 3 決定大小；參數為 None 者取鏈輸入）。
- `BindTarget::Checked = 9`（T-204 #203）：`bind:checked` 僅限 `<input type="checkbox">`，執行期只設清 Checked 位元並標樣式髒，不發 Change、不觸發 on:check/on:uncheck。
- `BindTarget::WorldAnchor = 8`：`world-anchor="<port>"` 編成一筆 BIND（旗標 `kBindAfterLayout`、一個來源、無步驟）；port 非 vec2/vec3 為編譯錯誤。

### 2.s.2 埠預設值（T-358，2026-10-16）

`IrPortField[m]` 之後追加 `u32 d, IrPortDefault{u32 port, u8 type, u8 pad[3], u8 value[16]}[d]`（24 B；`value` 依 `type`：Number f32、Bool u8、Color 4 bytes 依序 R,G,B,A、String u32 StringId、Vec2 2×f32、Vec3 3×f32、Enum u32 名 StringId；其餘型別不可有預設值）。只列有預設值的埠；舊讀取器把缺少的尾段視為空。minor 7。

## 2.r `RPT ` 區塊（T-177 #180，2026-09-27）

尾端可選：`u32 n, IrRepeat[n]`（28 B：`u32 element, u32 listPort (PORT 索引), u32 templateRoot, u32 enterTriggerActions, u32 exitTriggerActions (TRIG 觸發索引 + 1，0 = 無), u8 flags (kRepeatVirtualize=1), u8 pad[3], u32 viewportElement (0xFFFFFFFF = 無)`）。TriggerEvent 新增 ItemEnter 23、ItemExit 24；TriggerAction 新增 `remove` 16（只准在 item-exit）；BIND flag `kBindRepeatItem = 4`（與 kBindNoPrimary 並用）；ConverterArg 新增 Item 5、Index 6、Count 7。`<repeat>` 現在存在於 IR（DOCT 元素 + RPT；3.12.1 的說法更正）；模板元素帶 NodeFlags2 `kTemplate = 2`、`ElementData::templateId`。

- T-194（#192）：TRIG 事件補齊保留碼 DragStart 17、DragOver 18、Drop 19、Gesture 21（arg0 = GestureKind、arg1 = swipe 方向）、Player 22（arg0 = 0-based 玩家、arg1 = 內層事件）；尾端新增 DragLeave 26、Back 27、NavBlocked 28（arg0 = 方向或 any）；動作 Layer 15（flags 1 = overlay）；Haptic 的 arg2 = 強度（舊 IR 0 讀為 1）；`IrSelCond::FocusPlayer = 17`（`tag` = 玩家索引，`kIrSelCondLast = 17`）；StyleProp `focus-nav` 195（繼承）、`layer` 196；`flags2`：kLayerOverlay 4、kDraggable 8、kDropTarget 16、kFocusTrap 32；`nav*` 欄位存 id StringId + 1（值不含 `#`）。

## 2.v `SHDR` 區塊（T-203 #201，2026-09-27）

選用，DOCT 之後最多一個（writer 寫在 RPT 之後）：

```
uint32 shaderCount; IrShaderRef shaders[]   // 112 B: asset, sourceOffset/Length, glslOffset/Length,
                                            //   paramOffset/Count, float expandPx, IrEffect fallback (80 B)
uint32 paramCount;  IrShaderParam params[]  // 32 B: name (StringId), u8 type (0 float / 1 vec2 / 2 color), pad[3],
                                            //   float defaultValue[4], minValue, maxValue (無範圍時皆 0)
uint32 textLength;  uint8 text[]            // UTF-8；source 與 glsl 都是它的切片
```

驗證：asset 必須為 ASST kind 2；各段在 pool 內；`expandPx ≥ 0`；fallback kind 合法、非 Custom、`stopCount == 0`；asset 不重複；param name 在字串表內且 `type ≤ 2`。STYL 的 Custom `shader` 欄只檢查索引範圍（既有 round-trip fixture 以字型資產佔位）。

> T-228（#234）：`IrSceneNode::autoOrient`（原 `_pad3d` 一 byte）；Lottie `ao` 映射至此。T-225a/b（#229、#233）：SCEN 可選尾段 `modifiers[]`（`IrModifierData` 44 B）／`shapes[]`（`IrShapeData` 172 B），`kIrFormatMinor = 3`，`IrSvgTag` 加 `XtStar=9`、`Modifier=10`。

> T-243（#251，32 章）：`kIrFormatMinor = 5`。SCEN 可選尾段再接 `bones[]`（`IrBoneData` 4 B）、`constraints[]`（44 B）、`meshes[]`（32 B）、`skins[]`（20 B）、`tendonPool[]`（28 B）、`weightPool[]`（8 B）、`meshVec2Pool[]`（positions 後接 uvs）、`meshIndexPool[]`（u16），八個陣列全寫或全省；`IrPathData` 加 `skinIndex`（152→156 B）、`IrBinding` 加 `sceneNode`（24→28 B，0xFFFFFFFF = 元素本身），舊檔以 `readCountedWidened` 加寬；`IrSvgTag` 加 `XtSkeleton=11 XtBone=12 XtConstraint=13 XtMesh=14`。

## 2.w 載入驗證補完與上限（2026-10-02 審查修正；T-255 #266）

`IrView::validate` 對下列情況回 `kInvalidIr`（之前會越界、耗盡記憶體或走到未定義行為）。常數在 `core/include/xt/ir_pod.h`：

| 常數 | 值 | 意義 |
|---|---|---|
| `kMaxEffectSequence` | 16 | 一個 `effect`／`text-effect` 清單、或一個 scene 節點的效果範圍的最大長度 |
| `kMaxScenePaintListCount` | 0xFFFF | scene 節點的 paint 數上限 |
| `kMaxElementDepth` | 256（T-255b #318 由 512 降：實測 Debug 下 update 每層約 1,712 B、hit test 624 B，512 層在 512 KiB 執行緒溢位） | 元素樹深度；深度 ≥ 256 的元素使整份 IR 無效 |

新增的無效條件：

- pool-list 屬性（transition、animation、font-family、effect、background-gradient、text-effect）的值型別不是 ListRef 也不是 `var()`。
- Path keyframe 或 STYL 的 `d`，其 verbs 所需的點數與實際點數不相等。
- `data-xt-asset` 的 id 不在 ASST 表內；scene 的 Custom shader 不在資產表內。
- 純 Length 的單位 byte 大於 `UiScale`；text run 未依序排列。
- keyframe 時間或 Seek 時間非有限（NaN／Inf）。
- `.xtcl` 的 `frameCount` 超過檔案能容納的數量（`BakedPlayer::load`）。

編譯器端對應：符號展開後樹深 ≥ 512 時報 error，不會寫出載入器會拒絕的 IR。

執行期 API 可以餵進同樣的值，所以使用點也各自防守：`resolveEffectSequence` 以 scratch 大小為界；`setPathProperty` 忽略 verbs 與點數不符的值；`FileAssetResolver::ensureDecoded` 拒絕表外的 id；`evalTrack`／`Animator::seek`／`Document::seekTimeline` 把非有限時間當 0。

> 行為變更：`evalTrack(+Inf)` 現在回第一個 keyframe（非有限＝0），之前回最後一個。T-255b 會把 `+Inf` 改為夾到結尾，屆時更新本段。

## 2.x 審查修正鏈補記（2026-10-07 審查修正鏈合併；T-255b #318、T-263d #293）

- `kMaxElementDepth` = 256（上表已改）。`appendChild`／`insertBefore` 會到達上限時為靜默 no-op（與其既有前置條件一致）。
- 驗證補充：`font-size` 與漸層單位的範圍檢查；`BakedWriter::writeFrame` 拒絕 index ≥ `frameCount`；時間參數 `+Inf` 表示「到結尾」（取代 §2.w 的「非有限＝0」，見 04 §4.12）；`dt`／speed 的 NaN／Inf 讀為 0。
- **最後閘門的 fuzz 修正（#325 尾端 7066f638、2386c026）**：驗證器拒絕 layout 直接讀取、單位無法解析的 Length（非 Length 型別的宣告其單位 byte 須為 0：AnchorX/Y、TranslateX/Y、LineHeight、perspective 等；`transform-origin: 1em` 由樣式階段換算）；自動播放的 `<anim>` 指向不存在的 timeline 為 kInvalidIr（`Animator::play` 對缺少的 timeline 回 0）；每條 TLIN 軌的值型別須為該屬性接受者（`core/src/property_accept.h` 的共用表，`setProperty` 對不接受的型別不做事）；未定義的軌屬性或不適用於目標節點種類者為 kInvalidIr。迭代通知每個 instance 每次 update 最多 `kMaxIterationNotesPerUpdate` = 64 則，其餘合併為一則（`iteration` = 最後到達的索引，`Note::skipped` 記略過數）；marker 通知以 marker 清單為界。迴歸輸入在 `tests/fuzz/regressions/ir/`。
- 177 個畸形 IR 案例未再發現驗證器遺漏；205 個語料樣本經 `IrView` 讀出再以 `IrWriter` 寫回逐 byte 相同（15 §15.6，T-267c #322）。
- IR 格式與版本在本鏈未變；`CommandList::sources`（09 §9.8）不序列化。
