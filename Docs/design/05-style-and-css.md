# 05 · HTML/CSS 子集與樣式層疊

## 5.1 `.xt` 文件格式

XML 風格的 HTML 子集。編譯器用自寫的遞迴下降解析器（不引入第三方 XML 函式庫）。

```xml
<xt>
  <head>
    <link href="theme.xts"/>
    <style> .a { color: #fff; } </style>
  </head>
  <body>
    <div id="root" class="panel large" style="width: 200px">
      <span>Hello</span>
      <img src="images/logo.png"/>
      <anim src="anims/loading.json" autoplay="true" loop="true"/>
      <input type="text" value=""/>
      <button disabled="true">OK</button>
    </div>
  </body>
</xt>
```

規則：
- 所有標籤必須閉合（`<img/>` 或 `<img></img>`）。屬性值必須加引號。
- 支援 `<!-- -->` 註解。
- 實體：`&lt; &gt; &amp; &quot; &apos; &#NNN; &#xHH;`。
- 文字節點：元素內的文字經空白摺疊（連續空白 → 單一空格，首尾裁切）。**一個元素最多包含一段文字**，且有文字時不可再有子元素（第一版不做 inline 混排）。違反時編譯錯誤。
- 未知標籤視為 `div`，編譯警告。

## 5.2 元素標籤

| 標籤 | 預設 display | 特殊行為 |
|---|---|---|
| `xt`, `head`, `body`, `div`, `p`, `h1`–`h6`, `ul`, `li`, `section` | block | 無 |
| `span`, `label`, `b`, `i` | block | 無（第一版無 inline） |
| `img` | block | `src` 屬性 → AssetId；固有尺寸為圖片尺寸 |
| `anim` | block | `src` → SceneId；`autoplay`、`loop`（true/false/pingpong）、`speed`、`segment="start,end"`（秒）；固有尺寸為場景設計尺寸；子場景以 `object-fit: contain` 縮放至內容盒 |
| `button` | block | 可 focus；`disabled` 屬性 → `kStateDisabled` |
| `input` | block | `type=text`：可 focus、接收文字輸入，`value` 屬性；`type=checkbox`：點擊切換 `kStateChecked` |
| `scroll` | block | `overflow: scroll`，滾輪事件改變 `ScrollX/Y` |
| `camera` | none | 19.4：不佈局、不繪製、不命中，僅建立 `Camera` 子節點；屬性 `type`（`perspective` \| `orthographic`，預設 perspective）、`position="x y z"`（可省略 z → 0）、`target="x y z"`、`zoom`、`near`、`far`；不得有子節點；同一父下第二台只警告不使用 |

`head`、`link`、`style`、`title` 不產生元素。

## 5.3 `.xts` 樣式表語法

CSS 子集。解析器自寫。

- 選擇器：`tag`、`.class`、`#id`、`*`、後代組合子（空白）、子組合子 `>`、群組 `,`、偽類 `:hover :active :focus :disabled :checked`。可組合如 `div.panel#main:hover > .child`。
- T-173（#173）新增：空白 = 後代組合子（任何複合開頭）；`:not(<簡單選擇器清單>)`（不巢狀、不含複合／組合子／`:root`）、`:nth-child(an+b|odd|even)`、`:first-child`、`:last-child`、`[attr]`／`[attr=]`／`[attr^=]`／`[attr$=]`／`[attr*=]`（`.xt` 屬性與執行時 `setAttribute`；`[class…]` 為錯誤、`[id…]` 比對 id）、`:focus-within`、`:lang(<bcp47 前綴>)`（目前依繼承的 `lang` 屬性，`setLanguage` 之後改文件語言）。特異性：屬性選擇器與偽類算 class；`:not()` 取參數最高者。
- 不支援：`::before/::after`、相鄰兄弟、`:has()`。
- `@keyframes name { from|to|N% { decls } }`。
- `@font-face { font-family: "X"; src: "fonts/x.ttf"; font-weight: 400; }`。
- `!important`。
- 註解 `/* */`。
- 值單位：`px`、`%`、`em`、`auto`、無單位數字（僅 `flex-grow/shrink`、`opacity`、`z-index`、`line-height`）。
- 顏色：`#rgb #rgba #rrggbb #rrggbbaa`、`rgb()`、`rgba()`、`transparent`、17 個 CSS Level 1 命名色加 `orange`。
- 未知屬性或無法解析的值：編譯警告並忽略該宣告。
- 裁決（2026-10-21，T-413 對照測試發現）：**關鍵字與單位 ASCII 不分大小寫**（CSS Syntax 語意；`display: FLEX`、`10PX` 合法），命名色亦同；字串與 `url()` 內容保留大小寫。執行期 `mini_css` 已如此，編譯器仍為完全相符——補齊為 T-421。**清單型屬性（`animation`、`transition`、`effect`…）任一項無效 → 整條宣告丟棄並警告**，不是跳過該項；`animation` 已依此修正（T-413），`transition` 同樣補齊於 T-421。

## 5.4 支援的屬性

編號空間：0–159 為 PropId（03.9），160 起為不可動畫的 StyleProp。

| 屬性 | StyleProp | 型別 | 值 | 初始值 | 繼承 |
|---|---|---|---|---|---|
| `display` | 160 | Enum | `block`, `flex`, `none` | block | 否 |
| `position` | 161 | Enum | `static`, `relative`, `absolute` | static | 否 |
| `flex-direction` | 162 | Enum | `row`, `column`, `row-reverse`, `column-reverse` | row | 否 |
| `flex-wrap` | 163 | Enum | `nowrap`, `wrap` | nowrap | 否 |
| `justify-content` | 164 | Enum | `flex-start`, `flex-end`, `center`, `space-between`, `space-around`, `space-evenly` | flex-start | 否 |
| `align-items` | 165 | Enum | `stretch`, `flex-start`, `flex-end`, `center` | stretch | 否 |
| `align-self` | 166 | Enum | `auto` + align-items 的值 | auto | 否 |
| `align-content` | 167 | Enum | 同 justify-content + `stretch` | stretch | 否 |
| `overflow` | 168 | Enum | `visible`, `hidden`, `scroll` | visible | 否 |
| `z-index` | 169 | Float | 整數 | 0 | 否 |
| `visibility` | 170 | Enum | `visible`, `hidden` | visible | 是 |
| `pointer-events` | 171 | Enum | `auto`, `none` | auto | 是 |
| `font-family` | 172 | ListRef | 字串清單 | `"sans-serif"` | 是 |
| `font-weight` | 173 | Enum | 100–900 的百位數、`normal`=400、`bold`=700 | 400 | 是 |
| `font-style` | 174 | Enum | `normal`, `italic` | normal | 是 |
| `text-align` | 175 | Enum | `start`, `end`, `left`, `center`, `right` | start | 是 |
| `white-space` | 176 | Enum | `normal`, `nowrap` | normal | 是 |
| `text-overflow` | 177 | Enum | `clip`, `ellipsis` | clip | 否 |
| `cursor` | 178 | Enum | `default`, `pointer`, `text`, `move` | default | 是 |
| `object-fit` | 179 | Enum | `fill`, `contain`, `cover` | contain | 否 |
| `transition` | 180 | ListRef | 見 5.6 | 空 | 否 |
| `animation` | 181 | ListRef | 見 5.6 | 空 | 否 |
| `background-image` | 182 | StringId | 資產名或 `none` | none | 否 |
| `background-gradient` | 190 | ListRef | `none`、`linear-gradient(<angle>, <color-stop>+)`、`radial-gradient(<len> <len> at <len> <len>, <color-stop>+)`（5.5c；T-115） | none | 否 |
| `direction` | 183 | Enum | `ltr`, `rtl` | ltr | 是 |
| `writing-mode` | 184 | Enum | `horizontal-tb`, `vertical-rl`, `vertical-lr`（僅影響該元素的文字子節點，見 06.8） | horizontal-tb | 是 |
| `text-orientation` | 185 | Enum | `mixed`, `upright`, `sideways` | mixed | 是 |
| `lang` | 186 | StringId | BCP 47（來自 `lang` 屬性，非 CSS） | 空 | 是 |
| `effect` | 187 | ListRef | 見 18-effects.md §18.4 | none（空序列） | 否 |
| `backface-visibility` | 188 | Enum | `visible`, `hidden` | visible | 否 |
| `text-rendering` | 189 | Enum | `auto`, `geometricPrecision`（`auto`：小字走點陣 + 覆蓋率 gamma；`geometricPrecision`：一律外框、無 gamma，23 §23.4） | auto | 是 |

可動畫屬性（PropId）對應的 CSS 名稱與初始值：

| CSS | PropId | 初始值 | 繼承 |
|---|---|---|---|
| `opacity` | Opacity | 1 | 否 |
| `transform` | Translate/Rotation/Scale/Skew（見 5.5） | 恆等 | 否 |
| `transform-origin` | Anchor | `50% 50%`（佈局後換算為 px） | 否 |
| `width height min-* max-*` | 64–69 | auto / 0 / none(=Auto) | 否 |
| `margin*` `padding*` `border-*-width` | 70–81 | 0 | 否 |
| `left top right bottom` | 82–85 | auto | 否 |
| `flex-grow` `flex-shrink` `flex-basis` | 86–88 | 0 / 1 / auto | 否 |
| `row-gap` `column-gap` `gap`(簡寫) | 89–90 | 0 | 否 |
| `font-size` | 91 | 16px | 是 |
| `line-height` | 92 | 1.2（無單位，存為 Em 單位 1.2） | 是 |
| `letter-spacing` | 93 | 0 | 是 |
| `background-color` | 96 | transparent | 否 |
| `border-color` | 97 | transparent | 否 |
| `border-radius`(簡寫) `border-*-radius` | 98–101 | 0 | 否 |
| `color` | 102 | #000 | 是 |
| `image-opacity` | 103 | 1 | 否 |
| `perspective` | 106 | `none`(=Auto) | 否 |
| `perspective-origin` | 107–108 | `50% 50%`（佈局後依自身盒尺寸換算為 px） | 否 |

簡寫展開：`margin`、`padding`、`border-width`、`border-radius`（1–4 值 CSS 規則）、`border: <width> <color>`、`flex: <grow> <shrink> <basis>`、`gap`、`inset`（→ left/top/right/bottom）。

## 5.5 `transform`

語法：空白分隔的函式序列，支援 `translate(x[,y])`、`translateX`、`translateY`、`scale(s[,sy])`、`scaleX`、`scaleY`、`rotate(deg)`、`skew(x[,y])`、`skewX`、`skewY`。單位：長度用 `px`（`%` 相對自身盒尺寸），角度 `deg`。

**19.10 新增（T-47a）**：`translateZ(len)`（Length，僅 `px`/`em`，`%` 編譯錯誤「translateZ does not accept percent」）、`translate3d(x, y, z)`（x/y 同 `translate`，z 同 `translateZ` 的限制）、`rotateX(angle)`、`rotateY(angle)`。`rotate3d()`、`scale3d()`、`matrix3d()`、`perspective()` 函式不支援，編譯錯誤「unsupported transform function」。

編譯器把序列**合成為單一分解**（T-120 修訂；之前只接受固定順序、各至多一次）：
- 若序列只含各類函式**至多一次**且順序符合 `translate* → rotateX → rotateY → rotate → skew → scale`（可省略），直接對應各 PropId（與之前相同，位元組不變）。
- 否則把序列依 CSS 語意（左乘，先寫的先套用在外層）合成 4×4 仿射矩陣，再分解為 `translate(x,y,z)`、`rotateX`、`rotateY`、`rotate`（Z）、`skew(x,y)`、`scale(x,y)`（03.3 順序）：平移取第四欄；3×3 部分做 QR 型分解（Gram–Schmidt：第一軸長 = scaleX，第二軸去除第一軸分量後長 = scaleY 與 skewX，第三軸只允許為單位長且正交——否則需要 3D 剪切或 z 縮放，無法表示 → 編譯錯誤「transform sequence needs a 3D shear or z scale that XTFrame cannot represent」並指出該宣告）；旋轉矩陣依 Rz·Ry·Rx 拆成三個歐拉角（萬向鎖時取 rotateY = ±90°、rotateX = 0）。`rotateY(θ) translateZ(d)` 這類 3D HUD 常見寫法因此合法（平移向量 = R·(0,0,d)）。
- 執行期 `setStyle("transform: …")`（mini_css）同一規則。`transition: transform` 對分解後的分量各自過渡（既有語意）。

`@keyframes` 內的 `transform` 同樣規則，且各關鍵幀必須含相同的函式集合（缺者以恆等補上，編譯警告）。

**`none`（T-69）**：`transform: none` 是合法值，等同空序列（所有分量為恆等）；編譯器與執行時 mini_css 都接受。`@keyframes` 停靠點寫 `transform: none` 時，依上一條「缺者以恆等補上」處理，**不發警告**（`none` 是明確的恆等）。`none` 不可與函式混寫（`none rotate(10deg)` → 編譯錯誤「invalid value」）。

## 5.5a `perspective`、`perspective-origin`、`backface-visibility`（19.10，T-47a）

- `perspective: none | <length>`（`none` 編碼為 `LengthUnit::Auto`，PropId 106，可動畫）；`none`（預設）表示不建立透視空間根。
- `perspective-origin: <length-or-keyword> [<length-or-keyword>]`，語法同 `transform-origin`（PropId 107/108，預設 `50% 50%`，佈局後依自身 border-box 尺寸換算為 px）。
- `backface-visibility: visible | hidden`（StyleProp 188，不可動畫，預設 `visible`）；`hidden` 時背面朝向攝影機的節點不繪製（19.3）。
- `transform-style` 不支援（恆為 preserve-3d，19.10）。

## 5.5c `background-gradient`（T-115，0.4）

畫在 `background-color` 之上、`background-image` 之下，與背景色同一條路徑填滿 border box（含 `border-radius` 裁切；無邊框時等於 padding box）。

```
background-gradient: none
  | linear-gradient( <angle> , <color-stop> [, <color-stop>]+ )      // 角度：deg/turn/rad，CSS 語意 0deg 向上、順時針；stop 位置只接受 %（可省略：首末 0/100、中間均分）；2～16 個 stop；漸層線通過中心、長度 |W sin a| + |H cos a|
  | radial-gradient( <len|%> <len|%> at <len|%> <len|%> , <color-stop>+ )   // 半徑 rx ry、圓心 cx cy（px 或 %，相對 border box）；軸對齊橢圓（T-116 起 `PaintRef.p2` 帶第二軸；之前只有正圓）
```

IR：STYL 的宣告值為 ListRef 指向 `gradientPool[]`（每筆：kind、angle 或 rx/ry/cx/cy 四個 Length、stopOffset/stopCount 指向既有 `effectStops[]` 同一個 stop 池）。Painter：背景的 `FillCmd.paint` 改用 `PaintKind::Linear/Radial`，`p0/p1` 依上式在佈局後換算成元素局部 px（與 Lottie/SVG 場景走同一條光柵路徑，SW/GL 不需改）。執行期：`ElementRef::setStyle("background-gradient: …")` 可用；不可動畫（0.4 不做 stop 插值）；`transition` 忽略。編輯器屬性面板顯示為文字欄位（反射表 kind = ListRef，比照 `effect`）。

## 5.5b `effect` 序列規則（18-effects.md §18.4，T-51a）

- `effect`（StyleProp 187，不繼承，初始 `none`）語法、七種內建函式與其 `params` 索引對應，見 18-effects.md §18.3-18.4。編譯器將整個序列存進 STYL 的 `effects[]`/`effectStops[]` 池，`IrDecl.value` 為指向該池的 `ListRef`（與 `transition`/`animation` 同機制）。
- 長度只允許 `px`/`em`（`%` 為編譯錯誤「effect lengths do not accept percent」）；`custom()` 於本版一律編譯錯誤「custom effects are not supported in this version」；序列上限 16 個效果。
- `@keyframes` 對 `effect`：規則不同於 `transform`（5.5 的「補恆等」不適用——效果沒有恆等值）：
  - 該 `@keyframes` 區塊內只要有任一停靠點寫了 `effect`，則**每個**停靠點都必須寫 `effect`，否則編譯錯誤「effect keyframes must use the same effect sequence」。
  - 所有停靠點的效果序列必須有相同的 `kind`、相同的數量與順序，否則同一個錯誤。
  - 展開為每個效果、每個可動畫參數各一條 Track（`prop = effectParam(effectIndex, paramIndex)`，18.2 的編碼），只在該參數於任一停靠點與其他停靠點不同時才產生（全相同者不產生）；`DropShadow` 的位移以 `paramIndex = 18`（合成 `Vec2`）取代 `paramIndex 1`/`2` 兩條。
- `transition: effect <time>`：`IrTransitionSpec.prop = 187`；展開為堆疊中每個可動畫參數各自的 transition 是 StyleEngine（層疊）的職責，見 18-effects.md §18.4。

## 5.6 `transition` 與 `animation`

```
transition: <prop | all> <duration> [<easing>] [<delay>] {, ...}
animation:  <keyframes-name> <duration> [<easing>] [<delay>] [<iteration-count | infinite>]
            [<direction>] [<fill-mode>] [<play-state>] {, ...}
```

`transition` 的屬性名可用 shorthand：`transform` 展開為各變換分量（TranslateX/Y/Z、RotationX/Y、Rotation、SkewX/Y、ScaleX/Y），`transform-origin` 展開為 AnchorX/Y，`perspective-origin` 展開為 PerspectiveOriginX/Y，共用同一組時長/緩動/延遲（T-79）。各分量各自過渡，不是 CSS 的整串插值。**限制**：`transform-origin` 與 `perspective-origin` 的值是 `%` Length，佈局後才換算成錨點，過渡系統只對 float 屬性作差，所以這兩個 shorthand 目前只解析、不產生過渡（值直接跳變）；要動畫錨點請用 `@keyframes` 或以 `translate` 等價寫法（T-88 記錄）。

時間單位 `s`、`ms`。`easing` 為 5 個關鍵字或 `cubic-bezier(a,b,c,d)`、`steps(n[, start|end])`。

`animation` 的每個 `<keyframes-name>` 在編譯時解析為 `TimelineId`；找不到即編譯錯誤。

## 5.7 層疊演算法

對每個 `kDirtyStyle` 的 Element：

1. **匹配**：遍歷 `rules[]`，對主體 part 檢查 `tag/id/classes/stateMask`；通過後對祖先 part 依 combinator 向上搜尋（descendant：任一祖先；child：直接父）。匹配成本 O(rules × depth)，第一版不建索引。
2. **排序**：符合的規則依 `(important, specificity, 來源順序)` 升冪，後者覆蓋。inline style 的特異性視為 `1<<24`，仍低於任何 `!important`。
3. **計算**：從父元素的 `ComputedStyle` 複製「繼承」屬性作為起點，其餘屬性設初始值，再依序套用宣告。
4. **單位換算**：`em` 於此階段換算為 px（相對本元素的 `font-size`，`font-size` 本身的 em 相對父）。`%` 保留，佈局時換算。`transform-origin` 的 `%` 保留，佈局後換算。
5. **寫入基底值**：對每個 PropId 呼叫 `setProperty`（見 04.7 層 0）；若值改變且有對應 transition 宣告，註冊 Transition。
6. **animation 宣告變更**：比較新舊 `AnimationSpec` 清單；新增者 `Animator::play`，移除者 `stop`，不變者不動。`play-state: paused` 對應 `pause`。

```cpp
struct ComputedStyle {
    // 不可動畫，直接欄位
    Display display; Position position; FlexDirection flexDir; FlexWrap flexWrap;
    Justify justify; Align alignItems, alignSelf, alignContent;
    Overflow overflow; float zIndex; Visibility visibility; PointerEvents pointerEvents;
    uint32_t fontFamilyOffset, fontFamilyCount; uint16_t fontWeight; FontStyle fontStyle;
    TextAlign textAlign; WhiteSpace whiteSpace; TextOverflow textOverflow; Cursor cursor; ObjectFit objectFit;
    TextDirection direction; WritingMode writingMode; TextOrientation textOrientation; StringId lang;
    uint32_t transitionOffset, transitionCount;   // → IR transitions[]
    uint32_t animationOffset, animationCount;
    AssetId  backgroundImage;
    // 可動畫屬性的基底值（PropId 64..108 與 0..12）
    Value base[kElementPropCount];   // kElementPropCount = 109（PropId 0..108 的密集陣列；場景專用 32..63 槽位閒置）
    // 快取
    FontId resolvedFont;   // 由 FontProvider::match 得到
};
```

## 5.8 狀態偽類更新

`stateBits` 改變時（08）標記該元素 `kDirtyStyle`；**同時**標記其所有祖先與子孫 `kDirtyStyle`（因為 `.a:hover .b` 與 `.a:hover > .b` 需要重算 `.b`）。第一版接受此保守成本。

## 5.9 語言新增總表（2026-09-27 定案；對應 26～30 章與 16.2.6）

「階段」欄：**M18** = 編輯器藍圖必要（先做）；**0.6** = 16.2.6 的補齊與新能力（其後）。語意細節見括號內章節。

### 5.9.1 `.xt` 元素與屬性

| 語法 | 階段 | 說明 |
|---|---|---|
| `<symbol name origin requires state-groups variants>…</symbol>`、`<use symbol id slot:* prop:* attr:* variant:* sync>`、`<slot name>` | M18 | 26.2；編譯展開；覆寫只允許視覺與文字群組 |
| 所有元素必有 `id`（英數底線、文件唯一） | M18 | 26.1；缺者由編譯器警告、編輯器自動命名 |
| `on:<event>="<actions>"`（亦可 `.xts` 的 `on-<event>`） | M18 | 27.2 事件表與動作表 |
| `bind:<target>="<port> \| conv(...) ..."` | M18 | 27.3；target = 屬性、`text`、`src`、`visible`、`state.<Name>`、`class.<name>`、`variant.<Prop>` |
| `port:<name>="<type> [in\|out\|call]"`、`expose:false` | M18 | 27.3.1 |
| `<repeat port item-enter item-exit virtualize>` | M18 | 27.3.4、3.12.4 |
| `<glyph key="<action>">` | M18 | 27.7；行內或獨立 |
| 富文本：文字內 `<span style class>`、`<b>`、`<i>`、`<glyph>`、`<img inline>`、`<a on:click>` | M18 | 07.8 |
| `<video src loop rate>`、`<sequence src fps loop>`、`<external source="scene-view:cam\|engine://…">` | M18 | 25.3.5b、3.12.1 |
| `<particles …>` | M23 | 3.12.5 |
| `draggable`、`drop-target`、`nav:up/down/left/right`、`autofocus`、`aria-label` | M21 | 27.7、08.9 |
| 文字值 `@key`（在地化鍵；屬性值亦可） | M18 | 07.8、13.8 |

### 5.9.2 `.xts` 規則與 at-rule

| 語法 | 階段 | 說明 |
|---|---|---|
| `@timeline <name> { duration; marker; <selector> { <t> [easing] { decls } … } sound { <t> <cue> } }`、`@timeline <name> from "<file>#<name>"` | M18 | 28.1.2、4.10.1；選擇器限 `#id`／路徑 id；宣告可含 `effect.<名>.<參數>` |
| `symbol(<Name>) { … }` 範圍區塊 | M18 | 26.2.1 |
| `:state(<Name>)`、`state-groups`、`state-enter/exit: <Name> <actions>`、`state-transition` | M18 | 27.1、4.10.3 |
| `pointer-watch`、`layer: overlay`、`focus-nav`、`clip-start/clip-rate`、`path-trim`、`text-reveal`、`text-effect`（outline/glow/shadow）、`text-transform`、`text-decoration` | M18 | 27、28、07.8 |
| 原生 `var(--x)` 與 `--x:` 宣告（token；主題 = 換值不重編） | M18 | 30、26.3.2 |
| `@media (aspect-ratio ≥ …) / (min-width) / (platform: console) / (input: gamepad)` | M18 | 25.4 自適應 |
| 單位 `vw vh vmin vmax uiscale safe-top/right/bottom/left`；`calc()` | M18 | 3.12.6、06 |
| `:not()`、`:nth-child()`、`[attr]`、`:focus-within`、`:lang()` | M18 | 05.3 擴充 |
| `border-image`（九宮格；中段平鋪選項） | M23 | 26.3.4 |
| `translate`/`rotate`/`scale` 獨立屬性、`linear()` 緩動、`image-rendering`、`aspect-ratio`、`conic-gradient`、`hsl()`、`tabular-nums`、`line-clamp`、`text-wrap: balance` | 0.6 A/B | 16.2.6 |
| `mix-blend-mode`、`clip-path`（含隨形命中）、錨定定位、grid 子集、`@container`、`interpolate-size`、巢狀語法、`@layer`、`render-cache`、`color-mix()`／`oklch()` | 0.6 B | 16.2.6 |

### 5.9.3 解析器規則

- 未知 `on:`／`bind:` 動作或轉換器 → 編譯錯誤（不是警告），因為那是契約。
- `@timeline` 內時間為秒或毫秒；缺 `duration` 取最後關鍵幀、標記、音效三者之最大；同一元素同一屬性同一時間重複 → 錯誤。
- `var()` 找不到 → 警告並用 fallback（`var(--x, 8px)`）；無 fallback 用屬性初始值。
- T-170 實作規則：`:root` 規則只准放 token 宣告；token 可引用 token（循環 → 警告並丟棄）；**整值** `var()` 於 Float/Length/Color/Enum 長屬性與 margin/padding/border-width/border-radius/gap/inset 簡寫（逐長屬性）保留為引用、可換主題；巢狀於較大值內、list 型屬性（font-family、background-image、transition、animation、effect、background-gradient）與其他簡寫只在編譯時代入、主題不可改；`@keyframes` 內展平；`style` 屬性不可宣告 token；token 型別不合屬性 → 警告；`@timeline` 內的 `var()` 目前拒絕（待決）。
- `@media` 只允許上表的條件；在編譯時保留為條件式規則（IR 新區塊 `MEDIA`），執行時依視口與宿主旗標啟用。
- 2026-10-20 補記（Aux Duty XF-011）：`@media` 已支援（M18 T-176 起，規則見下方 T-176 條；M32 T-330 補 `prefers-reduced-motion`／`resolution`，見 §5.9.8）；舊資料寫「無 media query」者以此為準。

- T-173 匹配細節：條件部件於複合之後求值，仍 O(深度)；`:nth-child` 用 `resolve()` 期間的兄弟索引快取；`:focus-within` 於每次 `resolve()` 起始重算（只在樣式表用到時）；使用結構偽類時，兄弟增刪把同層子樹標髒；編譯期靜態匹配把帶條件的複合視為永不靜態匹配。編輯器規則面板只計特異性、不匹配偽類與屬性。

- T-176 實作規則：`@media` 寬高只接受 px；不支援的查詢 → 警告並略過區塊（讓 dc 匯入貼上的 CSS 仍可編譯）；巢狀 `@media`、內含 at-rule、`symbol()`、token 宣告 → 錯誤；`(aspect-ratio)`／`(orientation)` 可由 `DeviceInfo.aspect` 覆寫；比較容差 1e-4。`calc()` 只允許於 Length 長屬性（簡寫、`@keyframes`、`@timeline`、`var()` fallback 內拒絕）；`em` 與 `var()` 項在樣式時代入執行時池；`calc()` 內未定義且無 fallback 的 `var()` → 編譯警告、執行時丟棄宣告。執行時 `setStyle`（mini_css）尚不接受新單位與 `calc()`。

- T-179（#176）在地化：文字節點或 `placeholder`／`alt`／`title`（含 `slot:` 文字）以 `@` 開頭 = 鍵，鍵規則 `[A-Za-z_][A-Za-z0-9_-]*`；`\@` 逸出；`@` 開頭但非合法鍵 → 錯誤；`&#64;` 亦算 `@`；`<input>` 內的鍵警告且維持字面。`@font-fallback <lang> { font-family: A, "B C"; }`（同語言後者取代前者）。偵測在 `doc_emit`（符號展開之後）。匯入器尚未逸出 `@` 開頭文字。

- T-178（#177）富文本：元素有直接非空白文字且所有子元素都是行內標記（span、b、i、a、glyph、img inline）時整段展平為 runs（否則仍是「混排錯誤」）；空白摺疊並重對映 run 偏移；行內子元素自樹中移除。run 覆寫子集：color、font-size、font-weight、font-style、letter-spacing、text-effect；純 `.class` 規則（無標籤／id／狀態／條件、不在 `@media`／`symbol()` 內）於編譯時解析到 run，其他宣告警告丟棄。`a`、`glyph` 為已知標籤；`inline` 是唯一可裸寫的屬性。`text-effect` = StyleProp 192（可繼承，ListRef 進 `effects[]`；outline→Dilate、glow→Blur、shadow→Blur 加位移；半徑上限 8 px 並警告）。富文本與 `@key` 同段尚未處理（翻譯會使偏移失效）。

- T-175（#178）綁定與連接點：未宣告的 bind 來源 → 錯誤；未宣告的 `on:port(x)` 警告；未宣告的 `emit(x)` 仍以名稱送宿主。port 作用域 = 最內層符號實例；lookup 接受 `name`、`<id>.<port>`、`<useId>.<port>`（`ui#id.port` 未做）；`expose:false`（可裸寫）隱藏 `<useId>.<port>`。宿主轉換器寫 `host.<name>(...)`。`format(p, a, b)` 的 `a` 為 `{0}`；`select`／`step` 函數式以第一個引數為輸入；比較與 `range()` 的 `%` 相對 `<port>_max`（缺則錯誤）；`tween` 為鏈末且目標為 Float/Length/Color 時成為 Animator transition，否則在綁定內插值；`fromToken` 回 `var(--name)`；`#Other.<prop>` 於佈局後再評估一次（同幀多一次樣式解析與佈局）。

- T-187：同一文字節點同時有 runs 與 `@key` → 警告「translation invalidates run offsets」。

- T-191（#188）外部內容：`<video id src loop rate autoplay sync>`（只由宿主解碼；ASST 名 `engine://video:<id|src>`；src 不打包）、`<sequence id src fps=24 loop autoplay sync frame-size="WxH" frames="N">`（src = 資料夾／`name_%04d.png` 樣式／圖集；幀列於 `data-xt-frames`）、`<external source="scene-view:<cam>|engine://<name>|host:<name>">`；三者為替換元素（同 `<img>`，不可有子元素）；`loop/autoplay/sync` 可裸寫；錯誤：缺 src、壞 source、壞 frame-size、序列無幀。`.xts`：`clip-start: <time>`（StyleProp 193）、`clip-rate: <n>`（194，> 0），亦作用於 `<anim sync>`。觸發：`play(<media id>[, loop])`（`then` 等結束）、`stop`、`seek`（IR flag `kActionMedia = 16`；時間軸名優先於媒體 id）；事件 `media(<id>).end`（TriggerEvent MediaEnd = 25）。

- T-190：`<svg>` 為合法 `.xt` 元素（軌道 `#<svg id>/<inner id>` 進該 svg 自身時間軸）；SVG 解析器接受 `path-trim="s e o"` 屬性；向量屬性只在 `@timeline` 內有效（一般規則中警告丟棄）。

### 5.9.4 實作記錄（T-199 #197、T-200 #198）

- **`border-image`**（T-199）：`border-image: <url(x)|none> [<L> [<R> <T> <B>]] [fill] [stretch|repeat|round]`；長寫 `border-image-source`、`border-image-slice: L R T B [fill] | auto`、`border-image-repeat`、`border-image-fill`。切片為無單位來源像素或 `%`；`auto` 取資產清單 `measured.slice`（無則 0）。StyleProps **197**（StringRef，最終 ASST 名）、**198**（四個 f32，負值 = 比例、NaN = auto）、**199／200**（Enum）。繪製為 4 角 + 4 邊（+ fill 中段）的 `DrawImage` 子矩形 quad 取代一般邊框；`repeat` 置中平鋪帶部分端、`round` 整數次平鋪；命中仍為盒。偏差：border-width 為 0 的邊以切片像素當寬；切片重疊時依 CSS 等比縮小。執行期 `ElementRef::setStyle("border-image", …)` 不支援（編輯器改寫來源）。
- **`world-anchor="<port>"`**（T-200）：元素屬性；port 須為 vec2/vec3；語意見 02 §2.s.1 與 13 §13.8.13。
- **分量轉換器**（T-200）：`x`／`y`／`z`／`w`（Vec2/Vec3 分量或 Color r/g/b/a 0～1；純數字時 `x` 回原值其餘 0）、`vec2(a, b)`／`vec3(a, b, c)` 可作運算式頭（數字或 port，第一個 port 為輸入）或鏈尾 `<port> | vec2(y)`（鏈輸入放第一位）。`setPortText` 接受 `[x, y]`。
- **隱含系統 port**（T-200）：`on:` 或 `bind:` 提到 `system.loading/paused/resumed/disconnected`（event）或 `system.language/platform/inputDevice`（string）時，編譯器自動宣告為元素 0 的 in port。限制：只出現在 `.xts` `on-*` 規則裡的引用不會被偵測。

### 5.9.5 實作記錄（T-201 #199）

- **`conic-gradient([from] <angle> [at <pos>], stops)`**：0.6 B 提前完成。`from` 可省，中心預設 `50% 50%`，t 自起始射線順時針一圈；IR `IrGradient` kind 3、`PaintKind::Conic`、GPU `Shading::Gradient` kind 4（`GradientParams` 與 `GpuFrame` 結構不變）；SW 逐像素 `PaintEval::conicT`；runtime `setStyle` 也可解析。
- **`focus-trap`**：裸屬性或 `true`／`false`，其他值編譯錯誤 → NodeCommon `kFocusTrap`。
- **`world-anchor`、`draggable="axis-x"`** 見 27 §27.13；**`.xts` 路徑 id 選擇器** `#A/Box`（lexer 在 `#` token 內允許 `/`），`prop:` 白名單不變。
- **`<link rel="symbols|stylesheet">`** 只以路徑解析（T-138 的檔名搜尋順序限縮為只用於資產與 `@timeline from`）；文件引用自己是錯誤。
- **綁定目標**新增 `translate-x`／`translate-y`／`scale-x`／`scale-y`／`rotate`（不觸發重排）。
- `<button>` 引擎本無預設邊框，任務單「改為 0」無事可改；符號庫看到的側線源自邊框縫（06 §6.14）。

### 5.9.6 實作記錄（T-202 #202）：`<particles>`

- `<particles>` 的數值屬性編成元素最前面的 inline 宣告（`rate="30"` ≡ `particle-rate: 30`），故屬性蓋過 `.xts` 規則、`style` 與執行期宣告再蓋過屬性；`@timeline`／`bind:` 作用在同一組 PropId。
- `.xts`：每個 PropId 一個 longhand，另有簡寫 `particle-lifetime|speed|size|rotation|color`（color 可 1、2 或 4 個 stop）。
- `texture` 編成 ASST 項目並記 `data-xt-texture`（不用 `data-xt-asset`，避免固有尺寸）。
- `play/stop(<粒子 id>)` 沿用 `kActionMedia` 旗標、執行期依目標分派；`seek(<粒子>)` 為編譯錯誤。shape／blend／simulate-in-editor 值不合法或元素有子節點 → 錯誤。

> T-204（#203）：綁定目標新增 `checked`（僅 `<input type="checkbox">`；其他元素為編譯錯誤；不觸發事件與 check/uncheck 觸發，避免宿主改值時播出音效）。

> T-210（#209）發現：`transform: translateX(<百分比>)` 在 runtime 被丟棄（`style_decode::lengthToPx` 對 Percent 回 false）。**裁決**：修 core——百分比 translate 依元素自身 border box 解析（CSS 語意），→ T-211（#210）完成：`ElementData::cssTranslatePct`（% of border box，06 §6.7 fold 加 `pct × 0.01 × box`，與 transform-origin 同路徑）；px（Float）寫入取代整個分量（`bind:translate-x` 或 px keyframe 作用在 `translate(-50%)` 元素時置中會消失）；transition 任一端含 % → 兩端以 `Vec2(px, %)` 各自插值再相加；keyframe／`@timeline` 的 translate 軌只要有 `%` 停點整條為 Length 型別，相鄰停點 px／% 混用為編譯錯誤；`getProperty(TranslateX)` 仍只回 px；compose 路徑（任意順序 transform）仍拒絕 `%`。golden `ui_translate_percent`。匯入與 `repair.anchor` 維持 `calc(P% - Npx)`。

### 5.9.7 實作記錄（T-218 #219）：向量屬性進樣式層疊（31 §31.5.1）

- 語法：subject 可為場景目標 `#<svg id>/<inner>`、`#<svg id>/<tag>`（path|rect|circle|ellipse|line|polygon|polyline|g）、`#<svg id>/*`；前面可接任何祖先組合、偽類與狀態（`#Btn:hover #Icon/Blade`）。lexer 早把 `#A/B` 讀成一個 path id（T-201 符號實例子元素），所以場景目標在**符號展開後**由 DOM 判定：整個 id 不是任何元素 id、斜線前半是 svg／anim／img 元素才算；多段取最後一段（`#Play/Icon/Blade`）。最後一段的判定（T-236 #237 改寫）：在該 svg 的 `ids` 表 → 名稱；否則是 tag 名（含 `modifier`）→ tag；都不是 → 名稱；`@timeline` 同規則。`/*` 緊貼 id 讀進 hash，但下一個 `{`／`;`／`}` 前若出現 `*/` 仍視為註解。
- 允許宣告 = `appliesToSvgNode`（移到 `xts_props`）：fill、fill-opacity、stroke、stroke-opacity、stroke-width、stroke-dashoffset、path-trim*、opacity、transform 分量、`d`；`stroke-dasharray` 由 T-236（#237）補上（FloatArray，不 transition）；`fill: url()` 警告；其他屬性警告「only vector properties apply to <svg> content」。`transition` 接受向量屬性（`d` 除外）與 `path-trim`。`vector.unknownTarget` 對 inline svg 名稱目標找不到時警告（strict 下依既有規則失敗）。
- 限制：fill／stroke 只改顏色不改 `PaintKind`（`fill="none"` 的形狀設 fill 仍不畫）；含 `var()` 的部分替換（VarSubst）宣告重解析時向量屬性被丟。
- 測試 `Tests/unit/test_svg_style_rules.cpp`；golden `ui_svg_hover`。

> T-229（#231）：`@timeline`／`@keyframes` 內的 `visibility` 編譯為 PropId 13 `Visible`（Hold 幀、1 = visible），不再以 StyleProp 170 進軌道（後者會觸發 `Animator::evaluate` 斷言）；inline `<svg>` 內容亦放行（`appliesToSvgNode(Visible)`）。逐格動畫（31 §31.7.5）以此表達格的顯示區間。

> T-243（#251，32 章）：`<svg>` 內新元素 `<xt-skeleton>`、`<xt-bone length rotate translate scale>`、`<xt-constraint kind target …>`、`<xt-mesh href width height>`＋`<xt-pins>`／`<xt-tris>`、`<xt-weights bones version>`（格式見 32 §32.2.2）；`xt-skin="#id"` 指骨架。`bind:` 與 `world-anchor` 可寫在 `<svg>` 內元素上（BIND 帶 `sceneNode`；`on:` 尚未支援，警告）。`.xts` 名稱：`length`、`strength`、`mix`、`param0…5`、`inertia`、`damping`、`gravity`、`enabled`（約束別名只在 `@timeline` 與 `bind:` 依種類換算，場景目標規則不換算）。

### 5.9.8 M32 CSS 補齊定案（2026-10-14；調查報告 `Docs/reviews/2026-10-14-css3-gaps.md`）

使用者 2026-10-14：「研究還沒做的，把 CSS3 有需要的能力補齊」。調查結果：61 項對照，15 已支援、20 部分、22 缺、4 依本章與 06／07／16 不做（`::before/::after`、`+`、`:has()`；`position: fixed`；inline／inline-block；`text-align: justify`）。原則不變：只做增量，不重寫樣式引擎；除 mix-blend-mode 與 clip-path（使用時多一層離屏／stencil）外全部是編譯期或解析時成本。

**分期（任務單見 `Docs/tasks/`，順序即派工順序）**

| 單 | 內容 | 層 | 模型 |
|---|---|---|---|
| T-321 | 匯入器：放行 `em/vw/vh/vmin/vmax/calc()`；不支援的選擇器預過濾為 dropped（不再讓匯入失敗）；`conic-gradient` 與 Figma `GRADIENT_ANGULAR` 接到核心 conic；`text-shadow` 改成 `text-effect` 而非元素 `shadow()`；`dialect.md` 同步 | import | Opus |
| T-322 | 語法：`hsl()/hsla()`、完整 CSS 命名色、`rotateZ()`、2D `matrix()`（走 compose 分解）、`step-start/step-end`、`transition-*` 長寫、`rem`（相對 `:root` font-size）、匯入器對 `perspective()`／`rotate3d/scale3d/matrix3d`／`transform-style` 改 dropped（核心維持 19 §19.10 的不支援錯誤） | parser／mini_css／import | Opus |
| T-323 | 原生 `text-transform`（繼承型 Enum，shaping 前轉換；**大小寫表只做 ASCII + Latin-1 + Latin Extended-A**，其他碼位原樣）；匯入器停止寫死字元（24 §24.3c 同步） | resolver／layout_text／import | Opus |
| T-324 | `text-decoration`（underline／line-through／overline、color、thickness；以字型度量畫 FillRect；run 覆寫子集加入）；Figma `textDecoration` 輸出 | resolver／painter_text／import | Opus |
| T-325 | `outline-width/color/offset/style(solid)` 焦點框：沿 T-201 環形路徑畫在 border box 外、不影響佈局與命中；width／color 可動畫 | resolver／painter | Opus |
| T-326 | 元素層級 `mix-blend-mode`：**開放後端既有的全部 16 種 + add**（18 §18.12 的 4 值表擴為全部）；走 18 §18.1 的 PushLayer(opacity, blend) 路徑；damage 比照 BackdropBlur 聯集；兩個匯入前端接線（Figma `blendMode`、dc `mix-blend-mode`） | core／IR／import | Fable |
| T-327 | `background-size（cover/contain/<len>）/position/repeat`、背景圖裁到圓角、`image-rendering: pixelated`；GL sampler wrap | resolver／painter／GL | Opus |
| T-328 | `aspect-ratio`（block／flex／absolute 單軸 auto 時推導另一軸，與 min/max 凍結互動照 CSS Sizing 4） | layout | Opus |
| T-329 | 獨立 `translate/rotate/scale`（順序 translate·rotate·scale·transform；與 `transform` 同時存在時編譯期合成） | compile／mini_css | Opus |
| T-330 | `@media (prefers-reduced-motion)`（映射宿主旗標）、`(resolution)`；`min()/max()/clamp()` | parser／runtime pool／DeviceInfo | Opus |
| T-331 | `line-clamp`、`tabular-nums`（TextShaper 介面加 OpenType feature；NullTextShaper 忽略） | layout_text／xt_text | Opus |
| T-332 | `clip-path: inset()/circle()/polygon()` + 隨形命中（IR 池、`PushClipPath`、08 命中；GL stencil 位元風險見 16 §16.4） | core／IR／08 | Fable |
| T-333 | 逐邊 `border-color`、`dashed/dotted` 邊框（`geom_dash`）；Figma strokeDashes 輸出 | painter／import | Opus |
| T-334 | `filter: invert()/sepia()`（ColorAdjust 參數 5–7） | effects SW／GL／xtfx | Opus |
| T-335 | `linear()` 緩動（分段點池；IR 版本變更）+ **`rem`**（`LengthUnit::Rem`，相對 `<xt>` 根元素 font-size，T-322 撤回的實作） | Easing／IR／style_engine | Opus |
| T-336 | `overflow: auto/x/y`、flex `wrap-reverse`、`order` | layout | Opus |
| T-337 | 匯入器：`::before/::after` 展開成真實子元素（只在匯入時；核心仍不支援） | import | Opus |
| T-338 | **Grid 子集**（06 §6.15：固定軌道 px/%/fr/auto/repeat、gap、areas、grid-column/row 與 span、row-major 自動放置、對齊）；匯入器 2D grid 直接對映 | layout／mini_css／import | Opus |
| T-339 | 編輯器格線工具（06 §6.15 編輯器段；17 章同步）：切成格線、畫布覆蓋層、拖放放置、區域取名、屬性面板不露 CSS、版型模板存格線、`<repeat>` 接格線 | editor | Opus |

| T-340 | 編輯器屬性面板補齊 M32 新屬性（outline、text-transform、text-decoration、mix-blend-mode、background-size/position/repeat、image-rendering、aspect-ratio、獨立變換、clip-path…；`property_groups.cpp` 手寫表） | editor | Opus |
**T-325 屬性定義（05 §5.5 表追加）**：`outline-width`（Length，預設 0，可動畫）、`outline-color`（Color，預設 = `color`，可動畫）、`outline-offset`（Length，預設 0，可動畫）、`outline-style`（Enum `none | solid`，預設 none）；簡寫 `outline: <width> <style> <color>` 任意順序。畫在 border 之後、子項之前，外緣 = border box 外擴 offset + width、內緣外擴 offset、圓角 = border-radius + offset（夾 0）；不影響佈局、命中與 overflow；外擴計入 paint bounds。與 `effect: outline(...)`（T-201 向量描邊效果）是兩回事。

**T-323 屬性定義**：`text-transform: none | uppercase | lowercase | capitalize`，繼承，Enum，不可動畫；在 shaping 前對整個文字節點轉換（shaping 快取鍵含它）；大小寫表只含 ASCII、Latin-1 Supplement、Latin Extended-A，其他碼位原樣；`capitalize` 的詞首 = 文字開頭或前一碼位為空白／ASCII 標點；run 覆寫子集不含它；`<input>` 編輯值不轉換。24 §24.3c 的「匯入時寫死」隨本單作廢。

**實作記錄 T-325（PR #355，2026-10-14）**：PropId `OutlineWidth` 129／`OutlineColor` 130／`OutlineOffset` 131（`kElementPropCount` 132）、StyleProp `outline-style` 201（`kLast` 201，`style_decode.h` 與 `xts_props.h` 同步）；IR 版本不變，舊 fixture 可載。方角維持方角（只有 radius > 0 才取 max(0, r + offset)）；`outline-color` 未宣告時由 style_engine 取元素最終 `color`（簡寫缺色不輸出宣告，低優先序規則的 outline-color 會沿用）；畫在元素自身 image 之後、overflow 裁切與子項之前。`mini_css` 加 `Kind::Outline`／`kOutlineProp`。匯入器 `thin/medium/thick` = 1／3／5 px；`keyframes-fx` 夾具改用 `word-spacing` 觸發丟棄。golden `ui_outline_focus` + GL 對照（容差 16/0.03）。T-326 若加 StyleProp 從 203 起（202 已由 T-323 使用）。

**T-324 屬性定義**：`text-decoration-line: none | [underline || overline || line-through]`（StyleProp 位旗標；不繼承，但作用於後代文字——resolver 向下傳裝飾集合）、`text-decoration-color`（Color，預設 = `color`，可動畫）、`text-decoration-thickness: auto | <length>`（auto = max(1px, 1/14 em)，可動畫）；簡寫 `text-decoration: <line…> [<color>] [<thickness>]`。字形層每 run 一個 FillRect：underline 在基線下 `FontMetrics.underlineOffset`（shaper 未提供時 0.1 em）、overline 在 ascent、line-through 在基線上 0.3 em；寬 = run 進距總和；與 `text-effect` 的順序：效果 → 字形 → 裝飾。run 覆寫子集加 `-line`／`-color`（`<u>`、`<s>`）。

**實作記錄 T-322（PR #356，2026-10-14）**：`hsl()/hsla()`（`css_color.cpp` `hslToColor`，RGB 取整 8 位）；命名色 148 個共用表（`Code/Runtime/core/src/css_color.{h,cpp}`，`.xts`／`mini_css`／`parse_svg` 三處共用，`parse_svg` 原 31 色私表刪除）；`rotateZ`、2D `matrix()`（`transform_compose.cpp` `decomposeMatrix2D`，奇異矩陣警告丟棄）；`step-start/end`；`transition-*` 長寫只在同一宣告區塊內與簡寫合成（後者勝、property 數為項數、其他清單循環；跨規則逐分量層疊不做；缺 easing 用簡寫預設 linear；`mini_css` 一律拒收，同 `transition`）；匯入器 `dropUnsupported3dTransforms` 逐函式丟棄並記 sourceIssue，`preserve-3d` 靜默視為無損。**rem 未做**：需要新 `LengthUnit`，舊執行時會拒收，移到 T-335 隨 IR 版本一起；匯入器照 T-321 繼續丟 rem。`currentcolor` 無既有語意，未支援。

**T-326 屬性定義**：`mix-blend-mode` 見 18 §18.12（全部 16 種 + `add`；`additive` 別名）。

**T-327 屬性定義**：`background-size: auto | cover | contain | <len|%> [<len|%>]`（可動畫）、`background-position: <len|%|left|center|right> [<len|%|top|center|bottom>]`（% = (盒 − 圖) × %，可動畫）、`background-repeat: stretch | no-repeat | repeat | repeat-x | repeat-y`（**預設 `stretch`** = 既有行為「拉伸到 padding box」，XTFrame 擴充值，保證舊文件不變；CSS 四值只在明確宣告時生效）、`image-rendering: auto | pixelated`（`ImageCmd.smooth`，含 `<img>`／`<sequence>`）。repeat 以拆 quad 平鋪（上限 4096 張，超過退回 stretch 並警告），不依賴後端 wrap；有 border-radius 時整組圖以 `PushClipPath` 裁到 padding box 圓角。

**實作記錄 T-323（PR #357，2026-10-14）**：StyleProp `text-transform` 202（`kLast` 202）；`Code/Runtime/core/src/text_case.{h,cpp}` 兩張 384 項表（U+0000–U+017F，Unicode 16 簡單映射，大寫 119／小寫 118；會改 UTF-8 長度的 U+0130／U+0131／U+017F 維持原樣以守 run 偏移，`XT_ASSERT` 長度不變）；`capitalize` 詞界 = 文字開頭或前一碼位為 Unicode White_Space／ASCII 標點（`o'neil` → `O'Neil`）；轉換在 shaping 快取未命中時做（`TextNodeData::keyTransform` 入鍵，`m_caseScratch`），`markNonAnimatableDirty` 視為版面變更；可編輯 `<input>` 一律 None（顯示與編輯同路徑）；`@key` 替換後再轉換。匯入器 T-157 寫死機制整套移除（`dc_import.cpp` TransformRule 等），keyframe 內的 text-transform 靜默略過；Figma textCase 三值、SMALL_CAPS 丟棄。**T-326 的 StyleProp 從 203 起、T-324 從 203 起（兩者先併者取 203）。**

**T-328 屬性定義**：`aspect-ratio` 見 06 §6.16。

**實作記錄 T-324（PR #358，2026-10-14）**：StyleProp `text-decoration-line` 203（位旗標 1 underline／2 overline／4 line-through，`kLast` 203）、PropId `TextDecorationColor` 132、`TextDecorationThickness` 133（`kElementPropCount` 134）；厚度只收 `auto`／px／em。度量取 FreeType `underline_position/thickness`（post 表），字型無則 0.1 em，於 StyleEngine 解析字型時存進 `ComputedStyle`（`deco*`）。underline／line-through 以線中心對位、overline 上緣對 ascent；線矩形對齊整像素（最少 1 px）再夾進 run 盒（避免 SW／GL 分數座標差 1 px）；直排與旋轉 run 不畫；`text-reveal` 部分顯示的 run 仍畫整條；run 只能加線不能取消，run 線色依序取 run `-color` → run `color` → 元素裝飾色；祖先裝飾色與粗細在子元素解析時取快照（不跟祖先動畫逐幀）。dc `data-xt-rich` 加 u/ins → `<u>`、s/del/strike → `<s>`；Figma textDecoration 兩值。golden `ui_text_decoration`，GL 對照容差 16/0.05（差異只在字形邊緣）。`dialect.md` 未同步（T-327 一併補）。

**實作記錄 T-326（PR #359，2026-10-14，Fable）**：StyleProp `mix-blend-mode` 204（`kLast`）；`ComputedStyle::mixBlendMode` → `NodeCommon.blend`，Painter 既有 `enterNode` 的 `blend != Normal → PushLayer(opacity, blend)` 分支對所有節點種類生效，不需改邏輯。damage：`damage.cpp` `computeDamage()` 在 BackdropBlur 迴圈後新增一輪——blend != Normal 的 `LayerCmd.boundsHint` 與相交的髒矩形聯集，迭代到穩定；本元素變動不拉下方。GPU builder（`frame_builder.cpp` `compositeOnto`）未改：Normal 走硬體 SrcOver，其餘 16 種先複製父目標為 backdrop 再以 Composite shader 混合（`CompositeParams.mode`）。`.xtcl` `PackedLayerCmd.blend` 已序列化，無缺口；`command_simplifier` 最小子集把 Layer blend 降為 Normal（9.5 既有）。匯入：Figma 16 種 + LINEAR_DODGE→add、LINEAR_BURN 仍丟、PASS_THROUGH／NORMAL 不輸出；dc `plus-lighter`→add。golden `ui_blend_modes`（4×5 格 17 種）+ GL 對照 16/0.03。**編輯器屬性面板是手寫表**（`property_groups.cpp`），`mix-blend-mode`、`outline-*`、`text-transform`、`text-decoration` 都不會自動出現 → M32 收尾單 **T-340**：屬性面板補齊 M32 新屬性。

**T-329 屬性定義**：`translate: <x> [<y> [<z>]]`（% 同 §6.14.1）、`rotate: <angle> | z <angle>`、`scale: <sx> [<sy>]`；可動畫、不繼承、`none` = 初值；最終矩陣 = translate · rotate · scale · transform，原點同 `transform-origin`；單獨出現時直通既有 PropId（零新 PropId），與 `transform` 在**同一宣告區塊**（一條規則、一個 keyframe 停靠點、一個 `@timeline` key、一次 `setStyle`）同時出現時於編譯期合成；**跨規則或同時動畫時逐分量由後者覆寫，不相乘**（層疊後的 PropId 存不下兩組輸入；T-329 實作後修訂）；`rotate: x|y` 不做（19 章 `rotateX/Y`）。

**T-330 定義**：`@media (prefers-reduced-motion: reduce | no-preference)`（`DeviceInfo.reducedMotion`，宿主旗標 `system.reducedMotion`）、`@media (min-/max-/resolution: <n>dppx|x)`（以 `dpr` 求值）；`min()`／`max()`／`clamp()` 與 `calc()` 同位置同限制，全常數時編譯期折疊，否則進執行時 calc 池（op 追加 `Min`／`Max`，`clamp(a,b,c)` = `max(a, min(b, c))`）。

**實作記錄 T-327（PR #360，2026-10-14）**：PropId `BackgroundSizeX/Y` 134／135（`cover`／`contain` 編碼為 `LengthUnit::Auto` value 1／2，關鍵字間切換不插值）、`BackgroundPositionX/Y` 136／137（`kElementPropCount` 138）；StyleProp `background-repeat` 205、`image-rendering` 206（繼承，`kLast` 206）。平鋪拆 quad 上限 4096／元素，超過退 stretch 並由 `backgroundTilesCapped()` 立旗、文件 `warnings()` 記一次 `kWarnBackgroundTiles`；圓角以一組 `PushClipPath`／`PopClip` 包住整組圖（含 stretch），無圓角不加命令、超出以 `srcUv` 裁。`.xts`／`mini_css` 沒有 `background` 簡寫（維持），匯入器拆簡寫；匯入器不自動補 `background-repeat`（dialect 提醒作者）。Figma `scaleMode` 對映在 `figma_assets.cpp`：FILL／FIT → cover／contain center no-repeat、TILE → repeat + size、STRETCH（crop）只在 imageTransform 軸對齊時轉 size／position，否則仍點陣化。golden `ui_background_image`，GL 容差 16/0.01；既有 155 張逐位元組不變。dialect.md 三份已含 text-decoration 與本單屬性。

**T-331 定義**：`line-clamp: none | <integer ≥ 1>`（`-webkit-line-clamp` 別名；不繼承、不可動畫）：換行文字最多 n 行，第 n 行有後續內容時以 §6.8 的 ellipsis 機制在視覺末端截斷；`nowrap` + `text-overflow: ellipsis` 優先（n 視為 1）。`font-variant-numeric: normal | tabular-nums`（繼承）：`ShapeParams.features` 位旗標（bit0 = tnum）→ HarfBuzz feature；`NullTextShaper` 忽略；入 shaping 快取鍵；run 覆寫子集加入。

**T-332 定義**：`clip-path: none | inset(t [r [b [l]]] [round …]) | circle(r [at x y]) | ellipse(rx ry [at x y]) | polygon(x y, …)`；長度 px／%（% 相對 border box），`closest-side`／`farthest-side` 收；參考盒固定 border box；不繼承；**第一版不可動畫**（形狀進 IR 可選新池 `clipShapes[]`，StyleProp 為索引，0 = none；舊執行時略過該區塊）。繪製：元素變換後、背景前 `PushClipPath`，子項後 `PopClip`；與 `overflow: hidden` 疊加（路徑在外、矩形在內；T-332 實作後修訂）；效果在裁切之外；GL stencil 深度不足時退回包圍盒並警告一次。命中：08 §8.2 先做點在形內測試，不在則元素與子樹皆不命中。`path()`／`url()` 不做。

**實作記錄 T-328（PR #361，2026-10-14）**：PropId `AspectRatio` 138（Float，0 = auto，可動畫，觸發 relayout；`kElementPropCount` 139）。`resolveAspect(w, h, ratio, minmax, definiteIsAutomatic)`／`aspectMinMax()`（`layout.h`／`layout_engine.cpp`），呼叫點 block 2、flex 2、absolute 2。flex 項目 stretch 優先於比例（`m_aspectSuppressed` 包住該項目的三次 layoutChild；`alignSelf` 解析移到量測前）；06 §6.16 已依 CSS Sizing 4 修正。明寫比例套 border box；`<img>` `auto` 沿 §6.10 content box；absolute 只認明寫比例；img 兩軸 auto + min/max 會回推（2×2 圖 + `min-width:100px` → 100×100，既有 golden 不變）。已知小毛病：flex column 項目自寫 px 高又被 grow 拉高時比例用自身高度（layoutBlock 既有）。快照 `Tests/layout/aspect_ratio_*`。

**實作記錄 T-329（PR #362，2026-10-14）**：`xts_parser.cpp` `mergeIndependentTransforms()`（規則／keyframe 停靠點／timeline key 三處，區塊讀完後依 translate、rotate、scale、transform 串成序列交給既有解析，成功後以此取代區塊內 10 個分量宣告）；`xts_values.cpp` `parseIndependentTransform()` 單獨出現時固定順序快速路徑直通，`none` 只重置自己的分量；`mini_css` `parseDeclarations()` 同樣合成（哨兵 0xFFF7～0xFFF9）；`rotate: x|y` 警告丟棄；合成序列需走矩陣路徑時長度必須是 px（`translate: 50%` 配 `transform: translate(10px)` 依 05.5 報錯；固定順序時 % 照 T-211 保留）。新增 `transition: translate|rotate|scale`。手算矩陣案例 T(10,20)·R(90)·S(2)·T(5,0) → 平移 (10,30)、線性 [0 −2; 2 0]。整合測試 `Tests/layout/independent_transforms.xt`。

**T-333 定義**：`border-top/right/bottom/left-color`（四 PropId，可動畫；`border-color` 一～四值展開；舊單一 `BorderColor` 為四邊未宣告時的 fallback）、`border-style`／`border-*-style: none | solid | dashed | dotted`（預設 solid）。同色 solid 走既有環形單一 FillPath（輸出位元組不變）；異色 → 四個梯形（角以 45° 切、圓角沿弧中線）；`dashed` dash = 3×寬、gap = 2×寬，`dotted` dash = gap = 寬、圓端點。

**T-334 定義**：`filter: invert(<n|%>) sepia(<n|%>)`，併入 ColorAdjust 參數槽 5／6，`EffectInstance` 佈局與 IR 版本不變；數學依 CSS Filter Effects（invert `c + a(1−2c)`；sepia 標準矩陣與單位矩陣依 a 插值）。

**T-335**：M32 唯一動 IR 版本的單（次版本 +1）：`linear()` 緩動（`EasingKind::LinearPoints` + IR 池 `easingPoints[]`）與 `rem`（`LengthUnit::Rem`，相對 `<xt>` 根 font-size，樣式時換算、calc 內進執行時池）。02 章版本表隨該單併入時更新；宿主（CE）下次同步重建。

**T-336 定義**：`overflow` 加 `auto`（溢出前不裁不捲、溢出後同 scroll）；`overflow-x`／`overflow-y`（簡寫設兩軸；一軸非 visible 時另一軸 visible 視為 auto；裁切仍為整個 padding box，捲動只在非 visible 軸）；flex `wrap-reverse`（行堆疊反向，`align-content` start／end 跟著反）；`order: <integer>`（flex 項目穩定排序，繪製序與命中序跟著；absolute 子項不受影響；不可動畫）。

**T-337**：匯入時把有 `content` 的 `::before/::after` 規則展開為 `<span data-xt-pseudo>` 子元素（文字／`url()` 圖／空盒），其餘宣告成為該 span 的 class；帶偽類的基底不展開；核心仍不支援偽元素（05 §5.3 不變）。

**實作記錄 T-330（PR #363，2026-10-14）**：`MediaFeature` ReducedMotion=9（0 no-preference／1 reduce）、MinResolution=10、MaxResolution=11、Resolution=12（dppx 儲存，`x` 照寫、`dpi`/96，相等容差 1e-4）；`CalcOpKind` Min=7、Max=8（n 元為 n−1 個 op；`clamp(a,b,c)` = RPN `a b c Min Max`）；皆在 `ir_pod.h`，IR 版本不變——**用到新語法的文件舊執行時 `kInvalidIr` 拒收**（嚴格驗證維持，T-335 升次版本後自然分界）。`DeviceInfo.reducedMotion`；宿主 `postSystemEvent("reducedMotion", reduce|true|1 ／ no-preference|false|0)`，其他值 `kInvalidArgument`；`system.reducedMotion` 字串 port 由編譯器按需宣告（`bind_emit`）、流圖 `system` 節點第 9 個輸出（`systemEventKinds()` 9 種）。常數折疊 `Code/Runtime/compile/src/calc_emit.cpp` `foldCalcToPx()`（只對含 Min/Max、全 px、無 var 的程式；純 `calc(10px+5px)` 照舊進 CALC 以保 golden）；`@keyframes`／`@timeline` 內全 px 的 `min()` 仍拒收（同 calc）。執行時 `layout_engine.cpp` `evalCalc` 算 Min/Max。匯入器 `@media` 本就原樣保留（T-187）。`reducedMotion` 切換 UI → T-340。

**實作記錄 T-332（PR #364，2026-10-15，Fable）**：StyleProp `clip-path` 207（低段 160～207 **用盡**；208 起是修飾器 PropId）；IR 可選區塊 **`CLIP`**（DOCT 後、SHDR 後，至多一次：`u32 n, IrClipShape[n]{kind, paramOffset, paramCount}` + `u32 m, IrClipParam[m]{f32 value, u8 unit}`），宣告 Enum 0 = none、否則 1 + 池索引；IR 版本不變，未知區塊依 02 §2.1 略過（以改名 fourcc 的測試重現）。`Code/Runtime/core/src/clip_shape.{h,cpp}` 三用：painter 路徑、damage（`subtreeBounds` ∩ 形狀 bbox；`damage.cpp::collect` 追蹤開啟中的 PushClipPath 收緊 bounds）、`hit_test.cpp::tryEnter()`（overflow 檢查後、進入元素前，border box 空間）。串流順序 **路徑在外、矩形在內**（overflow 的 PushClipRect 本就只包子項，在背景之後；09 §9.3 偽碼過時，交集結果與 stencil 深度相同）。GL 既有 `kMaxClipDepth = 15`，超深退包圍盒 scissor 並 `lastError` 一次。polygon 固定 even-odd；inset `round` 1～4 值含橢圓角；circle % 半徑 sqrt((w²+h²)/2)；`closest/farthest-side` 以 Auto 單位 + 0/1 標記；`transition: clip-path` 警告丟棄；`setStyle` 走兩槽運行池。匯入器 `convertClipPath`，`path()`／`url()`／geometry-box 丟棄並警告。golden `ui_clip_path`（GL 16/0.03）。dialect.md 未補 clip-path 說明（下一張 import 相關單補）。

**StyleProp 編號規則（2026-10-15 定）**：低段 160～207 已滿；第二段自 **256** 起（rig PropId 止於 255）：`kLastLow = 207`、`kFirstHigh = 256`，`isStylePropId` 接受兩段；舊執行時 `style_decode` 略過不認得的號碼，IR 版本不變。T-331 取 256（`line-clamp`）、257（`font-variant-numeric`）；之後依併入順序遞增。

**實作記錄 T-331（PR #365，2026-10-15）**：StyleProp `line-clamp` 256、`font-variant-numeric` 257（第二段首兩號；`kLastLow` 207／`kFirstHigh` 256／`kLast` 257，`isStylePropId` 兩段，`doc_emit` 的 run 覆寫子集改看 257）。clamp：`TextLayoutParams.lineClamp`，`breakLines` 後截成 n 行並標記末行，末行先去視覺末端空白再走 nowrap 的 ellipsis 分支（同一 budget 迴圈 + `ellipsisGlyph`）；`layout_text.cpp` 決定 n（非 nowrap 用宣告值、nowrap+ellipsis 為 1、其他 nowrap 為 0）；改 n 只標髒版面不重新 shaping。tnum：`ShapeParams.features`（`kShapeFeatureTnum = 1`）、`TextNodeData::keyFeatures` 入快取鍵、`TextRunStyle.features`；FT shaper 推 `hb_feature_t{tnum}`。內建 NotoSans 子集預設數字已等寬（572/1000 em），golden 兩欄無視覺差。匯入：`-webkit-box` + `-webkit-line-clamp` → block + `line-clamp`；`tabular-nums`／`normal` 保留、其他丟棄；Figma `opentypeFlags.TNUM`。golden `ui_line_clamp`。

**實作記錄 T-334（PR #366，2026-10-15）**：ColorAdjust 參數槽 5 invert／6 sepia（`.xts` 與 `mini_css` 的 `color-adjust(... invert(n|%) sepia(n|%))`；參數空白取 1、夾 0..1；`style_emit`／`property_access` 參數名表加 invert、sepia 可動畫）；矩陣 `M = Sepia·Invert·Hue·Sat·Contrast·Brightness`，**固定槽位順序、不照宣告順序**（既有行為），最後夾限再 `a *= opacity`；SW `sw_effect_math.cpp` `invertMat`／`sepiaMat`，GL `gpu_effects.cpp` 同組法（shader 本吃 4×5 矩陣），SW／GL 差 ≤ 1/255。匯入 `filterToEffect` 加兩函式；`backdrop-filter` 仍丟棄（BackdropBlur 只有 radius／saturation／brightness 三槽，18 §18.3）。golden `fx_invert_sepia`。**T-340 追加**：`Code/Editor/Core/src/effect_stack.cpp:132` 輸出 color-adjust 文字不含 invert／sepia，編輯器存回會掉。

**實作記錄 T-333（PR #367，2026-10-15）**：PropId `BorderTop/Right/Bottom/LeftColor` 139～142（`kElementPropCount` 143）；StyleProp `border-*-style` 258～261（0 none／1 solid／2 dashed／3 dotted，`kBorderStyle*`，`kLast` 261）。`border-color` 單值仍寫舊 PropId 97（既有 IR 不變），2～4 值才展開；`ComputedStyle::borderColorSides` 四位元記「該邊用自己的顏色」（最後一個簡寫之後的逐邊宣告才算；`setProperty`／`setStyle` 亦設；其餘邊複製單一色讓 getProperty 讀到實際值）。`none` 只不畫、不改版面。方角沿外角→內角連線切（等寬 45°）、圓角沿外弧與內弧 45° 中點連線切，半段弧單一 cubic（k = 4/3·tan(θ/4)）；dashed／dotted 以中線（r − 相鄰寬平均/2）路徑 `flattenPath` → `applyDash`（3w/2w；dotted {0, 2w} 圓端點，相位回移 1e-3·w 讓第一點落在角切線）→ `strokeToFill`，兩端以角切線半平面裁切；每邊一個 NonZero FillPath（上右下左）。同色 solid 輸出位元組不變（`border_radius.xt` FNV-1a 3348850703092603236 寫死在測試）。`.xts` `border` 簡寫不含 style 段，匯入器另輸出 `border-style`；Figma `strokeDashes` 3:2 → dashed 無警告，其他取較近者並 Medium 警告。golden `ui_border_styles`。dialect.md 未補邊框樣式（T-336 匯入部分一併補）。

**實作記錄 T-335（PR #368，2026-10-15）**：IR **2.5 → 2.6**（唯一常數 `kIrFormatMinor = 6`；bundle／manifest／pack 只記 major）。`easingPoints[]` 池在 STYL 尾端 `floatPool` 之後（02 §2.x），`EasingKind::LinearPoints = 4`，`IrEasing.stepCount` = 點數、`x1` 位元組 = u32 起點索引；驗證 ≥ 2 點、在池內、有限、in 不遞減。Animator 持有整份文件共用池（載入複製 IR 池；`mini_css` 的 `linear()` 以 `internEasingPoints()` 去重附加）；`evalEasing`／`evalTrack` 加 pool 參數，空池時當 linear。編譯端 `RuleSet::easingPoints` → `StyleBuilder::easingPoints`；`style=""` 可寫；`var()` 展開後重解析出的 `linear()` 與 `bind_emit` 的緩動丟棄並警告；Lottie 匯出當 linear。`rem`：依 T-322 記錄重做；根字級 px 變動時整樹標髒；匯入器放行。相容：minor 0／3／4／5 fixture 與 216 份 corpus 往返不變；**舊執行時無 minor 上限檢查**——含 `linear()` 的 2.6 檔在舊版當 linear 播放，含 `rem` 的被 `validLengthUnit` 拒收（維持，不補版本拒收）。**宿主（CE）下次同步必須重建**。T-340 追加：`easing_popover`／`curve_editor`／`vector_morph_edit` 不認 `linear()`（顯示成 linear、編輯會覆寫）；`stylesheet.cpp formatStyleValue` 與 `property_autolayout` 單位列舉無 rem。

**實作記錄 T-336（PR #369，2026-10-15）**：`Overflow::Auto` = 3、`FlexWrap::WrapReverse` = 2；StyleProp `overflow-x` 262／`overflow-y` 263（`kLast` 263；單值 `overflow` 仍寫 168 同設兩軸，既有 IR 不變）；PropId `Order` 143（Float 存整數，`kElementPropCount` 144；非整數／帶單位拒收；`animatable=false`，transition 拒、`@keyframes` 丟棄警告）。helper `usedOverflowX/Y`、`clipsOverflow`、`appliedScroll`（`node.h`），painter／3D／hit_test／layout 捲動位移／repeat 虛擬化共用；滾輪 `event_dispatcher.cpp handleWheel` 逐軸 `scrollsOnAxis` 與 `maxScrollX/Y`，兩軸都不能捲時讓給祖先；hidden 軸 API 寫入保存但不套用位移。捲動尺寸：開始計入水平範圍、flex 容器用項目實際範圍、夾限改為「超出內容盒的量」（有 padding 時最後一段原本捲不到——修正；`ui_scroll_wheel` golden 不變）；`storeScrollSize` 移出遞迴框架（T-255b 深樹堆疊）。`order`：`ElementData::flexOrder` 快取（items／orders／permutation），`layout_flex.cpp applyFlexOrder()` 分行前穩定排序，全 0 不排；繪製與命中沿用 `painter.cpp sortedElementChildren()` 鍵改 (z-index, order)，只在 flex 容器內、absolute 以 0 計。未涵蓋：auto 溢出判定不計 absolute／relative／transform；wrap-reverse 鏡射用 min/max-height 夾限前的交叉尺寸。devtools `dumps.cpp` 加 `overflowY`。快照 `overflow_axes`／`flex_wrap_reverse`／`flex_order`。

**實作記錄 T-337（PR #370，2026-10-15）**：匯入時展開 `::before/::after`（含 CSS2 單冒號）。匹配器：標籤／`*`／`.class`／`#id` 複合 + 後代組合子；偽類、`>`／`+`／`~`／`[attr]`、`@media` 內的規則維持 dropped。插入 `<span data-xt-pseudo="before|after">`（路徑 `<host>/before|after` 不佔兄弟索引、`data-xt-name=<host>-before|after`）；content 文字／`url()`／`image-set` 第一項／空盒；`attr()`、counter、quote、混用 → 整條丟棄並 sourceIssue。沒寫 `content` 或 `content: none` 的規則若與有 content 的規則命中同一元素則一起層疊、不計 dropped；單獨存在仍 dropped。class `pseudo-<hash6>`（排序宣告字串雜湊，每 UI 去重，接在 helmet 樣式表後）。宿主只有文字時文字變子 run。不放 span：`img`／`input`／`br`／`hr`／`svg` 子樹／`sc-for`／`sc-if`／`data-xt-state`／`data-xt-rich` 子樹／glyph。`data-xt-pseudo` 進 `dc_tags.cpp` 已知標籤並原樣寫入 `.xt`（27 §27.5 表未改——補記於此）。報告 `pseudoExpanded {count, rules[]}`，lint 列 info。夾具 `pseudo-elements`：11 條 → 7 展開／4 dropped。

**實作記錄 T-338（PR #371，2026-10-15）**：`Display::Grid` = 3；StyleProp `grid-template-columns` 264、`-rows` 265、`-areas` 266、`justify-items` 267、`grid-column-start/end` 268／269、`grid-row-start/end` 270／271（Enum：0 auto、n 第 n 線、0x10000|n span n）、`grid-area` 272（StringRef，`auto` 存空字串；`kLast` 272）；`Justify::Stretch` = 6 為 `justify-content`／`align-content` 初始值（flex 當 flex-start，行為不變）。IR 可選區塊 **`GRID`**（DOCT／CLIP 之後，至多一次：`trackLists[]`、`gridTracks[]{f32 value, u8 kind}`、`areaLists[]{offset, count, rows, cols}`、`gridAreas[]{name StringId, rowStart, rowEnd, colStart, colEnd}`），三個樣板宣告 Enum 0 = none／1 + 索引；IR 仍 2.6，舊讀取器略過。`layout_grid.cpp`：`placeGridItems`（區域 → 明確線（超出夾到最後一軌，`kWarnGridPlacementClamped` 每文件一次）→ 鎖列子項 → row-major sparse 游標；隱式 `auto` 軌上限 1000）；`auto` 軌只計跨一軌子項的 max-content；`fr` 無內容下限；`stretchAutoTracks` 在 fr 之後把確定軸剩餘平均分給 `auto` 軌；`align-self` 生效、帶 `aspect-ratio` 的子項不 stretch；absolute／`display: none`／`<camera>`／`<repeat>` 樣板不參與；`order` 不作用。執行時 `setStyle` 收 `display: grid`／`justify-items`／放置屬性，三個樣板與 `grid-area` 回 `kUnsupported`（無執行時 GRID 池）。匯入：2D 直接對映；1D→flex 只在單列（欄）且子項數 ≤ 軌道數（`CssContext::childCount`，只有 inline style 知道子項數，class 規則仍走 flex）；`minmax`／`auto-fit`／`auto-flow`／`grid-template` 簡寫 → block 警告。快照 `grid_tracks`／`grid_areas`／`grid_align`／`grid_stretch`；golden `ui_grid_inventory`。已知缺口：虛擬化 `<repeat>` 的 spacer 佔格；devtools `dumps.cpp` 不在 dev_check 編譯範圍。

**裁決（主模型預設，使用者可否決）**：grid 子集**做**（使用者 2026-10-14「好」：子集 + 給不懂 CSS 的人用的編輯器工具，06 §6.15；T-338／T-339 排在 T-328 之後）；`position: sticky` **不做**；捲軸外觀先寫規格（05／08）再排號；`@container`、錨定定位、`interpolate-size`、巢狀語法與 `@layer`、`render-cache`、`color-mix()/oklch()`、`text-wrap: balance`、自動狀態過渡維持 16.2.6 B 待排。

**實作記錄 T-321（PR #354，2026-10-14）**：`hasUnsupportedUnit` 只擋 `rem/ch/ex/cm/mm/in/pt/pc`；`calc()` 非長屬性丟棄並警告；陰影也吃 em／vw（`isAnyLength`）。選擇器預過濾 `Importer::supportedSelectors()`（頂層與 `@media` 共用；逗號清單只丟不支援的那一個、其餘保留；`@media` 區塊規則全丟則整塊省略；每丟一個 `droppedDecls` +1）；`xts_parser` 不變。conic：dc `normalizeConic`（角度 stop 轉整圈百分比、雙位置 stop 拆兩個）、Figma `from <deg> at <cx>px <cy>px`；DIAMOND 仍點陣化（hud 夾具 `3:15` 改 DIAMOND，ANGULAR 由複製的 dump 測）。`text-shadow` → `text-effect: shadow x y blur color`（07 §7.8；最多 16 層，超過警告丟棄）。`dialect.md` 三份（含 `Skills/design-system/README.md`，`check_skill_sync` 要求逐位元組一致）。夾具 `css3-pass`：匯入由失敗（7 個編譯錯誤）變成功（7 個 selector dropped、0 low）；`css3-media`。

**文件同步**：`dialect.md`（integrations 與 `.claude/skills` 兩份）第 62 行的單位限制與 filter 段落與實作不一致，T-321 一併改。每張單併入後在本節下加實作記錄。

## 5.x 數字語法與 `animation` 次數 0（2026-10-02 使用者裁定；T-272 #278）

- **數字一律是 CSS Syntax Level 3 的 `<number>`，含指數**：`.xts`、執行期 `setStyle`、綁定與 trigger 的引數、元素的數字屬性（`<camera>` 等）、`.xtfx` 參數值，共用同一個讀取器（`Code/Runtime/core/src/css_number.h`）。格式：可選正負號、數字與可選小數、可選的 `e`／`E`＋可選正負號＋至少一位數字（`1e3`、`1E-1`、`1.5e+2`、`1e3px`）。`e` 後面不是數字時它是單位的開頭，所以 `1em`、`2ex`、`1e` 不變。轉換走 `xt::fromChars`，與 locale 無關。
- **超出 float 範圍**（`1e999`、400 位數字）是無效值，依位置：`.xts` 丟掉該宣告並報 `invalid value for "<prop>"`；`setStyle` 回 `kInvalidArgument`；綁定／trigger 引數「不是數字」；屬性忽略並發原本的 warning。**小到 float 表示不了**（`1e-50`、`1e-999`）讀成 0。
- 這些位置不再接受十六進位浮點、`inf`、`nan`（`strtof` 以前會收）。`select()` 綁定鍵寫 `inf`／`nan` 因此是字串鍵。
- **`animation` 次數 0**（含截斷後為 0 的小數）：該項不播放（同 CSS）。編譯器移除該項並 warning `animation "<name>": iteration count 0, the animation is not played`；清單因此變空時等同 `animation: none`。執行期 `setStyle("animation: …")` 靜默丟掉該項。`infinite` 不變。**IR 與宿主 API（`PlayParams::iterations`）裡 0 仍表示無限**，IR 格式與版本不變。
- 已知：編輯器模型把 `infinite` 與手打的 `0` 都存成 `iterations == 0`，且用 locale 相依的 `std::stof`（T-271b 改為獨立的 `infinite` 旗標與 `xt::fromChars`）；執行期把 ≥ 65535 的次數飽和到 65535，編譯器夾到 65534，兩者都是有限次數。

## 5.y 執行期 `setStyle` 的限制與警告（2026-10-07 審查修正鏈合併；T-270d #302、T-270e #305、T-270f #308）

- `setStyle` 與樣式綁定只能選擇**編譯文件的 font-family 清單已命名**的家族，以及**編譯文件資產表內**的圖片。其他名稱維持無效果，但每個元素每個屬性記一則警告到 `Document::warnings()`：
  - `font-family "<first>" is not declared by the compiled document; setStyle uses only the first family of a list and can only select families the document names (declare it in a stylesheet, or use @font-fallback)`——以清單**第一個**家族判斷（執行期只用第一個）。
  - `image "<name>" is not an asset of the compiled document`——執行期 `background-image` 只接受裸名或帶引號的名稱（無 `url()`、無 border-image-source）。
  - 支援執行期家族池與完整清單屬功能（BACKLOG D）；16-roadmap 第 97 行的限制仍成立。
- 元素的執行期 gradient／animation／effect 槽被釋放時（移除，或 `<repeat>` 實例回收），其已提交樣式對該槽的參照立刻歸為 none。
- `background-gradient`／`animation` 的執行期宣告每個（元素, 屬性）用兩個交替槽（偏移每次改變，樣式引擎仍偵測到變更、動畫仍重啟）；pool 因此有界。
- `setStyle` 文字值（`font-family`、`background-image`）存於元素自有槽；字串表已有的文字沿用其 id。

### 5.9.9 響應式例外屬性（2026-10-15；25 §25.4.2；T-345）

| 屬性 | 值 | 繼承 | 動畫 | 語意 |
|---|---|---|---|---|
| `ui-scale` | `auto \| none \| clamp(<number>, <number>)` | 是 | 否 | 子樹的 `uiscale` 係數：`auto` = 專案策略；`none` = 1 × dpr（裝置 px）；`clamp` = 專案係數夾在 [min, max]。長度解析（px、em 基底、字級、描邊、陰影、圓角）一律用該係數；`%`／`vw`／`vh`／`uiscale` 單位不變 |
| `media-override` | `auto \| show \| hide` | 是 | 否 | 套用 `@media` 條件式規則時對子樹略過 `display` 切換：`show` 永遠依非媒體規則顯示、`hide` 永遠 `display: none` |

編譯：兩者為 StyleProp（第二段續號）；執行時在 StyleEngine 解析該元素時取用，無每幀成本；`setStyle` 可改（改變 → 子樹 relayout）。匯入器原樣輸出。

### 5.9.10 無邊框宿主：`app-region`（2026-10-19；23 §23.ab；T-376）

| 屬性 | 值 | 繼承 | 動畫 | 語意 |
|---|---|---|---|---|
| `app-region` | `none \| drag \| no-drag` | 是 | 否 | 無邊框視窗的拖曳區宣告（仿 Electron `-webkit-app-region`）。宿主的 hit-test 由命中元素沿祖先找第一個非 `none` 的值：`drag` → 交給 OS 拖曳視窗；`no-drag` 或都是 `none` → 一般區。**`drag` 區內 OS 接手指標，UI 不再收到 PointerDown**，所以拖曳列上的按鈕必須自己宣告 `no-drag`（因為繼承）。宿主不做 hit-test 時此屬性無作用。 |

編譯：StyleProp 275（第二段續號），`ComputedStyle::appRegion`；IR 不升版（同 T-345 的加法）。

實作記錄 T-376：核心 `mini_css`／`style_decode`／`style_engine`（繼承）與編譯器 `xts_props` 都已登記 `app-region`（`none=0, drag=1, no-drag=2`），`Document::appRegionAt(NodeId)` 沿祖先回第一個非 `none` 的值；IR 未升版。

### 5.9.11 `hit-slop`（2026-10-20；Aux Duty XF-054；T-386）

| 屬性 | 值 | 繼承 | 動畫 | 語意 |
|---|---|---|---|---|
| `hit-slop` | `<length>{1,4}`（上右下左，同 margin 縮寫） | 否 | 否 | 只影響指標命中：元素的命中矩形由 border-box 向外擴大指定距離（隨 `clip-path` 隨形命中時不擴）；不影響版面、繪製、焦點導覽評分。細軌道、小按鈕用。 |

編譯：StyleProp 276，`ComputedStyle::hitSlop`（四個 float，邏輯像素，`em`／`%` 以元素自身解析）；IR 不升版。

實作記錄（T-386，2026-10-20）：IR 值為四個 f32（Color 編碼，上右下左）：≥ 0 為 px；(−65536, 0) 為百分比 −f（f = 百分比 / 100）；≤ −65536 為 `em`，存 −(65536 + em)（`xt::kHitSlopEmBias`）；負值長度、其他單位為無效值。`xts` 與 `mini_css`（`setStyle`）同一編碼。樣式引擎把 px／em 乘上元素的 ui-scale 存進 `ComputedStyle::hitSlop`；`%` 留為 −f，命中測試以元素自身 border-box 寬（左右）／高（上下）解析。`hit_test.cpp`：元素自身的命中矩形改為擴大後矩形（有 `clip-path` 時不擴）；元素裁切溢出時點在 padding box 外但在擴大矩形內只命中元素本身、不進子孫。`computedStyleValue(276)` 回傳 Color（上、右、下、左）。焦點導覽評分、damage、版面與繪製不讀它（golden 不變）。偏離：`ComputedStyle::hitSlop` 的 `%` 以負值保留到命中時才解析（樣式階段尚無 box 大小）。

### 5.9.12 連結樣式表內的資產路徑相對於該樣式表（2026-10-27；47 §47.4；T-490）

T-489 實測：`@font-face { src: "fonts/x.ttf" }` 寫在 `<link href>` 進來的 `.xts`（符號庫 `Assets/Symbols/tokens.xts`）時，`src` 以**連結它的文件**目錄為基準（`resolveAsset(options, xtDir, …)`），不是以該 `.xts` 自己的目錄——符號庫因此不能自帶字型，預覽與金樣永遠看不到 Oswald／Libre Baskerville。裁定：依 CSS 語意，**連結樣式表內的相對資產路徑（`@font-face src`、`background-image`、`border-image`）先以該樣式表所在目錄為基準**；順序改為：絕對路徑原樣 → 該樣式表目錄 / rel（存在才取）→ `options.searchDirs[i]` / rel（存在才取）→ 文件目錄 / rel（既有 fallback，不論存在與否）。文件內嵌 `<style>` 的樣式表目錄 = 文件目錄，行為不變；`compileDocumentFromStrings()` 的樣式表沒有目錄 → 跳過第一步。`xtframe-pack` 複製資產的路徑用同一個解析（22 §22.2）。T-490 實作；`FontFaceDef`／背景圖宣告需帶來源樣式表目錄（`RuleSet` 已知每條規則來自哪個 `<link>`，若否，在解析 `<link>` 時記下）。

**實作記錄 T-490（2026-10-27）**：`resolveAsset(options, baseDir, rel, sheetDir)`；`StyleRule::sheetDir`／`FontFaceDef::sheetDir` 由 `parseXts(..., sheetDir)` 蓋章，`resolveLinkPath()` 回傳磁碟檔的目錄；`<link rel="symbols">` 的符號庫 `<style>` 亦取該庫檔目錄（裁定：同屬「連結樣式表」）。記憶體 `files` 條目（編輯器未存檔的樣式表、`compileDocumentFromStrings`）的 sheetDir 為空 → 跳過第一步（編輯器存檔前的即時編譯仍以文件目錄解析；若要與磁碟一致可改以 `rootDir/href` 推目錄，待有需求再開單）。`custom()` 著色器與 T-273 發佈名仍以文件目錄為準。`xtframe-pack` 沒有第二份解析（走 `resolvedPath`）。夾具 `Tests/xt/linked-font/`，測試在 `Tests/unit/test_compile_document.cpp`。

**實作記錄 T-497（2026-10-27）**：`symbol_expand.cpp` `rewriteRules()` 的每實例規則副本帶 `sheetDir`，`symbol(...)` 內的 `background-image`／`border-image-source` 以符號庫檔目錄解析；`@font-face` 不經符號展開，無需改。夾具 `Tests/xt/symbol-texture/`。`mobile/`、`list/` 子目錄的庫檔直接被文件連結且無 `searchDirs` 時仍找不到 `textures/`（token 值相對庫根）——專案走 `shared.assetRoot`，符號測試 harness 以 `searchDirs = {XT_SYMBOLS_DIR}` 補。
