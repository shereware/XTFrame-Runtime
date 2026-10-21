# 09 · 繪製命令清單（CommandList）

CommandList 是核心與後端之間**唯一**的介面。引擎整合者只需要消費這個結構。所有座標為邏輯像素，變換矩陣已含節點世界變換；後端只需再乘上自己的投影。

## 9.1 結構

```cpp
namespace xt {

enum class CmdKind : uint8_t {
    FillPath      = 0,
    StrokePath    = 1,
    DrawImage     = 2,
    DrawGlyphs    = 3,
    PushClipRect  = 4,
    PushClipPath  = 5,
    PopClip       = 6,
    PushLayer     = 7,   // 離屏群組：不透明度 / 混合模式
    PopLayer      = 8,
    PushMask      = 9,   // 之後直到 EndMaskContent 是遮罩內容
    EndMaskContent= 10,  // 之後直到 PopMask 是被遮罩內容
    PopMask       = 11,
};

struct Command {
    CmdKind  kind;
    uint8_t  _pad[3];
    uint32_t index;        // 指向對應的 payload 陣列
};

struct PathRef  { uint32_t verbOffset, verbCount, pointOffset, pointCount; };

struct PaintRef {
    PaintKind kind;
    Color     color;                 // Solid，或漸層時作為乘數（一律 (1,1,1,alpha)）
    Vec2      p0, p1, p2;   // linear：p0→p1；radial（T-116 起）：p0 圓心、p1 = 圓心 + (rx, 0)、p2 = 圓心 + (0, ry)；p2 == p0 時為半徑 |p1−p0| 的正圓（舊行為）
    uint32_t  stopOffset, stopCount; // → CommandList::stops
};

struct FillCmd   { Mat2x3 xf; PathRef path; PaintRef paint; FillRule rule; uint8_t antialias; };
struct StrokeCmd { Mat2x3 xf; PathRef path; PaintRef paint; float width; LineCap cap; LineJoin join;
                   float miterLimit; uint32_t dashOffset, dashCount; float dashPhase; };
struct ImageCmd  { Mat2x3 xf; AssetId asset; Rect dst; Rect srcUv /*0..1*/; float opacity; uint8_t smooth; };
struct GlyphsCmd { Mat2x3 xf; FontId font; float size; Color color;
                   uint32_t glyphOffset, glyphCount; };   // → glyphs / glyphPos
struct ClipRectCmd { Mat2x3 xf; Rect rect; };            // rect 於 xf 座標系（可旋轉）
struct ClipPathCmd { Mat2x3 xf; PathRef path; FillRule rule; };
struct LayerCmd  { float opacity; BlendMode blend; Rect boundsHint; };   // boundsHint 為世界座標 AABB；T-133 起 Painter 填「內容 AABB 夾到視口」（同 EffectCmd，18.1 的 subtreeBounds；3D 下取投影四角，投影失敗才退回全視口），後端據此配置離屏緩衝
struct MaskCmd   { MaskMode mode; Rect boundsHint; };

struct CommandList {
    std::vector<Command>     commands;
    std::vector<FillCmd>     fills;
    std::vector<StrokeCmd>   strokes;
    std::vector<ImageCmd>    images;
    std::vector<GlyphsCmd>   glyphRuns;
    std::vector<ClipRectCmd> clipRects;
    std::vector<ClipPathCmd> clipPaths;
    std::vector<LayerCmd>    layers;
    std::vector<MaskCmd>     masks;
    // 池
    std::vector<PathVerb>    verbs;
    std::vector<float>       points;
    std::vector<GradientStop> stops;
    std::vector<uint32_t>    glyphs;       // glyph id
    std::vector<Vec2>        glyphPos;     // 每個 glyph 的基線原點（xf 座標系）
    std::vector<float>       dashes;
    Rect viewport;                         // 本幀邏輯視口
    float devicePixelRatio;
    void clear();                          // 清空所有 vector，保留容量
};
}
```

## 9.2 語意

- **順序**：命令依 `commands` 順序執行，先者在下。
- **座標**：`xf` 把命令自己的座標（路徑點、glyph 位置、`dst` 矩形）映射到視口邏輯座標。後端負責乘 `devicePixelRatio`。
- **Clip 堆疊**：`PushClipRect/PushClipPath` 與 `PopClip` 配對，巢狀取交集。Layer 與 Mask 不重設 clip。
- **Layer**：`PushLayer` 表示其內容先畫到透明離屏緩衝，`PopLayer` 時以 `opacity` 與 `blend` 合成回上一層。`opacity == 1 && blend == Normal` 時 Painter **不會**發出 Layer（直接把 opacity 乘進顏色不成立於有重疊子圖的群組，所以只有這個條件才省略）。
- **Mask**：`PushMask` → 遮罩內容 → `EndMaskContent` → 被遮罩內容 → `PopMask`。後端把兩段各畫到離屏緩衝，依 `mode` 合成。`Add/Subtract/Intersect` 模式的多重遮罩由 Painter 展開為巢狀 Mask 命令（見 9.4）。
- **Paint 顏色**：非預乘。漸層 stop 顏色亦非預乘；`PaintRef::color.a` 為整體 alpha 乘數。
- **antialias**：Painter 一律設 1；保留欄位給烘焙工具。
- **Glyph**：後端以 `(font, glyph id, size)` 向 `FontProvider` 取字形外框或點陣（見 07）。`glyphPos` 是每個字形的基線左端。

## 9.3 Painter 遍歷演算法

```
paintNode(n):
    若 !visible[n] 或 worldOpacity[n] <= 0：return
    若 kind == Element：
        若 style.overflow != visible：PushClipRect(world, paddingBox)   // 見 06
        emitElementBackground(n)      // 背景色 + 圓角 → FillPath；邊框 → FillPath（外框減內框，EvenOdd）
        emitElementImage(n)           // 若 tag == "img"
        遍歷子節點（含 Text 節點 → DrawGlyphs，含場景根）
        若有 clip：PopClip
        return
    需要 Layer = (opacity[n] < 1 且有 ≥2 個可見子孫繪製項) 或 blend != Normal
    若需要：PushLayer(opacity, blend)
    若 kind == Path：emitPath(n)（opacity 若未走 Layer 則乘進 paint.color.a；trim 於 Painter 暫存區重算，見 03.4 註）
    若 kind == Image：DrawImage
    若 kind == Group：
        遍歷子節點；遇到 Mask 節點：
            PushMask(mode)；遍歷 Mask 子樹；EndMaskContent；
            遍歷其後兄弟直到下一個 Mask 或結束；PopMask
    若需要：PopLayer
```

Element 的 `z-index`：同一父下的 Element 子節點依 `(z-index, 文件順序)` 穩定排序後遍歷。場景節點不參與 z-index。

## 9.4 多重形狀遮罩展開

Lottie 圖層若有多個 `masksProperties`，編譯器產生多個連續 Mask 節點。Painter 遇到連續 Mask 節點序列 `M1..Mk`：

- 所有為 `Add`：合併為單一 `PushMask(Alpha)`，遮罩內容依序畫 M1..Mk 子樹。
- 含 `Subtract/Intersect`：巢狀：`PushMask(M1.mode)` ... `EndMaskContent` `PushMask(M2.mode)` ... 每層各自 `PopMask`，內容在最內層。後端對 Subtract 以 `1 - mask` 處理、Intersect 以乘法處理。

## 9.5 引擎整合最小實作

引擎後端**最少**要支援：`FillPath`（NonZero + Solid）、`PushClipRect/PopClip`、`DrawGlyphs`、`DrawImage`。其餘可退化：

| 命令 | 允許的退化 |
|---|---|
| StrokePath | 由 `xt::strokeToFill()`（core 提供，見 10.1）轉成 FillPath |
| 漸層 | 取中間 stop 顏色當 Solid |
| PushLayer/PopLayer | 忽略 layer，改把 opacity 乘進內部每個 paint（結果在重疊處略差） |
| Mask | 忽略遮罩內容，直接畫被遮罩內容 |
| ClipPath | 以其 AABB 當 ClipRect |

`xt_core` 提供 `CommandListSimplifier`：把 CommandList 就地降級為上述最小子集，引擎可直接使用。

## 9.6 序列化（烘焙用）

`xtframe-bake` 輸出的逐幀串流即把 `CommandList` 各 vector 以「count + 元素」依序寫出，前置 `"XTCL"` 魔數與 frame index。格式細節見 12.5。

## 9.x T-160 修訂

節點帶效果時，包住 `PushEffect` 的 `LayerCmd.boundsHint` 應等於 `EffectCmd.boundsHint`（內容 AABB + 外擴），不只是內容 AABB（否則 opacity < 1 的薄元素光暈被裁）。子孫效果的外擴是否計入父層 Layer 待決（16.3）。

- T-178：`GlyphsCmd` 尾端新增 `GlyphEffect effect (None/Dilate/Blur)`、`pad[3]`、`float effectRadius`；painter 對每個效果各發一個命令（後列先畫、在填色之下），shadow 以 transform 平移 (dx, dy)。

## 9.7 T-202（#202）：`DrawParticles`

`CmdKind::DrawParticles = 19`，payload `ParticlesCmd{xf, texture, blend, instanceOffset, count}` + `ParticleInstance{x, y, w, h, rot(rad), rgba premul, uv0, uv1}` 兩個池；Painter 在元素於子節點順序中的位置發出；bounds、drawable 計數、layer/effect 邊界、dump、雜湊、damage 皆涵蓋。`SimplifyOptions::particles`（預設 true）移除並以 `SimplifyStats::droppedParticles` 累計粒子數；flatten3D 在投影內一律丟棄。SW：每粒子一個旋轉 quad（貼圖 bilinear 或純色），normal 與 additive（加總後 clamp）。

## 9.8 來源表、結構驗證與烘焙限制（2026-10-07 審查修正鏈合併；T-263a #282、T-263d #293、T-263e #297、T-263f #303）

**結構驗證**：交給後端的清單必須結構正確：`Command::index` 在其 kind 的 pool 內；每個 offset + count 範圍在 pool 內（64 位元計算）；路徑 verbs 已知且點數足夠；PopClip／PopProjection 不對空堆疊；3D transform 只在 projection 內；Layer／Mask／Effect 區段正確巢狀（`EndMaskContent` 每個 mask 一次）；結尾所有堆疊為空；`source` 不越界。`validateCommandList()` 不配置；`SwRenderer::render`（不畫）、`gpu::FrameBuilder::build`（空幀＋`lastError`）、`CommandListSimplifier::simplify`（原樣交回）、`BakedPlayer::frame`（無效幀讀為空）呼叫它，除非 `CommandList::structureChecked` 已設（Context 的輸出、`BakedPlayer` 已檢查的幀、simplifier 的輸出——後者以 96 份 golden 文件 × 512 種選項全部通過驗證為依據）。`Context::render` 在 Debug 以 XT_ASSERT 驗證自己的輸出。

**來源表**（多份文件同一幀各自解析資產；T-263c 測出三個後端都會拿錯資產 0）：
- `struct CommandSource { const IrView* ir; uint32_t loadSerial; }`、`std::vector<CommandSource> CommandList::sources`；不序列化，`clear()` 清空。
- `ImageCmd`、`ImageQuadCmd`、`ParticlesCmd`、`ImageMeshCmd`、`EffectCmd`（其 EffectInstance 的 shader 屬此來源）、`ShaderRef` 各加 `uint16_t source = 0`（`EffectCmd` 64→68 B、`ShaderRef` 120→124 B，其餘用既有 padding；`ParticlesCmd` 位置式初始化的欄位順序改變）。`ShaderTable::copyTo(asset, source, out)` 以 (source, id) 去重。
- 語意：`sources` 為空 ⇒ 每個命令的來源是後端 render／build 呼叫傳入的 IrView（烘焙清單、手工清單、單文件宿主）；非空 ⇒ `source` 索引該表，呼叫的 `ir` 引數對資產解析無作用（可為 null）。`Context::render` 每畫一份文件就加一項（含單文件、含隱藏 surface），Painter 蓋上索引；`Document::paint` 直接呼叫時不填。`xt::sourceIr(list, source, callIr)` 是唯一解析點。
- 生命週期：表內指標只到下一次 `Context::update()` 或該文件卸載／重載為止；宿主在建立的那一幀渲染。damage 基準清單的表在交換後立即清空，不會被解參考。
- simplifier 全部 pass 保留 `source`（flatten3D 的 quad 繼承圖片的）；damage 雜湊含 `source`；`dumpCommandList` 在表非空時印 ` source=<n>`；`xtframe-view --dump-commands` 同。

**清單標頭與就地簡化**：`simplify(const CommandList& in, CommandList& out, options, shaper)`（就地版呼叫它再交換）；`copyListHeader` 帶 viewport、dpr、`time`、`sources`，`frameSerial` 為 0，`structureChecked` 由 simplify 決定；效果保留時 `shaders`／`shaderParams`／`shaderText` 一併帶出（自訂效果不再把 `xtTime` 讀成 0）。要局部重繪的宿主用不改原清單的形式並把原清單交回 `render()`（23 §23.y）。

**damage 基準**：`CommandList::frameSerial`（不序列化，`clear()` 歸零）標記 Context 上次渲染的清單；`Context::render(out, &damage)` 只對它自己上次產生的清單比對，其他清單或未標 `structureChecked` 者回 `full`。宿主每幀交回同一份未動過的清單。

**烘焙**：`.xtcl` minor 4 不能存 `DrawParticles`、`DrawImageMesh`（也遺失 `PackedGlyphsCmd` 的文字效果欄位）：`BakedWriter::writeFrame` 拒絕並以 `BakedWriter::lastError()` 回 `baked frames cannot store <CommandName> commands yet`，`xtframe-bake` 以 1 結束、不留檔；`sources` 多於一項亦拒絕。納入這些資料是格式變更（minor + 1），待排程（BACKLOG D）。
