# 27 · 互動：狀態、觸發、資料綁定、flow、AI 標籤（v1.0 定案，2026-09-27）

狀態：**定案**（使用者 2026-09-27：「OK 啦，定案」）。細節依任務單修訂，修改前先改本章。

本章解決兩件事：美術不寫程式就能做出會反應的 UI（狀態 + 觸發 + 綁定），以及美術做的東西如何與遊戲接上（連接點、flow、宿主節點生成）。原則：**flow 只做資料邏輯；純視覺反應不經 flow**。

## 27.1 狀態

### 27.1.1 內建與自訂

| 狀態 | 對應 | 誰切換 |
|---|---|---|
| Normal | 無偽類 | — |
| Hover | `:hover` | 指標（08 章） |
| Pressed | `:active` | 指標 |
| Focus | `:focus` | 焦點（含手把導覽） |
| Checked | `:checked` | 元素自身或綁定 |
| Disabled | `:disabled` | 綁定或觸發 |
| 自訂（Selected、Locked、Empty、Low…） | `:state(Name)`（05 章新增偽類）；執行時是元素上的狀態集合 | 觸發、綁定、宿主 |

- 自訂狀態名同 id 規則；一個元素可同時有多個狀態；樣式以 `#Hp:state(Low) { … }` 表達。
- **狀態群組**（Gum 的互斥分類）：`state-groups="Size: S|M|L; Tone: Normal|Warn|Danger"`，同群組設定一個即清掉其他；觸發 `state(Danger)` 自動處理互斥。
- 狀態間過渡：`transition`（既有）或指定時間軸：`state-enter: Low play(hp-low-in); state-exit: Low play(hp-low-out)`（05 章新增屬性，值為觸發動作，見 27.2.3）。
- 符號的狀態：在符號定義內宣告，實例共享；Button 符號的 Up/Over/Down/Disabled 就是 Normal/Hover/Pressed/Disabled，不需要 Flash 的四格時間軸。
- 編輯器：屬性面板頂端狀態列（既有）加「+ 自訂狀態」；畫布顯示所選狀態；狀態間的 transition 或時間軸在狀態列旁編輯。

## 27.2 觸發

### 27.2.1 語法（05 章新增：`on:` 屬性族）

```html
<div id="Card" on:hover="play(card-hover)" on:leave="play(card-hover, reverse)"
     on:press="sound(ui_click); state(Selected, toggle)"
     on:show="play(intro)" on:hide="play(outro) then hide"
     on:port(hp)="if lt(20) state(Low) else state(Low, off)">
```

也可寫在 `.xts`：`#Card { on-hover: play(card-hover); }`（屬性名 `on-<event>`），方便 AI 與批次；兩處都有時 `.xt` 優先。

### 27.2.2 事件

| 事件 | 來源 | 備註 |
|---|---|---|
| `hover` / `leave` | 指標進入／離開命中區 | 可 `watch-only`（27.2.5） |
| `press` / `release` / `click` | 指標按下／放開／完整點擊 | |
| `focus` / `blur` | 焦點 | 手把導覽也算 |
| `check` / `uncheck` | Checked 變化 | |
| `show` / `hide` | 元素或 surface 由隱藏變顯示／反之（含宿主 `Show/Hide` 請求） | `hide` 是**請求**：動作可 `then hide` 延後真正隱藏 |
| `port(<name>)` | 該連接點值變化 | 帶值，可接條件 |
| `timeline(<name>).end` / `.marker(<m>)` | 時間軸播完／到達標記 | |
| `state(<Name>).enter` / `.exit` | 狀態進出 | |
| `drag.start` / `drag.over` / `drop` | 拖放（27.7） | |
| `key(<key>)` | 鍵盤（僅焦點元素） | |
| `gesture(<tap\|swipe\|pinch\|hold>)` | 觸控手勢 | 27.7 |
| `player(<n>).*` | 本地多人：指定玩家的輸入 | 27.7 |

### 27.2.3 動作

| 動作 | 說明 |
|---|---|
| `play(<timeline>[, reverse\|loop\|once\|hold][, from=<marker>])` | 播放本範圍的具名時間軸；預設 once + hold（停在末幀） |
| `stop(<timeline>)` / `seek(<timeline>, <marker\|time>)` | |
| `state(<Name>[, on\|off\|toggle])` | 自訂狀態 |
| `class(<name>[, on\|off\|toggle])` | 相容既有 flow 的 toggleClass |
| `show()` / `hide()` / `then hide` | 顯示隱藏；`then` 表示前一動作結束後執行 |
| `sound(<cue>[, volume])` | 27.6 |
| `set(<port>, <value>)` / `emit(<port>)` | 對外送值／事件（宿主或 flow 收到）。`<value>`：數字、`true`／`false`、`toggle`、字串、`event.value`、`value`、`pointer.fraction\|local(#id).x\|.y`、`port(<名>)`（同作用域 in／out port）、範本內 `item(<欄位>)`／`item()`／`repeat.item(...)`／`repeat.index`／`repeat.count`；除 `toggle` 外皆可接 `\| <轉換器> ...`（27.3.3 全部，`tween`／`delay` 除外；引數的 port 可寫 `port(<名>)` 或裸名）（T-411） |
| `if <cond> <action> [else <action>]` | 條件只允許轉換器目錄的比較（27.3.3），不是表達式語言 |
| `focus(<id>)` | 焦點導覽 |
| `spawn(<symbol>, at=<id\|pointer\|world>, lifetime=<time>[, timeline=<name>])` | 觸發即生成暫時實例（傷害數字、拾取提示）；物件池回收；實例不入文件 |
| `haptic(<cue>)` | 震動：事件交宿主 |
| `layer(overlay)` | 把元素提升到全域彈出層（逃出父層 `overflow: hidden`；tooltip、下拉） |

- 動作以 `;` 串接；`then` 串成序列；同一事件重複觸發時的既有播放：`play` 預設從頭重播，`hold` 保持。
- 執行時：每個元素一張觸發表（事件 → 動作清單），編譯進 IR（03 章 NodeCommon 尾端新增 `triggers` 索引），執行時是常數時間查表，不逐幀輪詢。

> T-411 實作記錄（B6）：`set()` 的值可為 bind 來源表達式。**語法**：頭為 `port(<名>)`（以觸發元素的作用域解析，in／out 皆可讀；call port 與 event 型別 port → `port(<n>): a call or event port has no value to read`；未宣告 → `port(<n>): no port "<n>" is declared here`）、範本內的 `item(<欄位>)`／`item()`／`repeat.item(...)`／`repeat.index`／`repeat.count`（與 `bind:` 同一解析器 `repeatTerm`，欄位檢查與錯誤訊息相同；範本外 `item(...)` → `item(<f>) is only valid inside a <repeat> template`，`repeat.*` 同 T-383；裸 `index`／`count` 不收；`outer.*` 不收 → 錯誤），或常數（數字、`true`／`false`、引號字串）；之後可接 `| <轉換器> ...`，由 bind 的轉換器解析器編譯（`compileActionValue()`，bind_emit），轉換器引數可寫 `port(<名>)`（只在動作值內）或裸 port 名；`tween`／`delay` 照舊拒收。目錄沒有 `add`，加法用 `offset(port(b))`。**編碼（IR 2.9）**：`SetSource::Port = 9`（arg2 位元 = PORT 索引）、`Item = 10`（鏈的輸入為觸發元素所在 `<repeat>` 實例的項目，第一步為讀欄位／序號／數量的 `item`／`index`／`count` 步驟，同 `kBindRepeatItem` 綁定）；轉換器步驟照 EventValue 放在 TRIG `conditions[]` 的 arg0／`extra`；常數接轉換器時仍是 Number／String，但 `extra` 不為 0（2.9 起執行期會跑）。`kSetSourceLast` 改 10、IR minor 升 9（`Tests/ir/fixtures/v2.9.xtir`）。**求值時機**：值在動作**執行時**求，與 `pointer.*` 相同走 Job／Waiter——事件當下的資料（指標位置、`event.value`）由 Job 攜帶，`port(...)` 的值與 `item(...)`／`repeat.index` 的項目則以**執行當下**為準（`play(t) then set(x, port(a))` 讀到的是時間軸結束時的 `a`）。repeat 實例內的範本 port（T-380）——包含目標、頭與轉換器引數——都改指該實例的副本；實例已退場／不在活的實例內時 Item 來源不寫入。偏離：`setText`／`setStyle` 不是觸發動作（只是 flow 的反射輸入，27.4.1），本項只作用於 `set()`；「out port 寫入」依 27.13 本來就合法（存值再 emit），未改為錯誤，測試以 call port 目標的既有錯誤代替。

### 27.2.4 生命週期握手

- 宿主 `Hide(surface)` → 觸發根元素 `hide` → 若有 `then hide` 動作，surface 進入 `hiding` 狀態、播完 outro 才真正隱藏並回報 `OnHidden`；沒有動作則立即隱藏。`Show` 同理觸發 `show`。
- 宿主可查詢 `isVisible / isHiding`；宿主生成的節點有 `OnShown/OnHidden` 輸出（25.5b 第 6 條）。
- **觸發範圍（2026-10-16 修訂，T-349）**：surface 的 `show`／`hide`（第一幀、宿主 `Show/Hide`）對 **`<body>` 與 `<body>` 的每個直接子元素**都觸發，視為同一個 surface：這些元素上的 `show()`／`hide()`／`then hide` 作用於 surface（`then hide` 計入 surface 的等待鏈，全部結束才 `OnHidden`）。更深的元素仍只在自己由隱藏變顯示時觸發。匯入器把畫板根上的 `show`／`hide` 觸發寫到 `<body>`（規範形）；引擎的範圍讓手寫在根 `div` 上的也能用（相容形）。
- **成員元素上的 `show()`／`hide()` 動作（2026-10-18 精確化，T-374；T-349 的實作只在 hiding 期間套用這條，與上文不符）**：成員 = `<body>` 與其直接子元素。對成員呼叫 `show()`：該元素恢復可見，且 surface 若隱藏則 `requestShow`（觸發 surface 的 `show`、回報 `OnShown`）。對成員呼叫 `hide()`：該元素隱藏；若此後 `<body>` 沒有任何可見的直接子元素（或 `hide()` 的是 `<body>` 本身），surface 進入 `requestHide`（有 `then hide` 則照等待鏈）並回報 `OnHidden`。`<body>` 下有多個頂層子元素時只藏其中一個不會藏 surface。宿主與編輯器展示空間因此能跟著流圖驅動的 UI 切換（例如 `on:port(active)="if eq(1) show() else hide()"` 寫在根 `div` 上）。 實作記錄（T-374，2026-10-18）：`Document::showElement`／`hideElement`（`Code/Runtime/core/src/trigger_runtime.cpp`）；「可見」= 不在 `hide` 動作的隱藏／等待清單、執行期 `visibility` 宣告（否則解析後樣式）非 hidden、`display` 非 none；成員 `hide()` 觸發 surface 隱藏時不另設該元素的 `visibility: hidden`（之後宿主 `Show` 才不會顯示空表面），只藏其中一個頂層子元素時照舊設元素級隱藏；`Document::lastHideSource()` 記 surface 最後一次 hide 的來源（`port <名稱>`／`trigger on:<事件>`／`host`）。

### 27.2.5 只監看不攔截（watch-only）

- `pointer-watch: true`（05 章新增屬性）：元素參與 hover/move 判定但**不吃**按下與點擊，事件穿透到下層 UI 或遊戲；08 章命中測試新增第三種結果 `WatchOnly`。用途：背景裝飾對滑鼠有反應、100 個會動的卡片只要 hover 回饋。
- 與既有 `pointer-events: none`（完全不參與）並存。

## 27.3 資料綁定與轉換器

### 27.3.1 連接點升級（13 章 HostPorts 修訂）

- port 型別：`number, string, bool, color, vec2, vec3, entity, enum(<values>), struct{…}, list<T>, texture, event`（`event` 帶可選參數）。
- 方向：`in`（宿主→UI）、`out`（UI→宿主）、`call`（宿主呼叫 UI 的函式並取回傳；或 UI 呼叫宿主）。
- 宣告：`.xt` 元素上 `port:<name>="<type> [in|out|call] [= <預設值>]"`（預設值：2026-10-16 修訂，T-358；只允許 number／bool／color／string／vec2／vec3／enum 的字面值，例如 `number in = 80`、`bool in = true`、`string in = "Guest"`、`enum(low|mid|high) in = mid`（列舉值以 `|` 分隔，同既有宣告語法）；沒寫則 0／false／""／透明黑 `rgba(0,0,0,0)`；執行期、畫布、截圖、展示一律以預設值起始，宿主 `set` 後覆蓋；`.xts` 無對應）；符號可宣告自己的 port，實例預設 expose 到父層（可 `expose:false`）。
- 讀回：宿主可 `get(port)` 取目前值；UI 端 `emit` 送值。
- 多實例：同一 UI 的多個 surface 各自一套 port；事件帶 `instanceId`。

### 27.3.2 綁定語法（05 章新增：`bind:` 屬性族）

> **2026-10-20 定案（Aux Duty XF-047／048／049／051／055；T-385）**
> - **`<use>` 上的字面 port 值**：`<use symbol="X" icon="prev" count="3">`——屬性名等於符號宣告的 in-port 時，值為該 port 的**編譯期初始值**（依 port 型別解析；list／struct 收 JSON），與 `bind:<port>` 互斥（同時出現 → 錯誤）。不再靜默忽略；`--strict` 下 `<use>` 上既非標準屬性、非 port、非 `bind:`／`prop:`／`state` 的屬性 → 警告「unknown attribute on <use>; not a port of symbol X」。
> - **`<use state="Dragging Open">`**：實例的初始自訂狀態（空白分隔；必須是符號 `requires` 宣告或符號內用到的狀態，否則錯誤）。靜態預覽用。
> - **`bind:disabled="<expr>"`**：真 → 設內建 `kStateDisabled`（與 `disabled` 屬性同義，`:disabled` 規則命中、不可焦點、不派 Click）。`requires="states: Disabled"` 即內建狀態；`bind:state.Disabled` 視為 `bind:disabled` 的別名（編譯器改寫，不另建自訂狀態）。**2026-10-21 修正（Aux Duty XF-066 回歸；T-390）：`Disabled` 這個名字在所有位置都等同內建**——`:state(Disabled)` 選擇器 ≡ `:disabled`（編譯期改寫）、`setCustomState(x,"Disabled",on)` ≡ 設內建位元、`hasCustomState("Disabled")` 讀內建位元、`state(Disabled, on)` 觸發動作同；因此舊寫法零改動照常運作，不發警告。**停用的子樹**：`kStateDisabled` 對指標與鍵盤派送是**子樹性**的（祖先停用 → 後代不收 Pointer*／Click／焦點，同 web `fieldset[disabled]`；XF-068），`:disabled` 選擇器仍只對宣告元素命中（子樹樣式用 `:disabled .x`）。
> - **虛擬化容器高度 0 的診斷（XF-073）**：`<repeat virtualize>` 的視口元素在第一次佈局後高度為 0 時，執行期發一次診斷（`Document` 診斷管道／`xtframe-view` 印出）「virtualized repeat "X" has a zero-height viewport; give the scroll container a height」。跨符號共用欄寬：用文件層 `VARS`（`--col-title: 240px`）即可，符號內 `var()` 可讀文件 token，寫進 skill。
> - **`<use>` 屬性名大小寫（XF-065 bug）**：解析器對無前綴屬性小寫化；port 比對改為**不分大小寫**，命中後以符號宣告的原始名為準（`hasAdv="false"` 對應 `port:hasAdv`）；`bind:`／`prop:` 已保留原大小寫不變。
> - **`bind:visible` 的語意**：假 = `visibility: hidden`（**保留版面**、不命中、不派事件）；要不佔版面用 `bind:class.x` 配 `.x { display: none }`，或新增的 **`bind:display="<expr>"`**（假 → `display: none`，真 → 恢復樣式表的值）。
>
> T-385 實作記錄：字面 port 值在 `emitPorts()` 之後由 `applyUseLiterals()` 處理——`<use>` 的一般屬性（無 `前綴:`，非 `symbol`／`id`／`class`／`style`／`state`）若與實例**直接**宣告的 port 同名（同 T-380 的範圍規則；port 優先於同名標準屬性），依型別解析成該 port 的 `IrPortDefault`（取代宣告的預設值），並從實例根的屬性移除（不再同時當元素屬性）。string 取原文不需引號；list 收 JSON 陣列（同 T-383）；struct 收 JSON 物件（`IrPortDefault` 新增 Struct：JSON 文字的 StringId，載入時同 `setPortText()` 解析；IR minor 不變）；texture／entity 不收 → 錯誤。out／call／event port → `a literal value cannot set an out port or an event port`；與 `bind:<同名>` 並存 → `a literal value and bind:<name> cannot both be given`。`--strict` 下非 port、非標準元素屬性（`title`、`disabled`、`autofocus`、`draggable`、`data-*`、`aria-*` 等）→ 警告 `unknown attribute "<name>" on <use symbol="X">: not a port of X`；非 strict 不發。`state="A B"` 在符號展開時檢查（`requires` 的 states、`state-groups` 成員、`symbol(X)` 規則的 `:state()`、符號內 `bind:state.<N>` 與 `on:` 動作的 `state(<N>, ...)`；內建狀態名 → 錯誤），編成實例根的 `data-xt-state` 屬性，載入時直接設定自訂狀態位元（不觸發 enter 觸發器；repeat 實例自範本複製）。`bind:disabled` 為 `BindTarget::Disabled = 11`（同 `Checked` 的位元路徑，設／清 `kStateDisabled`）；`bind:state.Disabled` 在 `emitPorts()` 之前改寫成 `bind:disabled`（靜默，不登記自訂狀態）。`bind:display` 為 `BindTarget::Display = 12`：假 → `setStyle("display: none")`，真 → 移除執行期 `display` 覆寫；結果為非布林字面的字串時（舊寫法 `flag | select(true: block, false: none)`）照原樣寫成 `display` 值，相容舊文件。

T-390 實作記錄（XF-065／066／068）：`:state(Disabled)` 在 `.xts` 選擇器解析（`xts_parser.cpp` 的 `parsePseudoCondition`）直接改成 `:disabled` 狀態位元（`:not(:state(Disabled))` 同理）；`Document::setCustomState(x, "Disabled", on)`／`hasCustomState(x, "Disabled")` 讀寫 `kStateDisabled`（`Document::applyDisabledState`，經 `ElementRef::setState`；不論文件是否另有同名自訂狀態），不觸發 enter／exit；觸發動作 `state(Disabled, on|off|toggle[, target=#id])` 編成 State 動作帶新旗標 `kStateBuiltinDisabled = 0x40`（arg0 = 0；IR 不升版，驗證器對帶此旗標者不檢查 arg0）；`state-groups` 的成員 `Disabled` 不進群組、不建槽。`<use>` 字面 port（`applyUseLiterals`）以 ASCII 不分大小寫比對，命中後用宣告原名（`bind:<原名>` 並存檢查兩種寫法）；同一實例直接宣告的 port 只差大小寫 → 錯誤 `ports "a" and "A" of symbol Z are ambiguous: <use> attribute names are case-insensitive`（每個符號報一次，該實例不套字面值）；`--strict` 未知屬性警告尾端加 ` (attribute names are case-insensitive)`。停用子樹：`event_dispatcher.cpp` 的 `isDisabled` 與 `focus_nav.cpp` 的 `disabledElement` 改為沿祖先鏈；`isFocusable` 遇祖先帶 `kStateDisabled` → 不可焦點；命中目標在停用子樹內時不派 PointerDown／PointerUp／PointerMove（PointerEnter／Leave 與 hover 狀態照舊，OutsidePress 照常檢查），Click／DoubleClick／checkbox 切換／鍵盤與手把啟動／拖曳起始都以同一祖先檢查擋下。未涵蓋（回報待裁定）：`on:state(Disabled).enter／.exit` 仍建一個自訂狀態槽，內建位元變化不會觸發它。

```html
<div id="Hp_Bar" bind:width="hp | range(0, hp_max, 0%, 100%)"></div>
<span id="Hp_Text" bind:text="format('{0}/{1}', hp, hp_max)"></span>
<div id="Hp_Icon" bind:state.Low="hp | lt(20%)"></div>
<div id="Gold" bind:text="gold | thousands"></div>
<img id="Portrait" bind:src="portrait_tex"></img>
```

- 左邊：屬性、`text`、`src`、`visible`、`state.<Name>`、`class.<name>`、`variant.<Prop>`。
- 右邊：一個或多個 port，經 `|` 串接轉換器；`format` 這類多輸入用函式式寫法。
- 綁定變化只標記該屬性髒（走 01 章的 `kDirtyPaint`/佈局旗標），不重編。
- 2026-10-20 補記（Aux Duty XF-037）：符號內 `emit(<name>)` 的 `<name>` 若不是宣告過的 port，原樣送出（`HostEvent::name` 即字面名稱，不加實例前綴，例如 `emit(winClose)`），無視窗的 `xt::Context` 也收得到，可 headless 測試。

### 27.3.3 轉換器目錄（固定集合，可串接；不是表達式語言）

> **保留名與 port 名衝突（2026-10-20 定案；Aux Duty XF-041 bug；T-383）**：`bind:` 來源位置的 `index`、`count`、`item`，以及轉換器名（`format`、`eq`、`thousands`…）與 `time`（`bind:` 來源的文件時鐘）是保留名。**在同一作用域宣告了同名 in-port 時，宣告的 port 優先**（`bind:text="index"` 讀 port `index`），且編譯器發警告「port name shadows the reserved name `index`; rename it or write `repeat.index`」——**只對來源位置有歧義的 `index`、`count`、`item`、`time` 發**；轉換器名不發（`|` 之後的名稱永遠是轉換器，沒有歧義；2026-10-20 裁定，T-385）。範本內要讀 repeat 的序號可寫明確形 `repeat.index`／`repeat.count`／`repeat.item(...)`（新增的明確別名，永遠指 repeat）。保留名全表列在本節末。

| 類別 | 轉換器 |
|---|---|
| 數值 | `range(inMin, inMax, outMin, outMax)`、`clamp(min, max)`、`round(n)`、`abs`、`scale(k)`、`offset(k)` |
| 比較→布林 | `lt(x)`、`le`、`gt`、`ge`、`eq`、`ne`、`between(a, b)`；`x` 可為常數、百分比（相對 `_max` 同名 port）或 port |
| 布林 | `not`、`and(port)`、`or(port)` |
| 格式 | `format(pattern, …)`、`thousands`、`fixed(n)`、`percent`、`time(mm:ss)`、`upper/lower` |
| 選擇 | `select(v1: a, v2: b, default: c)`（列舉→值）、`step(t1: a, t2: b, …)`（閾值→值） |
| 顏色 | `lerpColor(c1, c2)`、`fromToken(name)` |
| 文字 | `localize`（`@key` 解析）、`concat`、`glyph(action)`（動作名 → 當前裝置的按鍵字形） |
| 時間 | `tween(duration[, easing])`（數值變化時補間，計數器用）、`delay(t)` |
| 元素 | `#Other.width`／`#Other.text` 等：引用其他圖層的屬性（Gum 的屬性引用） |
| 清單 | `count`、`index`、`item(field)`（用於 `<repeat>` 內） |

執行時：轉換器是核心內建函式表（無配置、無腳本），編譯成綁定圖；宿主也可註冊自訂轉換器（C++ 函式，13 章）。

**保留名全表（2026-10-20；T-383）**：

| 類別 | 保留名 |
|---|---|
| repeat | `index`、`count`、`item`（明確形 `repeat.index`、`repeat.count`、`repeat.item(...)` 只在範本內有效） |
| 時鐘 | `time`（`bind:` 來源的文件時鐘；同名也是轉換器 `time(mm:ss)`） |
| 轉換器（不警告：無歧義） | `range`、`clamp`、`round`、`abs`、`scale`、`offset`、`lt`、`le`、`gt`、`ge`、`eq`、`ne`、`between`、`not`、`and`、`or`、`format`、`thousands`、`fixed`、`percent`、`time`、`upper`、`lower`、`select`、`step`、`lerpColor`、`fromToken`、`localize`、`concat`、`glyph`、`tween`、`delay`、`vec2`、`vec3` |

> T-383 實作記錄：`bind:` 來源（含轉換器引數）的裸 `index`／`count` 在範本內先查作用域（元素所在符號實例範圍往外到文件層）的 **in** port，命中即讀 port；`item(...)` 帶括號、轉換器位置（`|` 之後）的名稱本來就不會是 port，不受影響（`x | format('{0}!')` 仍是轉換器，`bind:text="format"` 是 port）。宣告與保留名同名的 in port 時警告 `port name "<name>" shadows a reserved name; bindings read the port; ...`（`index`／`count`／`item` 接 `write repeat.<name> ...`，轉換器名接 `<name>(...) after a '|' is still the converter`）；out／call port 不警告。`<use>` 上 `bind:<port>="index"` 的來源在實例根求值，看不到該實例自己的 port，所以仍是 repeat 序號。分量選擇器 `x`／`y`／`z`／`w` 不列為保留名。範本外寫 `repeat.index` 等 → `repeat.index is only valid inside a <repeat> template`。

### 27.3.4 清單與重複

> **巢狀清單（2026-10-21 定案；Aux Duty XF-071；T-391）**：`list<struct{…, rows: list<struct{…}>}>`——struct 欄位可為 `list<T>`（T 為 struct 或基本型別，深度不限但建議 ≤ 3）。`<repeat>` 範本內可再放 `<repeat port="item(rows)">`（來源寫 `item(<list 欄位>)`；也可指向文件層 list port，每個外層實例各自展開同一份）。內層範本的 `item(...)`／`index`／`count` 指內層；要讀外層用 `outer.item(<欄位>)`／`outer.index`（一層；再往外不支援）。路徑 id：`Rows/2/Sub/0/Cell`（`<外 repeat>/<i>/<內 repeat>/<j>/<模板相對 id>`）。`listUpdate(outer, i, struct)` 更新含子清單的整筆 → 內層以索引為鍵原地重綁；宿主也可用路徑 `Rows/2.rows` 作為 list port 直接 `listInsert/listRemove`。虛擬化只對最外層。
>
> T-391 實作記錄：IR 版本不變（minor 不升；舊 IR 照讀，新功能的 IR 舊執行期會拒收）。**型別**：struct 欄位可寫 `list<T>`（遞迴，struct 或基本型別；`list<struct>` 的 struct 欄位與 struct port 的欄位皆可）；PORT chunk 在 `defaults[]` 之後加選用尾段 `nested[]`（`IrPortNested` 16 bytes：`field`＝該 List 欄位在 `fields[]` 的索引、`entryType`、`fieldOffset/fieldCount`＝其項目欄位，必在該欄位之後，故巢狀有限；`defaults` 為空但有 `nested` 時寫 `defaultCount = 0`），驗證器要求每個 List 欄位恰有一筆。執行期每個 list 欄位值是指向該 port 自有、不可變儲存（`Slot::entryTrees`／`fieldTrees`，變更時整份替換）的 List Value；`getPort`、`setPortText`（巢狀 JSON 陣列）、`listSet/Insert/Update`（項目 struct 的欄位可為 List Value；`listUpdate` 未給的 list 欄位保留原值）、`hotReload`（依欄位名搬移，字串重新 intern）皆支援。`PortRecord` 尾端新增 `fieldLists`（每個 List 欄位一筆，依欄位順序：`name`＝欄位名、`entryType`、`fields`、`enumValues`、遞迴 `fieldLists`）；`xtframe-pack --gen-ports` 先生成內層紀錄型別（C++ `struct Row {...}; struct Group { std::vector<Row> rows; std::vector<std::string> tags; };`，有巢狀時才加 `#include <vector>`；C# `public Row[] Rows;`）。**範本內 `<repeat>`**：`port="item(<欄位>)"`（或 `repeat.item(...)`）指外層項目的 list 欄位，RPT 列帶新旗標 `kRepeatItemField = 4`、`listPort`＝該欄位在外層項目欄位中的索引（驗證器沿外層列一路解析到 List 欄位）；`port="<文件 list port>"` 照舊為 port 列。內層 `virtualize` → `virtualize is only supported on the outermost <repeat>, not inside another repeat's template`；`item(<欄位>)` 在最外層 → `item(<field>) as a list is only valid inside another <repeat> template`；非 list 欄位 → `field "<f>" is not a list<...>`；範本內 spawn pool 仍是錯誤。`outer.item(<欄位>)`／`outer.index` 編成新引數種類 `ConverterArg::OuterItem = 8`／`OuterIndex = 9`（當鏈頭時為 `item`／`index` op 帶這一個引數）；不在巢狀範本內 → `outer.index is only valid inside a <repeat> template nested in another <repeat> template`，外層無此欄位 → `outer.item(<f>): the enclosing list's entries have no field "<f>"`；`outer.<其他>` 仍當限定名。**執行期**：巢狀 `<repeat>` 的 RPT 列只是原型（`proto`，永不實例化）；外層每個實例建立時為它建立一份活的副本（`RepeatController::createInnerRepeats`，記在實例的 `inner`；實例銷毀時釋放，進回收池時連同內層實例節點一起釋放、取出再用時重建）；`item(<欄位>)` 副本讀自己的**巢狀 list slot**（`BindingRuntime::addNestedList`，名稱 `<外 repeat id>/<i>.<欄位>`，例如 `Rows/2.rows`，`lookup` 可得、不列入 `ports()`），外層實例每次求值時以外層項目的欄位重新填入（深比較，有變更才以 `Set` 語意、以索引為鍵原地重綁；沒變只重算讀 `outer.*` 的綁定）；文件 list port 副本直接展開該 port（每個外層實例各一份）。宿主對巢狀 slot 的 `listSet/Insert/Remove/Update`／`set`／`setText` 會寫回外層項目（多層則逐層往上），外層 port 隨即以等同 `listUpdate(外層, i)` 的方式通知（`PortEvent::ListChanged`、`index = i`，重算該外層實例）。路徑 id `Rows/2/Sub/0/Cell`，外層索引位移時內層 repeat id、實例 id、巢狀 slot 名稱、`outer.index` 一併更新；符號實例內的模板 id 前綴依原型的 id 去除（T-378 同規則）。T-380 實例 port 在內層同樣每實例一份（`Rows/1/Sub/1/Cell.label`），`emit`/`set` 找 port 副本時由內往外找。外層 `item-exit` 期間，退場實例裡的內層副本凍結（不隨清單、不被路徑 id 找到）。**事件**：內層實例的 `emit` 的 `PortEvent::itemIndex`／`item` 與 `HostEvent::itemIndex` 是**最外層**項目的索引與項目；內層索引從 `HostEvent::elementId` 的路徑取（`Rows/1/Sub/2/Cell`）。拖放的 `sourceItem`／`targetItem`（`Document::itemIndexOf`）是最內層實例的索引。未涵蓋：`outer.count` 與再往外一層（依定案不支援）；觸發器 `if` 條件內讀不到 `outer.*`（同 `item`）；巢狀 slot 沒有 `PortRecord` 的宣告位置（`portInfo` 回 `qualifiedName`＝路徑名）；外層 virtualize 時未實例化／在回收池中的列，其巢狀 slot 不存在（`lookup` 失敗，資料仍在外層 port）；flow 的 `.xtflow` 不能接巢狀 list（同 list port）。

> 2026-10-20 定案（Aux Duty XF-002／XF-014；T-380／T-381）：(1) `<use>` 上的 `bind:<port>` 可指向該符號實例宣告的 **in-port**（屬性與 port 同名時 port 優先；符號未宣告者仍視為屬性）；`<repeat>` 範本內的 `<use>` 因此可寫 `bind:title="item(title)"`，每個實例各自一份 port 值，路徑 id `Item/3/Cell.title` 宿主也可直接送。範本內**直接宣告** `port:` 仍不允許（port 屬於符號，不屬於範本）。(2) `list<T>` 的 T 可為基本型別（`number, string, bool, color, vec2, vec3, enum`）；範本內 `item()`（無欄位名）取整個項目；`bind:` 可用 `item(n)` 以常數索引讀清單 port 的第 n 項（越界 = **型別的空值**：數字 0、字串空、bool false、色 透明、enum 第 0 值；2026-10-20 改定，Aux Duty XF-043：保留前值會留下過期資料，與 XF-041 同類問題；T-383）。list port 可宣告預設值（JSON 陣列字面）。`list<struct>` 的 `item(field)` 不變。

```html
<!-- 2026-10-20 改用範式符號 ListItem（30 §30.13；Assets/Symbols/list/ListItem.xt） -->
<div id="Saves" port:items="list&lt;struct{title: string, subtitle: string, badge: string, selected: bool}&gt; in">
  <repeat port="items" id="Rows">
    <use symbol="ListItem" id="Item" bind:title="item(title)" bind:subtitle="item(subtitle)"
         bind:selected="item(selected)">
      <slot name="trailing"><span id="Badge" bind:text="item(badge)"></span></slot>
    </use>
  </repeat>
</div>
```

- `items` 是 `list<struct>` port；宿主一次送整個清單或增刪（13 章 `listInsert/listRemove/listUpdate`）；核心產生／回收實例（`Item/3/Cell` 路徑 id），支援虛擬化（只實例化可見範圍，捲動容器內）。
- 事件從實例回傳時帶 `index` 與 `item` 鍵。
- 2026-10-20 補記（Aux Duty XF-008／XF-027）：`<use>` 的 slot 子樹與 repeat 範本內的元素都可以 `bind:` 讀 `item()`／`index` 與文件層 port（例如 `bind:class.card-l="cardSize | eq(l)"`、slot 內 `bind:text="confirmMessage"`）；slot 內容屬於使用方文件，不經符號的 port。
- 2026-10-20 補記（Aux Duty XF-028）：虛擬化要明寫 `<repeat virtualize>`（預設關，放進捲動容器不會自動開）；只支援等高、直向堆疊，flex-wrap 的格狀 repeat 不可開。
- **進出場**：`<repeat item-enter="play(item-in)" item-exit="play(item-out) then remove">`，清單增刪有動畫，其他項以自動狀態過渡（FLIP，16.2.6 B）補位。

> T-380 實作記錄（定案 (1)）：新 `BindTarget::Port = 10`（`targetId` = PORT 索引；IR 驗證要求 in、非 event、元素本身；IR minor 不變，同 T-204 的 `Checked`）。編譯：`<use>` 展開後的實例根上 `bind:<name>`，若該實例範圍**直接**宣告了同名 port（不論 expose；巢狀實例的 port 不算）即為 port 目標，優先於同名屬性；out／call／event → `bind: cannot target an out port or an event port (bind:<name>)`。範本內 `port:` 只在宣告元素位於「範本根或其下的符號實例根」之下時允許，其餘錯誤訊息改為 `...; declare the port on the symbol and bind it on <use> with bind:<port>=item(...)`（`<use>` 上直接寫 `port:` 仍算直接宣告）。執行期：範本內宣告的 port 為 template-only（不可 `lookup`／`set`，不列入 `ports()`）；每個 repeat 實例建立時複製一份（新 slot，PortId 動態配發、實例銷毀時釋放並可重用；進回收池時值重設為宣告預設、池中不可 `lookup`），名稱跟著複本目前的路徑 id（`Item/3/Cell.title`、`Item/3/Cell/T.title`），索引位移後自動對應；實例的綁定副本（來源、`_max` 伴隨、轉換器 port 參數、port 目標）、`on:port(...)` 註冊、以及實例內 `set(<port>)`／`emit(<port>)` 都改指該實例的複本。`listUpdate` 只重算該實例；宿主直接 `set` 路徑 id 生效。偏離／未涵蓋：綁定（含重綁：`listSet`、`listRemove` 後的索引位移、回收池重用）**第一次**寫入 port 時只以 initial 重算讀者，不發 port listener 與 `on:port` 觸發（避免新實例建立就觸發與補間）；之後的變更照常通知。觸發器 `if` 條件內的 port 參數不改指複本（讀範本預設值）。`list` 型 in-port 當目標時只存值不通知（同 `set()`）。`validateBindingTarget`（編輯器的無情境驗證）不知道符號，仍把 `bind:<port>` 當未知屬性拒絕（2026-10-20 補記（Aux Duty XF-002）：T-382 起編輯器對 `<use>` 查符號宣告——本文件或其 `<link rel="symbols">` 庫——認得實例 in-port；找不到定義（`xt-standard:` 等）時接受任何 port 名，交給完整編譯檢查）。`hotReload` 不搬移實例複本的值（實例隨清單重建）。

> T-381 實作記錄（定案 (2)）：`list<T>` 的 T 可為 `number, string, bool, color, vec2, vec3, enum(...)`（`texture`、`entity`、`event`、巢狀 `list` 與項目型別上的預設值皆報錯 `a list's entries must be a struct{...} or one of ...`）。IR：`IrPort` 原本的 `_pad` 位元組改名 `entryType`（0 = `list<struct>`，否則項目的 PortValueType；enum 項目的值名照舊放在 `fields`，其他基本型別無欄位），IR 版本不變，舊 IR 讀作 `list<struct>`；IR 驗證要求只有 List 可帶 entryType、且只能是上列型別、非 enum 時 `fieldCount == 0`。`PortRecord::entryType` 對宿主公開。執行期：每個項目存一個 Value（Number／Bool 為 Float，String 為 StringRef，Color／Vec2／Vec3 原樣，enum 存值名的 StringRef；`listSet/listInsert/listUpdate` 接受 enum 的序數或值名），`getPort` 回傳由這些 Value 組成的 List。`setPortText`：基本型別 list 接受 JSON 陣列（`["a","b"]`、`[1,2]`、顏色字串、`[x,y]`、enum 值名或序數；JSON 數字給 string list 時轉文字），因此 `xtframe-view --port 'labels=["a","b"]'` 可用。範本內 `item()`（無參數）= 整個項目（只限基本型別 list；`list<struct>` 報錯 `item() needs a field name`，基本型別 list 寫 `item(<field>)` 報錯 `the list entries are not structs`）；編碼為 `ConverterArg::Item`／`ConverterOp::Item` 搭空字串欄位名。`item(n)`：重用 `ConverterOp::Item`，參數種類為 Number（無新 op、IR 不變）；只能緊接在清單 port 開頭之後（`labels | item(1)`），其他位置報錯 `item(<n>) reads an entry of a list port`；n 須為非負整數常數；越界時轉換鏈就此停止、綁定不套用（保留前值），清單變更（含 `listUpdate`）重新求值。`list<struct>` 的 `item(n)` 必須接 `item(<field>)`（`items | item(1) | item(name)`），否則報錯 `item(<n>) of a list<struct{...}> port yields a struct; follow it with item(<field>)`；清單值直接接 `item(<field>)` 報錯。偏離／未涵蓋：`list<bool>` 項目經 `item()`／`item(n)` 讀出時是布林值，但宿主 `getPort` 的項目是 Float 0/1（同 struct 欄位）。`hotReload` 只在新舊 entryType 相同時搬移基本型別項目（enum 值名須仍存在）。`xtframe-pack --gen-ports` 對基本型別 list 不產生紀錄型別，註解寫 `list<string>` 等。

> T-383 實作記錄（(2) 改定）：`item(<n>)` 越界產生型別空值（`list<enum>` 為第 0 個值名；`list<struct>` 的 `item(<n>) | item(<field>)` 為該欄位型別的空值），之後的轉換器照常執行，`listSet` 補足項目後恢復。list port 預設值 `= [ ... ]`（JSON 陣列；`list<struct>` 為物件陣列）編譯時檢查括號與每項是否符合項目型別（物件的欄位在載入時才比對），整段文字存成 `IrPortDefault` 的 StringId（`type` = List，IR minor 不變，同 T-380），載入時以 `setPortText` 的同一解析靜默套用（不通知、repeat 隨後照預設建立實例）。

## 27.4 flow

- **角色**：資料邏輯（條件、計時、計數、狀態機式流程、跨 UI 協調），不做純視覺反應。
- **範圍**：每個 UI 一張（既有 `.xtflow`）+ **bundle 全域流圖** `bundle.xtflow`（可引用 `ui#id.port`、UI 生命週期事件、宿主系統事件：載入、暫停、斷線、切語言、平台）。
- **在遊戲執行**：`xt_flow` runtime 隨 bundle 載入在宿主端執行（宿主插件內建）；與遊戲的邊界永遠是連接點；效能：訊號驅動。宿主可關閉（只用宿主邏輯）。
- **節點畫布 UI**（必要）：Qt 自繪節點圖（QGraphicsView）；節點類型與既有 17.6 相同；連接點面板可拖出節點；子圖／群組；搜尋；除錯時顯示即時值與觸發脈衝；表格檢視保留為「進階」。
- **宿主節點生成**：宿主插件在載入 bundle 時，依連接點表**生成**自己腳本系統的節點或元件（含 instanceId）；不再手寫節點。系統事件節點、世界座標轉 UI 座標節點、多實例節點由插件提供。

### 27.4.1 流圖執行時規格（自舊 17.6 移入，維持）

> **值型訊號按幀合併（2026-10-21 定案；使用者 `ui_menu-demo_2` 滑鼠跟隨 flow 幾秒內上萬次訊號、fps 不掉但卡頓；T-389）**
> - 文件層**值型**連接點（`pointerX/Y/NX/NY`、`pointerDown`、`viewportWidth/Height`、`clock`、`localTime`、元素 `hover`、port 值）是**連續訊號**：一次 `Context::update` 內無論收到多少輸入事件，只在輸入佇列全部派送完之後、flow `tick` 之前**推一次最新值**；同一節點在同一次推送中多個輸入變化只重算一次（先收集變更的來源，再拓樸序求值一輪）。**脈衝型**（Click、Finished、Marker、事件 port）維持逐事件、同步派送。
> - 同一幀內一個 sink（`setStyle`、`set(port)`…）只套用最後一個值。
> - 結果：1000 Hz 滑鼠也只算每幀一次；`demo3-menu` 的三條 `format → setStyle(effect)` 從每次移動 6 次降為每幀 3 次。延遲最多一幀，與既有「同一次輸入派送內反應」的差別人眼不可辨。
> - 編輯器 flow 面板的邊表與值表更新改為差異更新並節流（≤ 10 Hz）；展示空間不因訊號數記錄任何東西。
> - **同形狀的機制一併適用（使用者 2026-10-21「確保類似機制的地方都做妥善處理」）**：(a) 文件 port 的 `PortEvent` → flow Port-owner 輸出：值型同樣進 flush，脈衝逐事件；(b) 綁定執行時：同一幀內同一 port 多次 `set` 只重算讀它的綁定一次（dirty 標記、`update` 開頭求值），`tween`／`delay` 不變；(c) 觸發器：**連續事件**（PointerMove、Scroll、Wheel、DragOver、hover 值）的觸發器動作每幀對同一元素只以最後一個事件執行一次（`then` 等待鏈與脈衝事件不受影響）；(d) `ElementRef::setStyle`：同一元素連續收到相同字串 → no-op；解析結果以字串雜湊快取（上限 256 條，LRU）；(e) `effect` 只有參數變化時更新參數，不重建效果程式；(f) 編輯器與服務：MCP／服務的事件緩衝有上限（環形，≤ 4096 條）且值型訊號不逐筆記錄；連接點面板、flow 面板、時間軸面板的輪詢皆 ≤ 10 Hz 且差異更新。
> - **實作記錄（T-389）**：flow 的值型輸出（`#document.*`、元素 `hover`、`<anim>.localTime`、非事件宣告 port）在 `FlowRuntime::Impl` 以 (端點狀態, 埠) 為鍵只留最後值，`tick()` 開頭整批送出，整個 tick 為一批：重算型節點（math／compare／toString／select／lookup／format）只標 dirty，最後依載入時算好的拓樸序各求值一次；脈衝／事件輸出照舊同步，但送出前先把已暫存的值送完（批次中遇到脈衝也先結算 dirty 節點），維持「值先於脈衝」的次序。flow 的 tick 本來就在 `Context::update` 的輸入派送之後呼叫，未新增 core hook。`edgeLastFired` 每次投遞照舊更新；`values()` 只在 tick 末（或被讀時）同步有推送過的節點。(a) Port-owner 輸出同上（值型進批次、event 逐筆）。(b) `BindingRuntime::notify` 改為標 dirty，`Document::update` 開頭（推進 clock 之前，故 `tween` 起點不變）求值一次，`getPort` 仍即時；port listener 與 `port()` 觸發器不變。(c) `pointermove`／`scroll`／`drag.over` 觸發器以 (元素, 世代, 觸發器) 保留最後一個事件，`Document::update` 開頭執行；非 PointerMove／Wheel 的輸入派送前先執行已保留者（按下→拖曳→放開在同一幀內，最後一次 move 仍看得到 active）；沒有 Wheel 觸發事件（Wheel 產生 Scroll）。(d) `ElementData` 記上次 setStyle 文字的 64-bit 雜湊、長度與狀態，相同即 no-op（其他寫 `runtimeDecls` 的路徑一律清除）；含 `animation` 的文字例外（17.6「再宣告即重播」）；解析結果以 thread_local 256 條 LRU 快取。(e) 本來就沒問題：效果參數寫回元素自己的 runtime 段（T-270a），GL 效果程式只在 init 編譯、每次只設 uniform（有 shadow 去重），SW 每次繪製本來就算。(f) 本來就沒問題：`doc.changes` 上限 2000、回覆 500，只隨 undo 堆疊變化；服務不訂閱 port／flow 訊號；Demo 空間的 flow 作用在預覽 Document，不產生 history；連接點面板每幀只對變了的格 setText。flow 面板改為 100 ms 刷新一次（值、邊標記、脈衝），值表差異更新且列索引只建一次，> 200 邊時改由 delegate 畫底色。量測（`xtframe-view --project <ui_menu-demo_2> --ui demo3-menu --headless --perf --frames 300 --synth-pointer 16`，Release，平均 ms／幀，取最後 240 幀）：修前 input（含 flow 傳播）1.283、flow tick 0.002、update 1.413、setStyle 144 次／幀；修後 input 0.267、flow tick 0.043、update 0.475、setStyle 3 次／幀（flow 相關時間 ≈1.02 → ≈0.05 ms，約 1/20）。編輯器 Demo 空間（Debug、headless、`session.tick`＋flow 面板 tick）修後每幀 0.65 ms（無移動）／1.11 ms（16 move）。

連接點反射（`#<id>.<port>`：元素的 Click/PointerEnter/PointerLeave/Focus/Blur/AnimationEnd/Change/hover 輸出與 setText/addClass/removeClass/toggleClass/setStyle/setValue/focus/setProperty/playAnimation/stopAnimation 輸入；`<anim>` 的 Marker/localTime/Finished 與 play/pause/resume/seek/setSpeed；文件的 clock/viewport/pointer*）、訊號節點集（constant、counter、toggle、timer、clock、random、math、compare、toString、select、radio、lookup、merge、format）、`.xtflow` JSON 格式、型別與環的規則、`FlowRuntime` 介面——全部維持舊 17.6 的定義（見 git 歷史 `Docs/design/17-editor.md` v0.1 §17.6），本輪只新增：全域流圖、`ui#id.port` 引用、系統事件節點、`spawn`／`haptic`／`sound` 動作節點、多實例 id。

### 27.4.2 flow 執行時效能（M37，2026-10-25 定案；使用者「使用所有合理招式來優化 flow 運作的效能」）

**現況（讀碼，T-403 基線 大圖 0.257 ms／幀、典型 0.141）**：`FlowRuntime::Impl` 從頭到尾以**字串**為鍵——節點的 `params`／`inputValues`／`outputValues` 是 `std::map<std::string, PortValue>`，每次求值 `inputValues.find("a")` 都建一個 `std::string`；`paramStr(n, "op")` 回傳字串複本再逐一 `==` 比對；扇出索引是 `map<pair<int,string>, vector>`；每個文件訊號要組一次 `externalKey()`（三段字串串接）再查 `map<pair<ptr,string>>`／`lastOutputs`；`PortValue` 帶一個 `std::string`，數值也跟著複製；`Endpoint` 每次推送都建構含 `std::string port`。邊界也是文字：`set(port)` 把數字印成 17 位文字再讓核心解析；`format → setStyle` 每幀組 CSS 文字、核心逐字元解析（快取只對相同文字有效，跟著指標走的值永遠 miss）；`elementRefs.at(id)`／`lookupPort(name)` 每次投遞都查字串表；`pollDocumentOutputs` 每 tick 對每個 port 做字串比對與鍵串接；`syncValueList()` 每 tick 複製全部值給編輯器，遊戲裡沒人看。綁定執行時（core）已是槽位索引＋dirty 佇列，不是主要債。

**目標**：同一份語意（§27.4.1 的批次、合併、回饋保護、順序、錯誤、編輯器可見的 `values()`／`edgeLastFired`／面板）不變，格式零變更（43 章），把執行時改成**載入時編譯成整數程式、tick 內零字串零配置**，邊界改走型別化 API。

**裁決（全部做）**：

1. **圖編譯（載入時）**：每種 `NodeType` 有固定的輸入／輸出／參數槽位表（`port → slot` 在編譯期以 `constexpr` 表給出）；`NodeRuntime` 改為 SoA：全 runtime 一個 `std::vector<RtValue> values`，節點只存 `inputBase`／`outputBase`／`paramBase` 偏移；`Math.op`／`Compare.op` 等字串參數在載入時解析成 enum；數值參數快取成 `double`；`Format`／`ToString` 的格式字串在載入時切成段（字面＋槽位），求值時只拼接。扇出改為 CSR：`fanOutStart[(node,outSlot)]` 進 `fanOutEdges[]`；`BoundEdge` 的端點以整數（節點、槽位、外部槽位）表示。
2. **外部端點槽位化**：綁定時每個被引用的文件端點（元素 port、`#document.*`、`<anim>`、宣告 port、生命週期）配一個 `ExternalSlot` 整數；文件監聽回呼以**閉包捕捉槽位**，不再在回呼裡組 `externalKey`；待送訊號表 = 每槽位一個 `lastValue`＋pending 位元＋槽位索引陣列；`lastOutputs`／`pendingSignalIndex`／`externalFanOut` 三張字串 map 全部消失。
3. **`RtValue`**：`{type, b, n, StringId}`——字串進 runtime 自己的 interner／短字串池（`FlowGraph` 與編輯器 API 的 `PortValue` 不變，只在 `values()`／`load()` 邊界轉換）；數值路徑不再碰 `std::string`。`Endpoint` 不再含字串。
4. **型別化邊界**：`set(port)` 沉降點在綁定時快取 `PortId`／`PortDir`／`PortValueType`，數值與布林走 `Document::setPort(PortId, Value)`，字串才走 `setPortText`；元素沉降點快取 `ElementRef`（或 NodeId＋世代）；`setProperty` 快取 `PropId`；**`Format → setStyle` 編譯**：載入時若 format 字串能解析成「單一宣告清單、佔位符只出現在數值位置」（例：`transform: translate({a}px, {b}px)`、`width: {a}%`、`opacity: {a}`），就編成 `TypedStyleWrite{prop, unit, slots}`，求值時直接 `ref.setProperty(prop, Value)`／長度值，不組文字不解析；其他情況維持文字路徑（既有雜湊快取）。
5. **tick 內零配置**：`fanOut`／`deliverToNode`／`evaluateNode`／`flushSignals`／`pollDocumentOutputs` 在穩態不得呼叫 `new`／`malloc`（debug 建置以配置計數器測試斷言）；`Format`／`ToString` 用節點自有的 scratch 緩衝並以 move 送出；`syncValueList()` 只在有觀察者（編輯器 `values()` 曾被呼叫或面板訂閱）時執行；`Timer`／`Clock` 的參數讀快取值。
6. **量測**：`FrameStats` 加 `flowSignals`／`flowNodeEvals`／`flowSinkWrites`／`flowAllocs`（debug）與 `flowDeliverMs`／`flowEvalMs`／`flowSinkMs`；`Tests/perf/flow_big` 的 `flow_bench.py` 印分段；21 §21.6 的預算收緊為 **典型 ≤ 0.2、大圖 ≤ 0.8 ms**（基線目標：大圖 ≤ 0.08、典型 ≤ 0.05，即 ≥ 3×），p95 照記。
7. **不做**：不改 `.xtflow` 格式；不改 §27.4.1 語意；不做多執行緒 flow（圖太小，同步成本高於收益）；不改綁定執行時結構（只量測，有熱點再開小單）。

分期：T-475 量測與計數器 → T-476 圖編譯與槽位化 → T-477 型別化邊界 → T-478 零配置驗證、預算收緊、綁定端量測結論。每張 Opus，依序（同一批檔案）。

> **實作記錄（T-476）**：`flow_slots.h` 的 constexpr 槽位表＋`RtValue`／`StringPool`（常數 intern，動態字串為各儲存位置自有的緩衝，不 intern 以免無界成長）＋SoA 節點（op enum、format 預切段、scratch swap）＋外部槽位（`ExternalSlotInfo` 保留描述子供錯誤訊息與文件綁定；文件層 `DocOutput` 輸出槽位由監聽閉包捕捉、`lastOutputs` 改為槽位陣列；待送表以「來源綁定」=（端點狀態, 外部槽位）為單位，全域 flow 多實例各一）＋CSR 扇出＋`valuesObserved`；`flowAllocs` 只算 flow 自身（文件寫入與文件查詢 `boundingBox` 內的配置另計排除），大／典型 fixture 改 `spec.behavior: 1`，Release behavior 1 flowMs 0.183→0.054／0.088→0.035 ms、穩態 flowAllocs 0（`xt_flow_alloc_tests` Debug 斷言）。

> **實作記錄（T-477）**：`RtEdge::sink`（`SinkCache`：PortId／方向／型別、元素 NodeId＋世代、PropId；綁定時與 `rebind()` 解析，端點狀態的 Document／serial 變了才重解）；`set(port)` 的 number／entity／bool／event 走 `Document::setPort(Value)`（與 17 位文字的 `setPortText()` 經同一 `BindingRuntime::set()`，核心無差異、等價測試在 `test_style_template.cpp`），string／enum／color／vec 仍走文字；`Format`／`ToString → setStyle` 以核心新 API `compileStyleTemplate()`（以哨兵數值探測 mini_css、負值／零各自探測定義域）＋`ElementRef::setStyleTemplate()`（從文字依字面段取回數字直接寫 runtime 宣告，保留 setStyle 的雜湊 no-op 與 `setStyleCalls`，形狀不符回 kNotFound 改走 setStyle）實作「型別化寫入」——不用 `setProperty`，因其層級語意與 setStyle 的 cascade 不同；`<anim>` localTime 輪詢改快取 instance 指標；`Document::nodeToScreen` 無 3D 根時不建 vector；Release behavior 1（交錯 A/B 各兩輪）典型 flowMs 0.049→0.027、flowSinkMs 0.029→0.008，大圖 0.087→0.053／0.044→0.011 ms，`setStyleCalls` 不變，兩 fixture 的 set(port)＋setStyle 寫入 100% typed。

> **實作記錄（T-478）**：`flowAllocs` 重新計入沉降寫入與 `boundingBox()` 讀取，只排除走文字的 `setStyle`（核心 CSS 解析）；計入後浮現的兩處核心配置一併修掉——`BindingRuntime::notifyListeners()` 每次 port 變更複製整份 `std::function` 監聽清單（改為 `std::deque` 就地依索引走訪，通知中移除記墓碑、最外層結束時壓縮，通知中新增者下一次通知才呼叫）、`flushDirty()` 每幀讓 `m_dirty` 歸零容量（改兩個緩衝交換容量）；`xt_flow_alloc_tests` 斷言兩 fixture 200 幀、第 50 幀起每幀 0 配置（含 set(port)／setStyle 寫入，typed 100%）；Release behavior 1（2000 幀、三輪取中）典型 flowMs 0.088→0.022、大圖 0.170→0.043 ms（4.0×／3.9×，達 ≥ 3× 目標）；綁定端 0.009 ms，未達 flowMs 一半（40 %／21 %，bind:width 與宿主 set(port) 觸發的 bind:height 約 3:7），不開 PENDING；報告 `Docs/reviews/2026-10-25-flow-perf-after.md`。

## 27.5 AI 製作時的標籤（轉換器契約）

AI（skill、Claude Design）產出的 HTML/`.xt` 以 `data-xt-*` 標籤宣告意圖；匯入轉換器讀標籤生成 26/27/28 章的結構，**不生成 flow**。

| 標籤 | 值 | 轉換成 |
|---|---|---|
| `data-xt-name` | 名稱 | 圖層 id |
| `data-xt-symbol` | 符號名[; variants=…] | `<symbol>` 定義（第一次出現）與 `<use>` |
| `data-xt-variant` | `Kind=Primary` | 實例 `variant:` |
| `data-xt-slot` | 插槽名 | `<slot>` |
| `data-xt-state` | `Hover` / `Selected` … | 該元素是某狀態的外觀 → 併成狀態規則 |
| `data-xt-trigger` | `hover:play(pulse); leave:play(pulse,reverse)` | `on:` 屬性 |
| `data-xt-timeline` | `intro: 0 opacity 0, 400ms opacity 1` 或引用配方 | `@timeline` |
| `data-xt-port` | `hp:number in` | `port:` |
| `data-xt-bind` | `width=hp\|range(0,hp_max,0%,100%)` | `bind:` |
| `data-xt-token` | `color-primary` | 屬性值改引用 token |
| `data-xt-sound` | `press:ui_click` | `on:press="sound(...)"` |
| `data-xt-anchor` | `bottom-left` / `safe` | 約束（25.4） |
| `data-xt-role` | `hud-health` / `menu-list` … | 版型模板辨識與 lint |
| `data-xt-glyph` | `jump` | `<glyph key="jump">` 按鍵字形 |
| `data-xt-rich` | （文字內的 `<b>`/`<i>`/`<span style>`/`<img>`） | 富文本 runs |

- 轉換器規則：未知標籤警告不失敗；標籤與推導衝突時標籤優先；lint 報「有觸發但無對應時間軸」「狀態沒有差異」「port 未被綁定」。
- 字典隨轉換器成長；每次新增標籤要同步 skill 的 `references/dialect.md` 與本表。
- 實作狀態：15 個全部完成（T-197 `data-xt-name`、T-206 #205 其餘 14 個）。

### 27.5.1 T-206 實作記錄（#205）：`dc_import` 標籤轉換細則

- `data-xt-symbol`：第一次出現 → 同檔 `<head>` 的 `<symbol name variants>`，內部規則寫成 `symbol(Name){}`；之後出現 → `<use>`，只讀自身 style／class／標籤與 slot 文字，其餘子內容忽略不警告；`--symbols-dir <dir>` 改寫成 `<dir>/<Name>.xt` + `<link rel="symbols">`（已存在不加 `--force` 不覆寫）。只有文字的符號元素自動產生 `label` slot（外包 `span`，實例根變 div）。符號內部 id 與文件共用命名空間（避免再匯入撞 id）。再匯入時本地已有同名 `<symbol>` 則保留本地；`symbol(){}`、`@timeline`、狀態規則不加 import 標記，以本地為準。
- `data-xt-variant` 只在符號元素上有效；`data-xt-slot` 定義側 `<slot name>`（預設內容）、實例側 `slot:name="文字"`（只接受文字）。
- `data-xt-state`：內建狀態用偽類（Pressed → `:active`），自訂用 `:state(Name)`；只寫與 Normal 不同的 inline 宣告，同名子孫寫成 `#n:hover #child`；狀態元素本身不輸出。限制：Normal 有而狀態沒有的屬性無法表達「取消」。
- `data-xt-trigger`：同事件合併；沒有 `event:` 開頭的片段視為上一條延續；事件名交給編譯器驗證。**指向不存在時間軸的 `play/stop/seek` 只刪該段動作並記 `tag.triggerWithoutTimeline`**（否則整份匯入因編譯錯誤失敗）；`on:timeline(x).end` 的 x 不存在則刪整個屬性；觸發只找自己範圍內的時間軸（文件層另納入 helmet 內的 `@timeline`）。
- `data-xt-timeline`：`@timeline name { #id { 0ms {...} 400ms ease-out {...} } }`，不同元素同名合成一條多軌；無單位 = ms；`var()` 在時間軸內展開為字面值（編譯器拒收 var）；`loop` 只把單參數 `play(x)` 改為 `play(x, loop)`；不支援引用配方。
- `data-xt-port`：文件層 port 提升到 UI 根，符號內留在元素；重複宣告保留第一個並警告。`data-xt-bind` 原樣保留（未宣告 port 為編譯錯誤）。
- `data-xt-token`／`-token-prop`：改 `var(--x)` 並寫進 `:root`；未指定屬性先 color 再 background；token 不在 `tokens.json` 則警告保留字面值。
- `data-xt-sound`：在該事件動作串最前加 `sound(cue[, vol])`。
- `data-xt-anchor`：強制 `position:absolute`，指定邊寫 `1safe-left`，原有 px 偏移寫 `calc(1safe-left + 24px)`，刪對邊；非 px 偏移被取代並警告。
- `data-xt-role` → `data-role`；`data-xt-glyph` → `<glyph key>`（元素文字丟棄；富文本內為行內字形）；`data-xt-rich`：`b/strong`、`i/em`、`span`（style 限 color、font-size、font-weight、font-style、letter-spacing、text-effect + class）、`img inline`、`br` → runs；新增 `html::Element::leadingSpace` 保住 `</b>` 後的空格，純空白文字節點仍被丟棄；再匯入把「文字夾行內標記」當一個 raw markup 值往返。
- lint `tag.triggerWithoutTimeline`／`tag.stateNoDiff`（`tag.portUnbound` 判斷：port 名未以完整識別字出現在任何 `bind:`／`on:` 鍵或值）寫進 `import-report.json` 的 `lint[]`，計入 warningCount，`--lint` 列出。未知 `data-xt-*` 只警告。標籤優先於推導（已驗證：`<a href>` 導覽、字面色、既有 left/top/right/bottom、id）。
- fixture：`Tests/import/fixtures/dc_tags/`（帶齊全部標籤：匯入 → 編譯 → 再匯入零衝突 → 截圖）。

## 27.6 音效 cue

- 時間軸的音效軌（28.6）與觸發動作 `sound(cue)` 都只送事件 `#id.sound(cue, params)`（out port，自動存在）。
- cue 名稱同 id 規則。宿主：cue 直接對到自己音訊系統的觸發名執行；宿主事件節點可攔截客製。
- 編輯器：資產庫的 cue 對應本機音檔試聽（sidecar），無對應只閃標記。不做混音、音量曲線。

## 27.7 遊戲 UI 互動（都做）

- **輸入提示字形**：`<glyph key="jump"/>`（行內或獨立）依當前輸入裝置（鍵鼠／Xbox／PS／Switch／觸控）顯示對應圖示；字形表由 UI Project 或宿主提供（`glyphs/<device>.xts` 映射 action → 圖片／字型字元）；裝置切換事件 `input-device` 自動重繪。
- **本地多人**：08 章焦點改為每玩家一個（`player` 索引 + 裝置），觸發事件可限定 `player(n)`；PlayerJoin 符號。
- **觸控與震動**：手勢事件、虛擬搖桿符號、`haptic(cue)`。
- **全域字級縮放**：`uiscale` 單位（05）與宿主 `setUiScale`；無障礙：元素 `aria-label`（供朗讀事件）、色盲模擬預覽（編輯器）、減少動態（節制規格）。
- **全域彈出層**：`layer: overlay` 逃出父層裁切；modal 焦點鎖（焦點導覽不離開 modal 子樹）。

- **手把與方向鍵焦點導覽**：`focus-nav: auto | explicit`；auto 依幾何找最近元素；explicit 以 `nav:up="#id"` 等指定；焦點環樣式由 Focus 狀態決定；surface 的初始焦點 `autofocus`。08 章擴充：手把按鍵映射到 `press/click/back`。
- **拖放**：`draggable="true"`、`drop-target="true"`；事件 `drag.start/over/drop` 帶來源與目標 id 與 `item` 鍵；視覺由 Dragging 狀態與 `drag-ghost` 元素；宿主收到 `drop` out event 決定資料變化。
- **捲動與虛擬化**：`overflow: scroll` 容器 + `<repeat>` 虛擬化；慣性與捲軸樣式；手把捲動。
- **疊層與輸入阻擋**：surface 有 `z` 與 `input: block | pass | watch`；上層 `block` 時下層不收指標；modal = 上層 block + 背景壓暗（overlay，不模糊，依實測）。
- **tooltip／modal**：符號庫預設；tooltip 由 `hover` 延遲 600 ms 觸發（可設）。

## 27.8 修訂條款

- 05：`:state()`、`state-groups`、`state-enter/exit`、`on:`／`on-*`、`bind:`、`port:`、`pointer-watch`、`focus-nav`、`nav:*`、`draggable`、`drop-target`、`<repeat>`（含 item-enter/exit）、`<glyph>`、`layer: overlay`、`uiscale` 單位、`text-reveal`。
- 03：`spawn` 物件池與暫時實例；`world-anchor`。
- 08：`WatchOnly` 命中、拖放事件、手把映射、焦點導覽演算法、surface 疊層阻擋。
- 13：port 型別與方向、`get`、清單 API、自訂轉換器註冊、`setTheme`、生命週期握手、多實例 id。
- 23：宿主節點生成、系統事件、世界座標轉換、全域流圖。
- 24：`data-xt-*` 字典與 lint。
- 03：NodeCommon 尾端 `triggers`、`bindings` 索引。

## 27.9 T-174 實作記錄（#175）

- 狀態名與群組為文件級；一個狀態只屬一個群組。第一次 update 觸發 surface 的 `show` 一次。
- **surface 元素** = 根的 `body` 子元素（無則根）；隱藏的 surface 不繪製、不收輸入。`show()`／`hide()` 作用於觸發所在元素；在 surface 元素上等同 `requestShow/requestHide`；元素的 hide 設執行時 `visibility: hidden`，同樣等待 `then hide` 鏈；`requestShow` 取消等待中的 hide。
- 觸發於輸入事件的預設行為之後執行。`play` 預設 hold，`once` 結束釋放；重複觸發重播，顯式 `hold` 讓進行中的時間軸不動；loop 或失敗的 `play` 之後的 `then` 不執行或立即執行；`stop` 丟棄等待該時間軸的 then 鏈。`if` 比較事件值（T-174 事件皆為 0；T-175 供 port 值）。
- `.xts` 的 `on-*` 只經無偽類的選擇器套用（同 18.12 靜態查找）；頂層 `,` 亦可分隔動作或整串加引號；`pointer-watch` 可繼承；`key(k)` 只在 KeyDown 目標觸發。
- T-194 起 `layer` 動作與 drag／gesture／player 事件已支援（08 §8.9.1）；尚未：`set`、`spawn` 動作；`state-enter/exit` 為編譯期宣告非 StyleProp；`Document::query` 不支援 `:state()`；符號內的動作 id 未改寫為路徑 id。
- API：`ContextListener { onHostEvent(HostEvent), onShown(h), onHidden(h) }`（`Context::addListener/removeListener`）；`HostEvent { kind: Sound|Haptic|Emit, name, volume, element, elementId, document }`；`Context::setCustomState/requestShow/requestHide/isVisible/isHiding`；`Document::hitTestPress`（`HitMode::Press`，08.9）。`pointer-watch` = StyleProp 191。

## 27.10 T-175 實作記錄（#178）

見 05 §5.9.3 的 T-175 規則與 13 §13.8.9 的簽名；`if` 現在比較真實 port 值；`index` 轉換器在 T-177 之前恆為 0；`set`／`spawn`／`layer` 動作仍未支援；`localize` 接 T-179 的 provider 並隨 `setLanguage`。黃金圖 `ui_text_bind_hp_72/30`。

## 27.11 T-177 實作記錄（#180）

- 結構：`<repeat>` 必有 `id` 且恰一個子元素（模板根）；實例是模板根的複本，放在 `<repeat>` 元素之下（`<repeat>` 佈局如 div，作者對它設 flex）；巢狀 repeat、模板內 `port:`（T-380：符號宣告者除外，每實例一份，見 27.3.4）、非模板根的 `on:item-enter/exit` → 錯誤。
- 路徑 id：`<repeat id>/<index>/<模板相對 id>`（`Item/3/Cell/Icon`、`Item/3/Name`），索引位移時改名；**符號實例內**的 `<repeat>`：`<repeat id>` 已帶實例前綴（`CList/CRow`），模板相對 id 不得再帶（正確 `CList/CRow/0/Track/Add`，錯誤 `CList/CRow/0/CList/Track/Add`；2026-10-20 精確化，Aux Duty XF-024 bug，T-378）；`#<模板 id>` 選擇器（與符號路徑規則）經 `templateId` 仍匹配實例；`byId` 跳過模板節點、可查實例路徑。
- 以索引為鍵：`listSet` 原地重綁（重排 = 重綁不搬節點，子序永遠跟清單）且不觸發進出場；`listInsert` 觸發 enter、`listRemove` 觸發 exit；exit 無 `remove` 動作 → 警告並在動作開始時移除；退場中的實例凍結綁定原地不動。
- 模板內元素為成員的時間軸逐實例播放（模板外成員忽略、標記與音效不對複本觸發）。
- 虛擬化：項高 = 第一個實例佈局後的 margin-box 高（之前只有第 0 項；高 0 = 全部實例化）；viewport = `viewport=` 元素，否則最近的 `overflow: scroll` 祖先，否則文件視口；視窗 = 可見範圍前後各一頁；兩個 spacer div 維持捲動長度；回收池上限 3× 可見數（重設為模板的 class／屬性／狀態）；於 `Document::update` 佈局後檢查，有變動再一次 resolve + layout。假設等高直向堆疊。
- 未涵蓋：模板內 `anim`／`<camera>` 當一般元素、`@key` 不隨 `setLanguage`、`if` 讀不到 item/index、`focus(相對 id)` 無效；執行時字串池隨路徑 id 增長不回收；FLIP 補位未做。
- API：`Document::repeatInstances(id) → span<const ElementRef>`（只含存活實例）；`PortEvent` 尾端 `itemIndex, item`、`HostEvent` 尾端 `itemIndex`；`setPortText` 接受 JSON（核心內小型解析器）給 struct/list；`xtframe-view --port 'items=[{...}]'`。27.10 的「`index` 恆為 0」已失效。flow 的 `addEmitListener` 尚未帶 itemIndex。
- T-378 實作記錄：`RepeatController::renameInstance` 以 `<repeat id>` 去掉最後一段得到實例前綴（`CList/`、`Outer/Inner/`），模板 id 以它開頭就去掉後再串接；頂層 repeat 不變；符號來源表 `internalPaths[]` 尚未實作，無需同步。
- 路徑 id 慣例的總整理（符號內元素、清單項、符號內清單、port 路徑）見 13 §13.9（T-387，Aux Duty XF-064）。

## 27.12 T-193 實作記錄（#193）：流圖畫布、全域流圖、系統事件

- `.xtflow` 尾端新增（執行時忽略、壞項丟棄）：`nodes[].pos`、`groups[{title, rect, color?}]`；邊參數 `instance: "<n>"`（來源只放行該實例、目標只送該實例；無則全部）。新節點：`system`、`sound{cue, volume}`、`haptic{cue}`、`spawn{symbol}`、`emit{port}`（輸入 trigger／value）、`timeline{name, scope, ui, loop, rate}`（輸入 play/stop/seek、輸出 end、marker（每個標記都發））。
- 全域引用（`splitUiEndpoint`）：`ui#<uiId>.<element>.<port>`；`ui#<uiId>.<port>` = 該 UI 的 `#document.<port>`；`ui#<uiId>.shown/hidden` 只作輸出；全域圖拒絕 `#id.port`、UI 圖拒絕 `ui#`；全域 `timeline` 節點需 `ui`。`FlowRuntime::loadGlobal(json|graph, BundleContext&, errors)`；全域 runtime 不可 rebind（重載代替）。
- 系統事件：pulse `loading/paused/resumed/disconnected`；string `languageChanged/platform/inputDevice`（每次 post 都發）；`FlowRuntime::postSystemEvent`／`BundleContext::postSystemEvent`（送全域與每個 UI）；`xtframe-view --event "system <kind> [value]"`。系統事件尚未以 port 形式進 UI 側（23.10 第 4 條部分）。
- `enableFlow`／`flowRuntime` 不在 `xt::Context`（core 不可依賴 flow），在新的 `xt::flow::BundleContext`（uiId→handle、各 UI runtime、生命週期 listener）；動作節點以 `FlowHostEvent{Sound, Haptic, Spawn, Emit, uiId, instanceId, nodeId}` 經 `setHostEventListener` 送宿主（core 的 `HostEventKind` 無 Spawn）。時間軸事件取自冒泡到根的 TimelineEnd/TimelineMarker；缺時間軸一則執行時錯誤。
- `xtframe-pack`：驗證根或 `shared/` 的 `bundle.xtflow`（`ui#` 必為專案 UI）並複製到 `<out>/bundle.xtflow`；manifest `flows.global`（`Bundle::globalFlow()`）；`--gen-ports cpp|cs <file>`（與 `--ui` 並用 exit 2）：C++ `namespace xtui::<uiId> { enum class …; struct …; constexpr PortId … }`、C# `Xtui.<UiId>` 的 enum／struct／`Ports` 常數；`PackReport.portTables/globalFlowWritten`。`xtframe-view --project <dir>` 載入全部 UI 與流圖並印 `flow event …`。
- 畫布：`FlowCanvas`（QGraphicsView，持圖的副本，每次編輯以 `graphEdited` 回整圖 → `FlowPanel` 一個 `ReplaceFlowGraph`）、`FlowNodeItem`（類別色 token、型別埠、就地參數）、端點節點由邊生成、`FlowEdgeItem`（貝茲、型別色、脈衝閃爍）、`FlowGroupItem`、`FlowMinimap`、Ctrl+F、右鍵分類新增、Delete、滾輪縮放、中鍵平移、錯誤紅框、Demo 即時值；`FlowPanel` 兩個範圍分頁（UI 流圖 + 進階表格；全域流圖經 undo 每次套用即寫檔、解析失敗唯讀）。限制：端點位置與未連接的拖入端點不存檔；邊的 `class`／`prop` 只能在進階表改；全域畫布只知目前 UI 的 port、無即時值；子圖未做。連接點面板列的 tooltip 帶端點文字（Qt 拖曳只含 UserRole 以下角色）。
- 已知：`test_ui_project.cpp:352`「upgraded project packs the same」約 1/3 機率失敗——`.xtir` META 帶秒級時間戳，跨秒兩次打包 fileHash 不同（既有問題，待修：META 時間戳排除於 hash 或允許固定）。

### 27.12.1 T-200 追加（#198）

27.12 的「系統事件尚無 port 形式」已解決：隱含 `system.*` port（05 §5.9.4）與 `Context::postSystemEvent`／`ContextListener::onSystemEvent`（13 §13.8.13）；`BundleContext::postSystemEvent` 改經 Context 路由。

> T-197（#200）：`data-xt-name` 已由 dc 轉換器實作（優先於 `id`），再匯入以它為主鍵（24 §24.10）。

> T-204（#203）：`bind:checked`（02 §2.s.1）。

## 27.13 T-201 實作記錄（#199）：符號範圍觸發、`set`、`spawn`、`state target`、`axis-x`

- **符號範圍**：`play/stop/seek`、`then` 鏈、`timeline(<n>).end/.marker()` 先在觸發元素最近的符號實例範圍找群組，找不到再找根；`repeat_controller` 也用實例範圍。
- **`set(<port>, <value>)`**：值可為數字、`true/false`、`toggle`（限 bool）、字串或 `event.value [| 轉換器]`；對 in port 等同 setPort（觸發本地綁定），對 out port 存值再 emit。
- **`spawn(...)`**：語意與限制見 03 §3.12.8。
- **`state(<Name>, <mode>, target=#id)`**：與 spawn 共用 `scopedId()`；實例外 `#id` 為文件 id。
- **`draggable="axis-x"`**（27.7 擴充）：拖曳時發 `drag.value`（指標在元素 border box 上的比例，已套用 transform）；`set(value, event.value | range(...))` 可綁；`nav.value` 見 08。
- **綁定到變換分量**：`bind:translate-x/-y`、`scale-x/-y`、`rotate`。
- 已解決 T-198 的 15 條 workaround 中 13 條（30 §30.11）。

## 27.14 流圖節點的數值規則（2026-10-02 審查修正；T-258a #269、T-258b #272）

- **timer**：`interval` ≤ 0 仍表示「永不觸發」；`(0, 1 ms)` 在**執行時**當作 1 ms（`kMinTimerInterval`）；每次 `tick` 最多觸發 1000 次（`kMaxTimerFiresPerTick`）。`FlowGraph::parse` 不改寫作者寫的值（編輯器用同一個 parse；開檔再存檔必須逐 byte 往返）。
- **random**：`min > max` 時交換；非有限＝0；整數轉換飽和。
- 寫入 Entity／enum port 的負值或非有限值被拒絕。

### 27.3.5 輸入元素的綁定與事件、`Focused`、`requires` 選擇器群組（2026-10-21 定案；Aux Duty XF-072／075／076／077；T-392）

- `<input>`：`bind:value="<port>"`（port → 文字；使用者輸入不反向寫 port），觸發名 `on:change`（`Change` 事件，既有）與 `on:input`（每次編輯），動作值來源 `value`（本元素目前文字；只在 `on:change`／`on:input` 內有效），例：`<input bind:value="query" on:change="set(query, value); emit(search)">`。宣告式聚焦：`focus()` 無參數 = 本元素；可寫在 `on:state-enter(Open)`（狀態進入觸發名，若無則本單補）或 `on:show`。
- **`Focused`／`Hover`／`Active`／`Checked` 同 `Disabled`**：這些名字在 `requires`、`:state(X)`、`bind:state.X`、`setCustomState` 全部等同內建（`:focus`／`:hover`／`:active`／`:checked`）；不建自訂槽、不警告。
- **`requires` 檢查**對選擇器**群組**（逗號分隔）逐一檢查，任一含該狀態即通過；`:focus-within` 算 `Focused` 的規則。
- **寫明（XF-077）**：`<use>` 上 `bind:state.X` 設的是**實例根**的狀態，所以規則要寫 `:state(X) #Inner`（祖先帶狀態）；`#Inner:state(X)` 是內部元素自己的狀態。
- `placeholder` 在 headless／截圖同樣繪製（XF-076 若為 bug 則修）。

T-392 實作記錄（XF-072～078）：`bind:value` 為 `BindTarget::Value = 13`，只准可編輯 `<input>`（無 `type` 或 `type="text"`，否則錯誤 `binds only on an editable <input>`），執行期 `EventDispatcher::setEditValue` 換值、游標移到尾端、讓顯示字串失效，**不**派 Change。觸發名 `change`＝`TriggerEvent::Change = 37`、`input`＝`TriggerEvent::Input = 38`：核心的 `EventType::Change` 本來就每次編輯（每鍵、setValue、checkbox 切換）派一次，所以沒有另加 `EventType::Input`，兩個觸發器都掛在同一個 Change 上（只在 Change 的目標元素本身觸發，先 `change` 後 `input`）。動作值來源 `value`（可接 `| 轉換器`）＝`SetSource::ElementValue = 8`（`kSetSourceLast` 改 8），讀觸發元素的 `ElementRef::text()`（input 即其值）；用在 `change`／`input` 以外 → 編譯錯誤 `value is only valid in change and input triggers`。`focus()` 無參數 → Focus 動作 arg0 = `kInvalidString`（驗證器放行）＝本元素。觸發名 `state-enter(<名>)`／`state-exit(<名>)` 是 `state(<名>).enter|.exit` 的別名（同一 canonical）。內建別名：`builtinStateAlias()`（`xt/node.h`）把 `Disabled`／`Focused`／`Hover`／`Active`／`Checked` 對到 ElementState 位元，用在 `:state(X)` 解析改寫、`state-groups` 成員（不建槽）、`bind:state.X`（`Disabled` 照舊改寫成 `bind:disabled`；其餘為 `BindTarget::BuiltinState = 14`，targetId = 位元，不登記自訂狀態）、`state(X, mode[, target=])` 動作（沿用旗標 `kStateBuiltin` ＝ T-390 的 0x40：arg0 = 0 仍是 Disabled，其餘 arg0 = 位元；IR 不升版）、`if state(Focused)`、`Document::setCustomState`／`hasCustomState`（`applyBuiltinState`：Disabled／Checked 經 `ElementRef::setState`，Checked 會跑 check／uncheck；Focused／Hover／Active 只設位元並標樣式髒，不移動焦點也不改指標狀態）。`requires` 檢查原因（XF-076）：舊版把 `:state(Focused)` 編成自訂狀態條件，而 `requires` 視 `Focused` 為內建只看狀態位元，`:focus-within` 也不算，所以 `#Box:focus-within, :state(Focused) #Box` 兩個選擇器都不命中；現在 `:state(Focused)` 即 `:focus` 位元，`:focus-within`（非否定）算 Focused，群組本來就逐一檢查。虛擬化：`RepeatController::afterLayout` 在第一次量到條目高度的那一輪檢查一次，有 viewport 元素且其 padding box 高 ≤ 0 → `Document::addWarning("virtualized repeat \"<id 或 list port 名>\" has a zero-height viewport; give the scroll container a height")`；`xtframe-view` 新增 `--strict`（`.xt` 以 strict 編譯，headless 結束時把 `Document::warnings()` 每條印成 `xtframe-view: warning: ...`）。`--event` 解析失敗一律印 `xtframe-view: cannot parse --event '<原文>': <原因> (expected "<frame> #id.<Event>[=value]")`；`--print-text` 找不到 id 時 stderr 印 `xtframe-view: --print-text: no element '<id>'`（stdout 照舊 `text=`）。未做（回報待裁定）：`placeholder` 不是 headless 才不畫——核心從未實作 `<input placeholder>` 的繪製（`layout_text.cpp` 只排 EditState 值或 IR 文字，painter 沒有 placeholder 路徑，屬性只存著並做在地化），視窗模式與編輯器同樣不畫；要畫需先定顏色／不透明度（web 為淡化的 `color`／`::placeholder`）、是否影響內在寬度、顯示時的游標與點擊位置。`on:state(Focused).enter`／`.exit` 仍建一個自訂狀態槽（同 T-390 的 Disabled）。
