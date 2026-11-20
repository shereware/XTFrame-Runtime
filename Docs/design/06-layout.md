# 06 · 佈局

## 6.1 範圍

支援：`display: block | flex | none`、`position: static | relative | absolute`、盒模型（`box-sizing` 固定為 `border-box`）、單行文字與自動換行文字、`overflow`、`min/max` 約束。

不支援：inline 混排、float、grid、`table`、margin collapsing、`position: fixed`、書寫方向。

## 6.2 資料結構

```cpp
struct Edges { float top, right, bottom, left; };

struct LayoutBox {
    // 相對於父元素**內容盒**左上角（未含父的 scroll 與 transform）
    float x, y;
    float width, height;         // border-box 尺寸
    Edges margin, padding, border;   // 已換算為 px
    // 派生（快取）
    Rect paddingBox() const;     // 相對自身 border-box 左上
    Rect contentBox() const;
    // 文字量測快取
    float intrinsicWidth, intrinsicHeight;   // 不換行的內容尺寸（img/anim/文字）
    // 滾動
    float scrollWidth, scrollHeight;   // 內容溢出範圍
};
```

Element 的 `NodeCommon::translate` 於佈局後設為 `(box.x + cssTranslate.x, box.y + cssTranslate.y)`，`anchor` 設為 `transform-origin` 換算後的 px 值，見 6.7。

## 6.3 進入點

```cpp
void LayoutEngine::layout(Document&, Rect viewport);
```

根元素（`body`）的可用尺寸 = viewport 尺寸，`width/height: auto` 時 `body` 的 width = viewport.w，height = 內容高度（**不**自動填滿 viewport）。若要填滿，樣式表寫 `body { height: 100% }`，`%` 相對 viewport。

每個 `kDirtyLayout` 的最上層子樹重新佈局。第一版：**任一元素 dirty 即從根重排**（簡化；效能優化留後續）。

## 6.4 長度解析

```
resolve(Length len, float reference, float fallback):
    Px      → len.value
    Percent → reference * len.value / 100（reference 未定時 → fallback）
    Em      → 已於樣式階段換算，不會出現
    Auto    → fallback（呼叫端決定；width 的 auto 見各演算法）
```

`width/min-width/max-width/padding/margin/left/right` 的 `%` 參考 = 包含塊**內容盒寬**。`height/min-height/max-height/top/bottom` 的 `%` 參考 = 包含塊**內容盒高**，但只在該高度為**確定值**時成立（T-74 修訂；原設計以寬為參考，`height: 100%` 因此無法填滿視口）：確定值 = 視口高（對根的子元素）、包含塊自身 `height` 為 px 或可解析的 %、absolute 由 `top`+`bottom` 決定的高、flex 容器在交叉軸 stretch 後的高；包含塊高度為 auto（由內容決定）時，`height` 的 `%` 視為 `auto`，`min/max-height` 的 `%` 視為未設定（同 CSS 2.1 §10.5）。`margin: auto` 在 block 中水平置中，在 flex 中吸收剩餘空間（第一版：flex 只支援主軸方向 `auto` margin 吸收）。

## 6.5 Block 佈局

`layoutBlock(elem, availW, availH)`：

1. 計算 `margin/padding/border` px 值。
2. `contentW`：`width` 非 auto → resolve 減去 padding/border；auto → `availW - marginL - marginR - padding - border`。套 min/max。
3. 遍歷 `position != absolute` 且 `display != none` 的子元素，依序垂直堆疊：
   - 子元素以 `layoutChild(child, contentW, remainingH)` 佈局，得到其 border-box 尺寸。
   - `child.x = child.margin.left`（若 child 水平 margin 皆 auto：`(contentW - child.width)/2`）。
   - `child.y = cursorY + child.margin.top`；`cursorY += margin.top + child.height + margin.bottom`。
4. 文字子節點（若有）：呼叫 6.8 的文字佈局，得到高度並加到 cursorY（直排時以 contentH 為主軸可用長度，寬度取欄數 × lineHeight）。
5. `contentH`：`height` 非 auto → resolve；auto → `cursorY`。套 min/max。
6. `elem.width = contentW + padding + border`，同理 height。
7. `scrollHeight = max(cursorY, contentH)`。
8. 絕對定位子元素於 6.6 處理。

## 6.6 Absolute 定位

包含塊 = 最近的 `position != static` 祖先的 **padding box**，否則 root。在父元素完成自身尺寸後處理：

```
對 absolute 子元素 c：
    cbW, cbH = 包含塊 padding box 尺寸
    寬：
      width 非 auto → w = resolve(width, cbW)
      否則 left、right 皆非 auto → w = cbW - left - right - margins
      否則 w = intrinsic（shrink-to-fit：先以 cbW 為 availW 佈局取 intrinsicWidth，夾在 [0, cbW]）
                     `display: flex` 容器的 intrinsicWidth（T-89）：row = Σ 子項 outer 主軸尺寸（flex-basis/width 或內容寬）+ gap 總和 + 自身 padding/border；column = max 子項 outer 寬 + padding/border；wrap 時視為單行量測；子項 `flex-shrink` 在量測階段不作用
    x：
      left 非 auto → x = left + marginL
      否則 right 非 auto → x = cbW - right - w - marginR
      否則 x = 靜態位置（父內容盒中若為 static 時的 y；第一版取 0）
    高與 y 同理（top/bottom）：height 非 auto → resolve(height, cbH)；否則 top、bottom 皆非 auto → h = cbH - top - bottom - margins；
      否則 h = 內容高（以已決定的 w 佈局子樹後的 cursorY，含 padding/border；T-74 修訂：不得為 0）
    以 (w, h 或 auto) 對 c 執行 layoutChild
    c.x, c.y 轉為相對父內容盒（減去父 padding 偏移）
```

`position: relative`：正常流佈局後，`x += left（或 -right）`、`y += top（或 -bottom）`，不影響兄弟。

## 6.7 變換整合

佈局後對每個 Element：

```
anchorPx = (resolve(originX, box.width), resolve(originY, box.height))
common.anchor    = anchorPx
common.translate = (box.x + cssTranslate.x + anchorPx.x, box.y + cssTranslate.y + anchorPx.y)
```

`cssTranslate` 是 PropId 3/4 的**樣式基底值**，而 `common.translate` 是最終寫入欄位。為避免 Animator 與 Layout 對同一欄位競寫，Element 的 `Translate/Anchor` PropId 寫入的是 `ElementData::cssTranslate / cssAnchor`，由佈局階段合成到 `NodeCommon`。此規則是 03.11 `setProperty` 對 Element 的特例。

子元素的世界變換 = 父的世界變換 × 父的 `scroll` 平移（`-scrollX, -scrollY`，僅 overflow: scroll）× 父的 padding 偏移 × 子的 local。

## 6.8 文字佈局

輸入：文字節點、可用寬度 `availW`（直排時為可用高度 `availH`）、`ComputedStyle`（font、size、line-height、letter-spacing、text-align、white-space、text-overflow、direction、writing-mode、text-orientation、lang）。

```
res = shaper->shape(text, params)                 // 07.2：runs（邏輯順序）、glyphs、breakAfter、graphemeStart
主軸長度 mainAvail = 水平時 availW；直排時 availH
run 的主軸 advance：水平或 rotated run 用 Σ xAdvance；upright run 用 Σ yAdvance
若 white-space == nowrap：單行（僅在 breakAfter == 2 處必斷）
否則貪婪斷行（邏輯順序）：
    逐 run、逐 glyph 累積 advance；記住最後一個 breakAfter >= 1 的 cluster 位置
    超過 mainAvail 時於最後斷行機會切；同一 run 內可切（產生兩個 GlyphRun）
    若單一片段超過 mainAvail 且無斷行機會，強制於字素邊界（graphemeStart）切
    breakAfter == 2 一律切
每行：
    UAX#9 L2 重排：level 最高到最低奇數 level，逐層反轉「連續 level >= 該層」的 run 序列
    行長 = Σ run advance；行交叉尺寸 = resolve(lineHeight, fontSize)
    基線 = 行頂 + (lineHeight - (ascent + descent) * size) / 2 + ascent * size（以行內最大字型度量）
    text-align：start → 依 direction（ltr 為 left，rtl 為 right）；end 相反；left/center/right 固定
    text-overflow: ellipsis 且 nowrap 且行長 > mainAvail：從**視覺末端**（LTR 右端、RTL 左端）移除字素直到放得下 U+2026
直排（writing-mode 非 horizontal-tb）：
    行 → 欄：欄寬 = lineHeight；vertical-rl 第一欄 x = 節點寬 - 欄寬，往左；vertical-lr 從 0 往右
    欄內 run 由上往下；upright run 每個 glyph 依 yAdvance 前進，xOffset/yOffset 套用；rotated run 以欄中心線為軸順時針旋轉 90°，
    GlyphRun::rotated = 1，Painter 為該 run 產生 rotationDeg(90) 的變換
輸出：TextNodeData::runs、lines；intrinsic 主軸尺寸 = 不換行時最大行長；交叉尺寸 = 行數 × lineHeight
```

節點盒尺寸：水平時 `intrinsicWidth = 最大行長`、`height = 行數 × lineHeight`；直排時 `width = 欄數 × lineHeight`、`intrinsicHeight = 最大欄長`。文字節點的位置一律為父內容盒左上，因此 `vertical-rl` 的第一欄位於**文字節點自身寬度**的右端，而非父元素內容盒右端（第一版限制；要靠右需父元素 `text-align`/寬度配合，待 0.2 的邏輯屬性）。**元素的區塊佈局不因 writing-mode 改變**（第一版限制，見 07.1）。

## 6.9 Flex 佈局

實作 CSS Flexbox 的**單線與多線**演算法，省略 `order`、`align-content: space-*` 以外的細節依 CSS 規範 §9 步驟：

```
layoutFlex(container, availW, availH):
  1. 決定主軸/交叉軸（flex-direction）；mainAvail、crossAvail 由容器內容盒決定（auto 高度容器的 crossAvail 未定）
  2. 對每個 flex item（非 absolute、非 none）：
       flexBasis = basis 非 auto ? resolve(basis, mainAvail)
                 : (主軸尺寸屬性非 auto ? resolve(width|height) : 內容尺寸)   // 內容尺寸：以 mainAvail 為可用寬佈局一次取 intrinsic
       hypotheticalMain = clamp(flexBasis, min, max)
       outerMain = hypotheticalMain + 主軸 margin
  3. 分線：nowrap 全部一線；wrap 依 outerMain 累加超過 mainAvail 時換線（gap 計入）
  4. 每線解析彈性長度（規範 §9.7）：
       free = mainAvail - Σouter - gaps
       free > 0 → 依 flex-grow 比例分配（grow 總和 0 則不分配）
       free < 0 → 依 flex-shrink × flexBasis 加權縮小
       凍結違反 min/max 的項目，反覆直到無違反（最多 items 次）
  5. 交叉尺寸：每項以其最終主尺寸為可用寬（row）或高（column）佈局一次，取交叉尺寸；
       align-items/self == stretch 且交叉尺寸屬性 auto → 之後拉伸到線高
  6. 線交叉尺寸 = 該線項目最大 outerCross；單線且容器交叉尺寸已定 → 線高 = 容器交叉尺寸
  7. align-content 分配多線於交叉軸（stretch 平分剩餘）
  8. 主軸位置：justify-content；主軸 auto margin 先吸收剩餘（每個 auto margin 平分）
  9. 交叉軸位置：align-self（auto 取 align-items）
 10. 容器 auto 尺寸：主軸 = Σ線最大主長 + gaps（nowrap）；交叉 = Σ線高 + gaps
 11. row-reverse / column-reverse：主軸位置反向
```

每個 item 的 `layoutChild` 可能被呼叫兩次（量測、最終）；文字 shaping 結果以 `(text, font, size, availW)` 為鍵快取於 TextNodeData，避免重複 shaping。

## 6.10 `img` 與 `anim` 的固有尺寸

- `img`：固有 = 圖片像素尺寸（由 `AssetResolver::imageSize` 提供；未載入時 0×0）。`width/height` 只給一個時另一個依比例。
- `anim`：固有 = 場景設計尺寸。內容盒確定後，場景根 Group 設 `scale = fit(objectFit)`、`translate` 置中（contain/cover）或拉伸（fill）。

## 6.11 測試基準

佈局結果以 JSON 快照比對（15.3）：每個元素輸出 `id/tag, x, y, w, h`。測試案例至少涵蓋：block 堆疊、百分比寬、absolute 四邊、flex row/column、wrap、grow/shrink、min/max 凍結、justify 六種、align 四種、文字換行、ellipsis、RTL 段落對齊、雙向重排（阿拉伯與拉丁混排）、直排 vertical-rl 兩欄、mixed 方向的 CJK+拉丁混排。

## 6.12 T-160 修訂（2026-09-25）

沒有 relayout 的幀，帶 `kDirtyTransform` 的 Element 仍要在佈局階段重新折算 anchor/`translate`（`foldElementTranslate`/`refreshDirtyTransforms`）；否則只有 transform 的 CSS 動畫不會移動元素（bench BUG-6 主因）。`perspective` 被動畫改動時 `perspectivePx` 同類問題待決（16.3）。`FileAssetResolver::imageSize` 改讀 PNG/JPEG/GIF/BMP 檔頭（依 AssetId 快取），不解碼；首幀 update 32 → 6 ms（解碼移到第一次 render）。

## 6.13 T-176：`LengthEnv`（#174）

多數長度在佈局與繪製時經 thread-local `LengthEnv`（`Document::update/paint` 以 `ScopedLengthEnv` 設定；`currentLengthEnv`、`lengthToPxEnv`、`evalCalc`）解析；`font-size` 在樣式時轉 px，用到裝置單位的元素被追蹤、裝置變更時標 `kDirtyStyle`；視口、安全區、uiscale 任一變更 → 全樹 `kDirtyLayout`。安全區以視口比例表示（上下對高、左右對寬）。

- T-178：整段先 shaping 取得斷行與字素資料，再逐 run 以各自樣式 shaping，行可跨 run 斷；每行高度與基線取該行最大的 run；行內物件為基線對齊盒（高 = `height` 或字級，圖片保持比例）。run 分開 shaping 會失去跨 run 的 bidi 脈絡；直排時行內物件填滿欄格。

## 6.14 T-201 修訂（#199）

- flex 列中 `width: auto` 的 block 子項取 **max-content**（新增 `LayoutEngine::maxContentWidth`），同 CSS。
- 絕對定位 `width: auto` 取 **shrink-to-fit**（夾在包含塊內；`white-space: nowrap` 例外），取代 T-74 part B 的做法。
- `<input>` 內允許裝飾用子元素佈局；checkbox 點子元素也切換勾選。
- 邊框環的內輪廓改為逆時針並以 nonzero 填色，修掉單邊 border 在 flex 容器與按鈕上的多餘側線（09 painter）。

## 6.14.1 T-211（#210）：百分比 translate

§6.7 fold 加 `cssTranslatePct × 0.01 × border box`（`layout_engine.cpp` `foldElementTranslate`）；寫入目標 `ElementData::cssTranslatePct`；規則見 05 章尾註。

## 6.15 Grid 子集（2026-10-14 使用者裁定「好」；M32 T-338 佈局、T-339 編輯器）

§6.1「不支援 grid」改為**支援固定軌道的子集**；目標是「完全不懂 CSS 的人能用」，所以語言面保守、編輯器面做足。

- **容器**：`display: grid`。軌道 `grid-template-columns`／`grid-template-rows`：`<px> | <%> | <n>fr | auto | repeat(<n>, <track>…)`；不做 `minmax()`、`auto-fit/auto-fill`、`subgrid`、`masonry`、隱式軌道大小（`grid-auto-*` 固定 `auto`）。`gap`／`row-gap`／`column-gap` 同 flex。對齊：`justify-items`／`align-items`（start | center | end | stretch，預設 stretch）；`justify-content`／`align-content`（**stretch（預設）** | start | center | end | space-between | space-around，作用於軌道整體；stretch = 該軸尺寸確定且無 `fr` 軌吸收剩餘時，剩餘空間平均分給 `auto` 軌，CSS Grid §12.8；2026-10-15 T-338 實作後補）。
- **區域**：`grid-template-areas: "a a b" "c . b"`（`.` 為空格；區域必須是矩形，否則整個宣告警告丟棄）；子項 `grid-area: <name>`。這是美術的主要用法：畫布上以名字顯示格子。
- **放置**：`grid-column`／`grid-row`：`<start> [/ <end>]`、`span <n>`；未放置的子項依 row-major 自動填第一個空格（不做 dense）。`position: absolute` 子項脫離格線（同 flex）。
- **量測**：px／% 直接；`fr` 分配剩餘；`auto` 軌道 = 該軌道內子項最大 max-content（一次量測，不做 CSS Grid 的多輪 intrinsic 算法）；內容超出格子時照 overflow 規則。子項的 width/height 為 auto 時填滿格子（stretch）或依對齊取固有尺寸。
- **成本**：固定軌道一次量測；只有 `auto` 軌道對子項多一次 max-content 量測。無每幀成本。
- **匯入**：24 章的 2D grid 降級改為直接對映；1D→flex 轉換只在「單列（或單欄）且子項數 ≤ 軌道數（不會產生隱式軌）」時保留，否則輸出真 grid（T-338 裁定）；`minmax`／`auto-fit` 等不支援的值 → 整個容器降回 block 並警告（現行為）。
- **編輯器（T-339，17 章同步）**：容器「切成格線」命令；畫布格線覆蓋層（拖分隔線改軌道、雙擊加欄列、拖子項進格子＝放置、拖格子邊角＝span、框選幾格取名＝區域）；屬性面板只顯示「3 欄 × 2 列、間距 8」與區域清單，不露 CSS；版型模板（30 part 7）可存格線；`<repeat>` 綁到格線容器自動填格。
- **測試**：`Tests/layout` 快照對照 Chrome（16 §16.4），案例：px/fr/auto 混合、repeat、areas、span、自動放置、gap、對齊、absolute 子項。

## 6.16 `aspect-ratio`（2026-10-14；M32 T-328）

`aspect-ratio: auto | <w> / <h> | <number>`（Float = w/h，0 = auto，可動畫，不繼承）。規則：只有一軸為確定值（px／可解析 %／flex 主軸算出的尺寸／absolute 由兩邊決定）而另一軸 `auto` 時，另一軸依比例推導；兩軸都確定時忽略；兩軸都 auto 時 block 以寬為確定軸、flex 項目以主軸為確定軸。推導值再經 min/max 夾限，被夾時以該軸重算另一軸**一次**（CSS Sizing 4 §5.1 的單次版本，不迭代）。flex 項目交叉軸為 auto 且 `align-self` 為 `stretch`（預設）時，**stretch 優先、比例被忽略**（CSS Sizing 4 §5.1 與 Flexbox §9.4，Chrome 行為；2026-10-14 修正，原稿寫反）；`align-self` 非 stretch 或交叉軸確定時才依比例。被夾限時只有「確定軸本身是自動尺寸（填滿、固有、shrink-to-fit）」才回推另一軸；作者明寫的 px 不改（CSS 2.1 §10.4）。替換元素（§6.10 `img`／`anim`／`video`／`sequence`）`auto` = 固有比例，明確值覆蓋固有比例。成本：一次乘除，無新迴圈。測試：`Tests/layout` 快照對照 Chrome。

## 6.x 版面簡寫 `anchor` / `offset` / `offset-end` / `size`（2026-11-05；54 §54.5.1；T-588 #565）

純編譯期糖，只准在 v2 `<layout>` 區段：展開對照表與 `size: fill` 三種情境（UI 根 `100vw`／`100vh`、自由子項 `100%`、堆疊子項 `flex-grow: 1; flex-basis: 0`／`align-self: stretch`）見 54 §54.5.1；實作與細則見 54 章 T-588 實作記錄。佈局引擎不認識這四個名字——它們到不了 IR。

## 6.x 排版邊界與局部重排（2026-11-18 定案；修訂 §6.3「第一版：任一元素 dirty 即從根重排」）

**量測**（v0.18.0，Sherefox `debate`）：每幀改一個文字 → update 0.058 → 0.261 ms，全部來自排版（laidOut 0 → 34＝整棵顯示中的樹；styled 仍為 0）；改 1 個或 5 個都是 34。原因：`setText` 只標該節點的 layout 髒位元，但髒位元一路傳到根，`LayoutEngine::layout` 見根髒就整份文件重排，再對整棵樹做絕對定位與變換。`NodeArena` 早已留了 `stopsLayoutPropagation()` 這個掛鉤，但它恆回 false。

**規則**：

1. **排版邊界**：一個元素若同時滿足下列條件，它內部的排版改變不會影響它自己的外框，因而不影響祖先與兄弟：
   - 寬與高都是**確定的**：以長度指定（含 `calc()` 不含百分比者），或是絕對定位且兩軸各由「尺寸」或「兩側 inset」決定；百分比只在包含塊該軸確定時算確定。`min-`／`max-` 限制不影響判定（它們只會夾住確定值）。
   - 它不是「主軸或交叉軸尺寸由內容決定」的 flex／grid 項目：flex 項目須 `flex-basis` 為確定長度（或 `auto` 而該軸尺寸確定）且 `flex-grow`／`flex-shrink` 的結果不依賴內容——保守起見，第一版只把 `flex: none`（或等效）且尺寸確定的項目、以及不在 flex／grid 容器內的元素算作邊界；grid 項目須所在軌道皆為確定長度或 `fr`。
   - `overflow` 不是 `visible`（`hidden`／`clip`／`scroll`／`auto`），使內容溢出不改變祖先捲動容器的可捲動範圍；或它沒有任何會被溢出影響的祖先捲動容器（保守：第一版只接受非 `visible`）。
   - 不參與基線對齊的傳遞（它不是 inline-level、不是以 `baseline` 對齊的 flex 項目）。
   文件根永遠是邊界。
2. **髒位元的傳播停在最近的邊界**（含自己不算：一個節點變髒，往上標到第一個邊界祖先為止，該祖先標為「重排根」）。邊界的判定用上一次的計算樣式；樣式改變使邊界性質改變的那一次，照舊往上傳到下一個邊界。
3. **局部重排**：`LayoutEngine::layout` 對每個重排根只重排其子樹（以它上次確定的外框為包含塊）；絕對定位的子孫只處理包含塊在該子樹內者；`applyTransforms` 只走該子樹。多個重排根互為祖孫時取最外層。
4. **更便宜的一步**（包在同一個機制裡）：重排根是文字節點的直接容器、且新文字的量測結果（行數、每行寬高）與之前相同時，仍照第 3 點走——不另開特例；單價由 57 章的階梯量測決定之後是否值得再細分。
5. **防呆**：驗證模式 `LayoutEngine::setVerifyPartial(true)`——局部重排之後再從根完整重排一次並逐節點比對方框；不同即為邊界判定錯誤，記錄診斷（第一個不同的節點）並以完整重排的結果為準。所有測試、golden、corpus 回放預設開啟；任何不一致是測試失敗。總開關 `XT_PARTIAL_LAYOUT=0`。
6. 給製作者的指引（寫進技能的 performance 參考檔）：頻繁改寫的文字放在寬高固定、`overflow: hidden` 的容器裡。

**T-755 實作記錄（#714，2026-11-19）**：邊界判定快取在 `ElementData::layoutFlags`（寬確定、高確定、是邊界、grid 軌道固定）；`kDirtyLayout` 往上標到寫入節點上方最近的邊界，該邊界加標 `kLayoutRoot` 並記進 `NodeArena::layoutRoots()`；`layout_partial.cpp`。退回完整重排：文件根被標為重排根（結構改變、寫入上方沒有邊界、視口／安全區／uiscale 改變）、局部重排關閉（`XT_PARTIAL_LAYOUT=0`／`Context::setPartialLayout(false)`）、尚未做過完整重排、重排根沒有可重播的參數。驗證模式與 23 §23.ag 的同一個開關（`XT_VERIFY_VISUAL_GENERATION=1`）：每次局部重排後跑完整重排、逐節點逐位元比對，不一致即中止——所有測試、golden、corpus 開啟下零不一致。`ElementRef::setText` 改為只標文字節點（否則重排根永遠越過文字的直接容器）。局部重排處理重排根底下**所有**絕對定位子孫（設計寫的是包含塊在子樹內者；實作取較安全的超集合，追認）。`FrameStats::layoutRoots`／`layoutFullPasses`／`partialLayoutMismatch`。

量測（Release、wgpu、1920×1080、釘效能核）：文字放在固定尺寸、`overflow:hidden` 的盒子裡時，單一文字變更的重排 370 → 1 個元素、update 0.78 → 0.031 ms；War Table storm 的 layout 0.167 → 0.046 ms（重排元素 1053 → 351）、focus 0.045 → 0.027 ms。**但 Sherefox `debate` 沒有改善（重排 34 → 32 個元素，layout 多約 6%）**：它的文字容器是 `overflow: visible` 的固定尺寸絕對定位元素——只差「overflow 不是 visible」這一條，第一個邊界因此是整個畫面。常見的寫法都是這樣，這條條件太嚴：T-782 放寬（固定尺寸、`overflow: visible` 的元素也是邊界；祖先捲動容器的捲動範圍在局部重排後另行更新）。

**2026-11-19 修訂（T-782 #716）：邊界的條件去掉 overflow。** 本節第 1 點原本要求邊界的 overflow 兩軸都不是 `visible`，理由是「子孫溢出會擴大祖先捲動容器的捲動範圍」。這個理由在本引擎不成立：捲動範圍只來自**直接子元素**的方框（§6.5 的 `storeScrollSize`；略過絕對定位與 `display:none` 的子元素），孫輩以下的溢出本來就不往上傳，而邊界自己的方框不隨內容改變。layout 階段也沒有其他讀「子孫溢出後的範圍」的地方（sticky 未實作、沒有 CSS anchor 定位、`focus_nav` 不看溢出、效果範圍與 `projectionBoundsHint` 在 paint 階段、捲動夾限讀節點自己的範圍）。**現行規則**：寬高都確定（T-755 的規則）即為邊界，另加 flex 項目（grow 0、shrink 0）與 grid 項目（固定軌道）的條件；overflow 無關。日後若實作 sticky、CSS anchor 定位或讓捲動範圍納入子孫溢出，必須回頭檢查這條。沒有重排根時 `LayoutEngine::layout()` 直接返回（消掉 T-755 的 6% 回退）。

量測（同 T-755 的條件）：Sherefox `debate` 每幀改一個文字——重排 32 → 6 個元素、layout 0.196 → 0.043 ms、update 0.263 → 0.092 ms（T-755 之前是 34／0.187／0.254）；War Table storm 的 layout 0.0455 → 0.0348 ms（重排元素 351 → 217）。驗證模式下所有測試、golden、corpus 零不一致。

