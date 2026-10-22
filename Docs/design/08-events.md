# 08 · 事件、命中測試與狀態

## 8.1 宿主輸入介面

宿主把原始輸入排進 `Context` 的佇列，`update()` 依序處理：

```cpp
namespace xt {
enum class PointerButton : uint8_t { Left=0, Right=1, Middle=2 };
enum class KeyCode : uint16_t { /* Unknown..Up（≤82）：USB HID usage 表的子集：字母、數字、方向、Enter、Escape、Tab、Backspace、Delete、Home、End、Space。
                                   T-143 起（83..133，依序給值、**不是** HID）：F1–F12、PageUp/PageDown/Insert、ShiftLeft/Right、ControlLeft/Right、AltLeft/Right、MetaLeft/Right、CapsLock、
                                   Minus、Equal、BracketLeft/Right、Backslash、Semicolon、Quote、Backquote、Comma、Period、Slash、Numpad0–9、NumpadAdd/Subtract/Multiply/Divide/Decimal/Enter（命名照 W3C KeyboardEvent.code）。
                                   宿主對照規則：對不到的鍵送 Unknown（事件照樣產生）；Qt 分不出左右 Shift/Ctrl/Alt，一律對到 *Left；Shift 產生的符號對到產生它的實體鍵（美式配置）。 */ };
enum Modifiers : uint8_t { kShift=1, kCtrl=2, kAlt=4, kSuper=8 };

struct InputEvent {
    enum Kind : uint8_t { PointerMove, PointerDown, PointerUp, Wheel, KeyDown, KeyUp, TextInput, PointerLeave,
                          CompositionStart, CompositionUpdate, CompositionEnd } kind;
    Vec2 pos;                 // 邏輯像素，視口座標
    PointerButton button;
    Vec2 wheelDelta;          // 行為單位，正 y 表示向下捲
    KeyCode key; uint8_t modifiers; uint8_t repeat;
    char utf8[8]; uint8_t utf8Len;   // TextInput
    std::string_view composition;    // CompositionUpdate：目前組字字串（宿主保證在 pushInput 期間有效；Context 立即複製）
    uint32_t compositionCursor, compositionSelLen;   // 組字字串內的游標位元組偏移與選取長度（IME 提供）
    uint32_t pointerId;       // 第一版只用 0
};

void Context::pushInput(const InputEvent&);
}
```

## 8.2 命中測試

`hitTest(pos) → NodeId`：深度優先**後序**、同層子節點**逆序**（後畫的先命中）遍歷 Element 樹：

```
hit(elem, pos):
    若 display none / visibility hidden / pointer-events none：跳過自身但**仍**測試子孫（pointer-events:none 時）
    localPos = inverse(world) * pos
    若 overflow != visible 且 localPos 不在 padding box 內：return 無
    先測子元素（z-index 高者先，同 z 者文件逆序）→ 命中則回傳
    若 localPos 在 border box 內 → 回傳 elem
    return 無
```

場景節點（anim 內的 Path 等）不個別命中；命中落在 `anim` 元素。文字節點命中歸於其父元素。`transform` 有旋轉時以反矩陣轉換點，因此命中區域跟隨變換。

**2026-10-14 M32 T-332 修訂**：元素有 `clip-path`（05 §5.9.8）時，進入該元素前先做「點在形內」測試（inset 圓角矩形、圓、橢圓、多邊形 even-odd，座標在元素自身變換後的 border box 空間）；不在形內則該元素與其子樹一律不命中，其餘規則不變。

## 8.3 事件型別與冒泡

```cpp
enum class EventType : uint8_t {
    PointerDown, PointerUp, Click, PointerMove, PointerEnter, PointerLeave, Wheel,
    KeyDown, KeyUp, TextInput, Focus, Blur, Change,   // Change：input/checkbox 值改變
    CompositionStart, CompositionUpdate, CompositionEnd,
    AnimationStart, AnimationEnd, AnimationIteration, TransitionEnd, Marker
};

struct Event {
    EventType type;
    NodeId    target;        // 原始命中元素
    NodeId    current;       // 目前分派到的元素
    Vec2      pos;           // 視口座標
    Vec2      localPos;      // current 的局部座標
    PointerButton button; Vec2 wheelDelta; KeyCode key; uint8_t modifiers;
    std::string_view text;   // TextInput
    uint32_t  animationId; StringId markerName;
    bool      stopPropagation = false;
    bool      preventDefault  = false;
};

using EventListener = std::function<void(Event&)>;
uint32_t Document::addListener(NodeId, EventType, EventListener);   // 回傳 listener id
void     Document::removeListener(uint32_t);
```

分派：從 `target` 向上到根，對每個元素呼叫其該型別的監聽器（註冊順序）。`stopPropagation` 停止向上。`PointerEnter/Leave/Focus/Blur` **不冒泡**。

監聽器儲存：`std::vector<Listener>` 池 + 每個 Element 的鏈表頭（`ElementData::listenerHead`），移除時標記 `dead`，每 1024 次移除做一次壓實。

## 8.4 指標狀態機

```
PointerMove:
    h = hitTest(pos)
    若 h != hoverNode：
        對舊 hoverNode 及其祖先清 kStateHover，發 PointerLeave（自 hoverNode 到與新路徑分岔處）
        對 h 及其祖先設 kStateHover，發 PointerEnter
        hoverNode = h
    若 activeNode 有效：發 PointerMove 到 activeNode（拖曳語意）；否則到 h
PointerDown:
    h = hitTest(pos)；activeNode = h；設 h 及祖先 kStateActive
    發 PointerDown
    若 h 可 focus（button, input）且未 preventDefault：setFocus(h)；否則 setFocus(無)
PointerUp:
    發 PointerUp 到 activeNode
    若 hitTest(pos) == activeNode 或其子孫：發 Click 到 activeNode
        若 activeNode 是 checkbox 且未 disabled：切換 kStateChecked，發 Change
    清 kStateActive；activeNode = 無
Wheel:
    h = hitTest(pos)；發 Wheel；若未 preventDefault：向上找最近 overflow: scroll 的元素，
        ScrollY += wheelDelta.y * 40，ScrollX += wheelDelta.x * 40，夾在 [0, scrollHeight - clientHeight]
PointerLeave（指標離開視窗）:
    等同 PointerMove 到無命中
```

`disabled` 元素：仍可 hover，但不設 active、不發 Click、不可 focus。

## 8.5 鍵盤與焦點

- `focusNode` 單一。`setFocus(n)`：舊者發 Blur 清 `kStateFocus`；新者設位元發 Focus。
- `KeyDown/KeyUp/TextInput` 發到 `focusNode`，無焦點則發到根。
- `Tab` 且未 preventDefault：依文件順序移到下一個可 focus 且非 disabled 的元素（Shift+Tab 反向），循環。
- `input[type=text]` 的預設行為（未 preventDefault 時）：TextInput 插入到游標位置、Backspace/Delete 刪除、Left/Right/Home/End 移動游標。值存於 `ElementData::attributes["value"]` 對應的執行時字串（`Document` 持有一個 `std::string` 池，因為此字串會變動，是 1.7 規則的唯一例外）。改變時發 `Change`。游標繪製：Painter 在焦點 input 的游標位置畫 1px 寬 FillPath，閃爍週期 1 秒（以 clock 判定）。

## 8.6 動畫事件

`Animator::onFinished` → 若該實例由 CSS `animation` 建立，對其元素發 `AnimationEnd`；由 API 建立則呼叫 API 註冊的回呼（13）。`AnimationIteration` 於 `iter` 增加時發。`onMarker` → 發 `Marker` 事件到 `anim` 元素。Transition 完成 → `TransitionEnd`。這些事件冒泡。

## 8.7 游標

`Context::cursor()` 回傳 hoverNode 的 `ComputedStyle::cursor`，宿主自行設定系統游標。

## 8.8 IME 組字與文字編輯

`input[type=text]` 的值為 UTF-8 字串，游標與選取以**位元組偏移**表示，只能落在字素邊界（`ShapeResult::graphemeStart`）。

宿主責任：
- 焦點進入可編輯元素時 `Context::wantsTextInput()` 變 true，宿主啟動平台文字輸入（SDL：`SDL_StartTextInput`）；離開時停止。
- 每幀（或焦點/游標變動時）呼叫 `Context::textInputRect()` 取得游標矩形（視口座標），交給平台定位候選視窗（SDL：`SDL_SetTextInputRect`）。
- 把平台的組字事件轉為 `CompositionStart / CompositionUpdate(composition, cursor, selLen) / CompositionEnd`；確定文字走 `TextInput`。

核心行為：
- `CompositionStart`：記錄組字起點 = 目前游標（若有選取，先刪除選取）。
- `CompositionUpdate`：元素的**顯示字串** = value 前段 + composition + value 後段；重新佈局；Painter 在 composition 範圍的每個 GlyphRun bounds 下畫 1px 底線（顏色同 `color`），並在 `compositionCursor` 處畫游標。`value` 本身**不變**，不發 `Change`。
- `CompositionEnd`：清除組字狀態，顯示字串回到 value；平台隨後送的 `TextInput` 才真正插入並發 `Change`。
- 組字中收到 `KeyDown`（Backspace 等）：不做預設編輯行為（由 IME 處理），事件仍冒泡。
- 三個 Composition 事件都冒泡，可 `preventDefault` 以阻止預設顯示（宿主自繪）。

游標移動（無組字時）：`Left/Right` 依**視覺方向**移動一個字素：在 LTR run 內 Right = 下一字素，在 RTL run 內 Right = 上一字素；跨 run 邊界時依 UAX#9 重排後的視覺順序決定下一個 run。`Home/End` 到邏輯字串首尾。多行 input 與直排 input 第一版不支援（`writing-mode` 對 input 忽略並警告）。

滑鼠點擊定位：07.7 的命中測試回傳邏輯位元組偏移。

## 8.9 編輯器藍圖新增（2026-09-27 定案；對應 27 章）

- **命中結果新增 `WatchOnly`**：`pointer-watch: true` 的元素參與 hover/move 判定但不吃 down/up/click；事件穿透到下一個命中（下層 UI 或宿主）。
- **全域彈出層**：`layer: overlay` 的子樹在命中順序最前、繪製於根之後；不受祖先 `overflow: hidden` 裁切。
  - 2026-10-20 補記（Aux Duty XF-019）：`layer` 是 `.xts` 樣式屬性（05 §5.9.2），寫 `#Confirm { layer: overlay; }`，不是元素屬性 `<div layer="overlay">`。
  - 2026-10-20 補記（Aux Duty XF-029）：`<use>` 的實例根是有尺寸的盒子，會吃點擊；內部 `display: none` 不會讓實例根失去命中，且 `pointer-events` 會繼承——全畫面容器寫 `pointer-events: none`、各面板根寫 `pointer-events: auto`。overlay 容器內未宣告 `layer` 的子元素畫在巢狀 overlay 之下，要蓋在上面的那個也宣告 `layer: overlay`。
- **surface 疊層**：宿主的多個 surface 依 `z` 排序，`input: block | pass | watch`；`block` 時下層不收指標；modal = block + `kFocusTrap`。
- **多玩家焦點**：`focusNode[player]`（最多 8）；輸入事件帶 `player` 與裝置類型；觸發 `player(n).*` 只對該玩家生效。
- **手把映射**：A/Cross → `press/click`，B/Circle → `back` 事件，方向鍵與左搖桿 → 焦點導覽，肩鍵 → `tab-prev/next`；`focus-nav: auto` 以幾何評分（方向錐 + 距離 + 對齊）選下一個可焦點元素，`explicit` 用 `nav:*`；焦點環由 Focus 狀態樣式決定。
- **觸控**：`gesture(tap|swipe|pinch|hold)` 事件；虛擬搖桿符號（30.4）把觸控轉成向量 port。
- **拖放**：`drag.start/over/drop` 帶來源 id、目標 id、`item` 鍵；拖曳中的視覺以 `Dragging` 狀態與 `drag-ghost` 元素表達；跨 surface 拖放由宿主轉送。
- **生命週期**：surface `show/hide` 請求觸發根元素事件；`then hide` 讓 surface 進 `hiding` 直到時間軸播完再回報 `OnHidden`。
- **spawn 實例**：不參與命中（`pointer-events: none`），壽命到期回收。
- **輸出事件**：`sound(cue, params)`、`haptic(cue)`、`emit(port)` 經 13.8 送宿主。

- T-172：`EventType` 尾端新增 `TimelineEnd`、`TimelineMarker`；`Event` 帶時間軸名稱（與標記名）。

### 8.9.1 T-194 實作記錄（#192）

- **焦點評分**（`focus_nav.cpp`）：以邊框盒的螢幕矩形；`major` = 方向上的中心偏移、`gap` = 遠邊到近邊距離（重疊 0）、`orthoGap` = 垂直軸間隙（跨度重疊 0）、`orthoCenter` = 垂直中心偏移；候選須 `major > 0.5` 且（`orthoGap == 0` 或 `orthoCenter ≤ 2·major`，約 63° 錐）；`score = gap + 4·orthoGap + 0.5·orthoCenter` 最低者勝、同分依文件序。`nav:<dir>` 目標可焦點時一律優先；`explicit` 無 `nav:` → blocked；`none` 只排除自身與子樹為目標（離開仍走幾何）；搜尋不出最近的 `kFocusTrap` 祖先；無焦點時先 `autofocus`、否則第一個可焦點元素。可焦點 = button／input／checkbox／`tabindex`／有 `on:focus`／`on:press`／`draggable`／`drop-target`／`autofocus`；點擊取焦點；Enter 啟動非按鈕的可焦點元素。
- **玩家**：0-based（`player(n)`、`:focus-player(n)`、`--event "press A 1"`）；`setPlayerDevice(0, kind)` 兼 `setInputDevice`；shell 不自動切裝置。
- **surface**：預設 Block（維持舊「最上層吃全部」）；Pass 的命中忽略該 surface 的根與 body；Watch 只看指標移動；鍵盤與手把送到最後按下的 surface（其上無 Block 者），否則最上層 Block；隱藏 surface 不收也不參與路由；焦點鎖只由 `Document::setFocusTrap` 設（無標記語法）。
- **拖放**：每文件一次；按下移動 4 px 成拖曳且不產生 click；`drag.over` 進入目標時觸發一次；ghost 恆繪（60%；手把拖曳放在焦點元素 +(8,8)）；手把長按 0.5 s 開始、B 取消（不送 back）；`Dragging`／`DropHover` 只在文件宣告該狀態名時切換；跨 surface 只經 `onDragCross` 通知宿主。
- **手勢**：`xt/gesture.h` 的 `GestureConfig`／`GestureRecognizer`（core，shell 餵 SDL finger 事件）；雙擊也回報第一次 tap；pinch 過閾值後每次移動回報。
- **overlay**：painter 主遍歷跳過並排隊 `kLayerOverlay`（3D 根外），主迴圈後 `paintOverlays()` 依繪製序、在所有祖先裁切與圖層之外、用父的 world opacity 繪出（巢狀 overlay 續排）；drag ghost 於 `paintGhost()`（PushLayer 0.6 → 平移後的來源子樹 → PopLayer）；`hitTestDetailed` 先反序測 overlay 再走一般樹（整樹遍歷）；3D 空間根內不支援 overlay 與 ghost；GpuFrame 與 CommandList 不變。
- `InputEvent` 尾端：GamepadButtonDown/Up、GamepadAxisMotion、Gesture 種類與 `gamepadButton/gamepadAxis/axisValue/player/gesture/direction/gestureScale` 欄位；`GamepadButton`（SDL 編號）、`GamepadAxis`、`NavDirection`、`GestureKind`、`kMaxPlayers = 8`；`Event` 尾端 `player, direction, gesture, gestureScale, dragSource, dropTarget, sourceItem, targetItem`；`HostEvent` 尾端 `strength, text, target, targetId, targetItemIndex, player`；`EventType` 尾端 Back、NavBlocked、DragStart/Over/Leave、Drop、Gesture；`HostEventKind` Announce 3、Drop 4。`aria-label` 留在屬性池（不可在地化）。未處理：符號／repeat 模板內的 `nav:*` 與 `focus()` id 未改寫路徑。

### 8.9.x 實作記錄（T-201 #199）

- `focus-trap` 屬性（裸／`true`／`false`）設 `kFocusTrap`，等同 T-194 的 API。
- `draggable="axis-x"` 元素可聚焦但不是拖放來源；聚焦時左右鍵、D-pad、搖桿發 `nav.value(±1)` 且**不移動焦點**。

## 8.x 派送中的重入（2026-10-02 審查修正；T-260a #273）

- **傳播路徑在任何 listener 執行之前就固定**。路徑上的節點被 listener 銷毀（或槽被重用）時跳過該節點，事件繼續傳到其餘祖先。預設動作的目標被 listener 銷毀時，預設動作不做事。
- listener 清單以索引走訪；callback 可以新增／移除 listener（既有規則 8.3）。
- listener 在 callback 內卸載自己的文件：該文件之後的 listener 不再被呼叫（13 §13.9）。

## 8.10 鍵盤預設啟動的開關與 DoubleClick（2026-10-20 定案；Aux Duty XF-030／XF-012；T-379）

- **預設行為（寫明）**：焦點在 `<button>` 時 `Enter`／`Space` 觸發 Click；`<checkbox>` 為 `Space`；其他可焦點元素為 `Enter`（8.9.1）；`Tab`／方向鍵移焦點。監聽器 `preventDefault` 可逐次取消。
- **文件層開關**：`<body key-activation="auto | none">`（預設 `auto`）。`none` = 不做 Enter／Space 啟動，焦點導覽（Tab／方向鍵／`nav:*`）**照常**。桌面程式把 Space 留給自己時用。對應 `Context::setKeyboardActivation(bool)`（執行期覆寫，`true` 回到文件宣告）與 `Document::keyboardActivation()`。編譯：`<body>` 屬性進 IR 文件旗標（不升版，用既有旗標位）。
- **`DoubleClick` 事件**：`EventType::DoubleClick`，在第二次 Click 後派發（同一目標元素、兩次 PointerDown 間隔 ≤ 400 ms、位移 ≤ 4 px 邏輯像素），冒泡；第一次 Click 照常派發（不延遲）。`InputEvent` 尾端加 `uint8_t clicks`（宿主有就填，SDL 的 `clicks`；0 = 核心自己算）。`on:dblclick` 觸發名；`<a>`／`input` 無預設行為。
- **實作記錄（T-379）**：開關存成 `<body>` 的 `key-activation` 屬性（IR 無既有文件層旗標位，沿用 DOCT `attrPool`，不升版）；編譯器對 `auto`／`none` 以外的值報錯。`Document::keyboardActivation()` 回傳文件宣告；`Context::setKeyboardActivation(bool)`／`keyboardActivation()` 為執行期覆寫，生效值 = 兩者皆真（`Document::dispatchInput` 每次交給 dispatcher）。DoubleClick 只計指標 Click：`InputEvent::clicks`（PointerUp 優先、否則 PointerDown 的值）非 0 時以 `>= 2` 判定，否則自算（上次指標 Click 同元素同世代、同按鍵、兩次 PointerDown 間隔 ≤ 0.4 s 且位移 ≤ 4 px）；成對後重置，第三擊重新起算。`TriggerEvent::DoubleClick = 31`（`on:dblclick`）；SDL 外殼填 `button.clicks`。

- **（2026-10-20 補，Aux Duty XF-044；T-383）** `Event` 尾端加 `uint8_t clicks`：Click 事件帶這組連點的序號（1 = 第一下、2 = 第二下），`DoubleClick` 帶 2。宿主要「單擊延後、雙擊取消單擊」時，在 Click 看 `clicks==1` 起計時、`DoubleClick` 取消即可，不必自記狀態。實作記錄（T-383）：點擊序號在 Click 派發前判定（`EventDispatcher::classifyClick`，判定規則與時序同 T-379），宿主給 `InputEvent::clicks` 時原值照填（Click 與 DoubleClick 皆同）；鍵盤／手把啟動的 Click 為 1；其他事件為 0。

## 8.11 指標取消、滾輪單位、元素相對座標（2026-10-20 定案；Aux Duty XF-052／053／056；T-386）

- **`PointerCancel`**（`EventType` 尾端加）：activeNode 存在時若視窗失去焦點、指標離開視窗且 OS 收回捕捉、或宿主呼叫 `Context::cancelPointer()`，派 `PointerCancel` 到 activeNode（冒泡），清 `kStateActive`／activeNode，**不派** PointerUp／Click。觸發名 `on:pointercancel`。`InputEvent::PointerCancel` 供宿主送；`xt_shell` 在 `SDL_WINDOWEVENT_FOCUS_LOST` 與 `SDL_WINDOWEVENT_LEAVE`（有按鍵按住時）送出。
- **滾輪單位**：`wheelDelta` 以「格」為單位，1.0 = 一格（一行）；觸控板與高解析滾輪給小數；像素制宿主除以其行高（建議 40 邏輯像素）再送。向下為正。`InputEvent` 不加像素欄位。
- **元素相對座標**（滑桿等不經宿主）：`Event::localPos` 是 `current`（監聽者）的 border-box 座標，觸發器寫在實例根時即根座標。新增 `Document::localPosIn(NodeId element, Vec2 rootPos) → Vec2` 與 `Document::fractionIn(NodeId element, Vec2 rootPos) → Vec2`（border-box 內比例，夾在 [0,1]；元素未佈局回 (0,0)）。觸發器動作加 `set(<port>, pointer.fraction(#Track).x)`／`.y` 與 `pointer.local(#Track).x/.y`（只在 Pointer* 事件的觸發器內有效；`#id` 為同作用域元素），符號內的滑桿可寫 `on:pointermove="if active set(value, pointer.fraction(#Track).x)"`。
- 實作記錄（T-386，2026-10-20）：`EventType::PointerCancel`、`InputEvent::PointerCancel`、`Context::cancelPointer()`（佇列一個 PointerCancel；`Context` 把它送到每個 surface 並解除指標捕捉，只有持有 activeNode 的文件有反應）；`EventDispatcher::handlePointerCancel` 先清 active 鏈與 activeNode（含進行中的指標拖放與 `axis-x` 拖曳）再派事件，`Event::pos` 為最後的指標位置。`xt_shell`：`SDL_WINDOWEVENT_FOCUS_LOST` 在 `translateSdlEvent` 轉成 PointerCancel；`SDL_WINDOWEVENT_LEAVE` 時 `SDL_GetMouseState` 有按鍵則先 `cancelPointer()` 再送 PointerLeave；多視窗依 windowID 送到該視窗。觸發器：`TriggerEvent::PointerCancel = 32`、`PointerMove = 33`（觸發名 `pointercancel`、`pointermove`；`pointerdown`／`pointerup` 為 `press`／`release` 的別名，同一 canonical）；`if active` = 觸發器元素帶 `kStateActive`（IR：If 動作 `flags = kIfActive`、空條件範圍）；`set()` 值 `pointer.fraction(#id).x|.y`／`pointer.local(#id).x|.y`（可接 `| 轉換器`）為 `SetSource::PointerFractionX/Y`、`PointerLocalX/Y`（4～7，arg2 = 作用域解析後的 id 字串），只在 hover／leave／press／release／click／dblclick／pointermove／pointercancel 觸發器內合法，否則編譯錯；執行期用觸發事件的 `pos`（Job／Waiter 帶著，`then` 等待後仍是同一點）。IR 不升版。偏離／未涵蓋：沒有 `on:wheel` 觸發器，故「Wheel 觸發器」不在合法清單；編輯器觸發器面板的事件清單未加 `pointermove`／`pointercancel`。
- **（2026-10-20 補定，T-386 實作時發現文件未定義）** 觸發名 `on:pointermove`（`PointerMove`，冒泡）正式加入；`on:pointerdown`／`on:pointerup` 為 `press`／`release` 的別名。觸發器 `if active`（無比較式）= 該元素帶 `kStateActive`，任何觸發器可用。`hit-slop` 的 `em`／`%` 以元素自身解析（05 §5.9.11 原寫「邏輯像素」，改為允許三種單位）。

## 8.12 指標修飾鍵、捲動事件、外部按下、拖曳重排（2026-10-20 定案；Aux Duty XF-057～061；T-387／T-388）

- **修飾鍵（XF-057 bug）**：`Event::modifiers` 對 Pointer*／Click／DoubleClick／Wheel／Drag* 一律填 `InputEvent::modifiers`（原本只有鍵盤事件填）。宿主送指標事件時要帶修飾鍵；`xt_shell` 以 `SDL_GetModState` 填。
- **捲動（XF-059）**：`EventType::Scroll`（目標 = 捲動容器，冒泡；`Event::scrollPos` = 新的 (scrollX, scrollY)），由使用者滾輪／拖曳捲軸／慣性／API 捲動都派發；觸發名 `on:scroll`。API 見 13 §13.9。
- **外部按下（XF-061，light-dismiss）**：`EventType::OutsidePress`：PointerDown 的命中目標不在元素子樹內時，對每個宣告了 `on:outsidepress` 的**可見**元素派發（不冒泡；target = 該元素；`Event::pos` = 按下位置）。配合 `hide()` 即 popover 的 light-dismiss；`layer: overlay` 子樹同樣適用。
- **拖曳重排（XF-058）**：
  - `Event::dropFraction`（Vec2，DragOver／Drop：指標在 dropTarget border-box 內的比例，夾 [0,1]）——宿主以 `y < 0.5` 判「插在前」；
  - **取消**：拖曳中按 `Escape`、按下第二個指標鍵（右鍵）、`PointerCancel`、或 `Context::cancelDrag()` → 派 `DragCancel`（dragSource，冒泡）後結束，**不派** Drop；`DragEnd` 照常派（帶 `Event::dropped=false`）。原「手把 B 取消」照舊走這條。
  - **門檻**：`draggable` 元素可設屬性 `drag-threshold="<px>"`（預設 4，邏輯像素）。
  - **Click 抑制**：成拖曳後的 PointerUp 不派 Click（規格原文，實作確認：放開在來源元素上也不派）。
  - **靠邊自動捲動**：拖曳中指標進入最近 `overflow: scroll` 祖先（或 dropTarget 自己）邊緣 24 px 帶時，每幀以距離線性加速捲動（最大 600 px/s），`drag-autoscroll="false"` 可關。
  - 多列 payload 不做：宿主在 `drag.start` 時自己記選取集合。
  - 實作記錄（T-388，2026-10-20）：`Event::dropFraction`（`Document::fractionIn`；DragOver 依協調裁定改為 web `dragover` 語意：指標拖曳在 drop target 上每次移動都派，`dropFraction`／`targetItem` 每次更新，`DropHover` 只在進入時設、DragLeave 不變；手把拖曳仍只在焦點換目標時派；Drop 於放開時；`HostEvent::Drop` 尾端加 `dropFraction`）、`Event::dropped`；`EventType::DragCancel`、`EventType::DragEnd`（原 T-194 無 DragEnd，本任務新增：每次拖曳結束都派到 dragSource、冒泡，取消時緊接在 DragCancel 之後）；`TriggerEvent::DragCancel = 34`（觸發名 `drag.cancel`；`DragEnd` 無觸發名）；`Context::cancelDrag()`（立即對每個 surface 呼叫 `Document::cancelDrag()`，不排隊）。取消順序：清 `Dragging`／`DropHover` → 目標 DragLeave（沿用 T-194）→ DragCancel → DragEnd(dropped=false)。Escape 在拖曳中被吃掉（不派 KeyDown）；指標拖曳中的第二個 PointerDown 被吃掉（不派 PointerDown，第一個按壓仍為 active，其放開不派 Click）；手把 B 與 PointerCancel 走同一 `cancelDrag`。Click 抑制：實作前即不派（`handlePointerUp` 對成拖曳的按壓跳過 activate），T-388 另以 `m_pressDragged` 涵蓋「取消後才放開」的情況。`drag-threshold`／`drag-autoscroll` 為一般屬性（attrPool），編譯期驗證（非負數／true|false，否則錯誤），dispatcher 於執行期讀取。自動捲動在 `EventDispatcher::tick`（`Document::update` 開頭）：自 dropTarget（無則指標下元素）向上找第一個捲動軸有效的容器（`scroll`，或 `auto` 且溢出，與滾輪同一判定），容器帶 `drag-autoscroll="false"` 或指標不在其 border-box 內即不捲；速度 `(24 − 距離)/24 × 600 px/s × dt`，夾 [0,max]，經 `setProperty(ScrollX/ScrollY)`（T-387 的 `setScroll` 併入後改接）。偏離／未涵蓋：HostEvent 沒有 DragOver，故 DragOver 的 dropFraction 只經 Event 監聽器取得。
- **T-387 實作記錄（修飾鍵、捲動、外部按下）**：
  - 修飾鍵：`EventDispatcher` 處理 Pointer*／Wheel／PointerLeave／PointerCancel／Gesture 輸入期間記住 `InputEvent::modifiers`，`dispatch`／`dispatchAt` 對 `modifiers == 0` 的事件補上（同 `player` 的作法），所以 Enter／Leave／Move／Down／Up／Click／DoubleClick／Wheel／Scroll／OutsidePress／Drag* 都帶；鍵盤事件照舊自填。`xt_shell` 的滑鼠移動／按鍵／滾輪與手勢事件以 `SDL_GetModState()` 填。
  - 捲動：唯一的寫入點 `EventDispatcher::setScroll(arena, node, x, y, notify)`，兩軸各夾在 [0, max]（max 只在 `overflow: scroll`／`auto` 軸上 = layout box 的 `maxScrollX/Y`，其他軸 0）；位置真的變了且 `notify` 才派 Scroll（冒泡，`Event::scrollPos`）。目前實際存在的捲動路徑是滾輪、13 §13.9 的 API 與 T-388 的拖曳靠邊自動捲動（`EventDispatcher::autoScroll` 改走 `setScroll(..., notify=true)`，每幀有移動就派一次 Scroll）——核心沒有捲軸拖曳、慣性或手把捲動。宿主直接 `setProperty(ScrollY)` 不派 Scroll。
  - 外部按下：登錄表 = `addListener(OutsidePress)` 與含 `on:outsidepress` 的觸發表（載入與 repeat 實例 `bindNode`）登錄的元素（帶 generation，死節點下次登錄時清掉）。PointerDown 派發後、預設動作前（不受 `preventDefault` 影響）逐一檢查：在文件樹內（不是脫離的模板／池中實例）、自己與祖先都不是 `display: none`／`visibility: hidden`、命中目標不在其子樹（命中為空 = 在外）→ `dispatchAt`（不冒泡；`pos`、`localPos`、`button`、`modifiers`）。PointerDown 監聽器若把命中元素刪掉，該次不派。
  - 觸發名 `on:scroll`（`TriggerEvent::Scroll` = 35）、`on:outsidepress`（`OutsidePress` = 36；34 是 T-388 的 `DragCancel`）。

## 8.13 觸發器的按鍵過濾與條件、`Document::cancelPointer`（2026-10-21 定案；Aux Duty XF-067／068；T-390）

- **按鍵過濾**：指標觸發名可帶按鍵：`on:press(left|middle|right)`、`on:release(...)`、`on:click(...)`、`on:dblclick(...)`、`on:pointermove(...)`（move 以目前按住的鍵判）；不帶 = 任何鍵（現況）。`if button(left|middle|right)` 條件 = 本事件的 `Event::button`（非指標事件 → 編譯錯）。
- **`if` 條件擴充**：`if state(<名>)`（元素帶該自訂或內建狀態，含 `Disabled`／`Hover`／`Active`／`Focus`／`Checked`）、`if port(<名>) <op> <值>`（`eq/ne/gt/lt/ge/le`，port 為同作用域 in-port，值為常數；`on:port` 既有的 `if eq(1)` 不變）、`if not ...`；可用 `and` 串接（不做 `or`，拆成兩條觸發）。
- `Document::cancelPointer()`（公開，Document 層，立即執行）；`Context::cancelPointer()` 排入輸入佇列、派送時對每份文件呼叫（與其後排入的 PointerLeave 保持順序；T-390 實作裁定）。
- **寫明**：`PointerCancel` 之後的 PointerUp 不派送（按壓已結束）；`hit-slop` 只看矩形，`border-radius` 不影響（`clip-path` 才隨形）；`bind:disabled` 的子樹效果見 27 §27.3.2。
- **T-390 實作記錄**：`IrTrigger` 原 `_pad[3]` 的第一個位元組改為 `button`（`PointerButton` + 1，0 = 任意鍵；IR 不升版，驗證器拒絕 > 3）；觸發名 `press|release|click|dblclick|pointermove(<left|middle|right>)`（`pointerdown`／`pointerup` 同），canonical 帶 `(<鍵>)`，可與不帶鍵的同事件觸發器並存。`TriggerRuntime::fire` 比對 `onDomEvent` 記下的鍵；Click 現帶放開的鍵（原本 Click 的 `Event::button` 一律是 Left），PointerMove 在按壓中帶按住的鍵（`EventDispatcher::m_pressButton`），無按壓時 `pointermove(<鍵>)` 不觸發。條件：If 動作新旗標 `kIfTerms = 2`，arg0／arg1 為項列表：每項一個標頭 step（op = `Operands`；args[0] = `IfTerm`：Compare 0／Active 1／Button 2／State 3／Port 4；args[1] = 1 表示 `not`；args[2] = 運算元；State 的 args[3] = 1 表示內建位元；Port 的 port 索引以 `ConverterArg::Port` 標記，逐元素解析）後接 `argCount` 個比較 step，全部成立走 true 分支。只寫單一比較或 `if active`（無 `not`／`and`）仍用 T-175／T-386 的舊編碼，金樣不變。`if state(X)`：`Disabled／Hover／Active／Focus／Checked` 讀 ElementState，其餘為自訂狀態（未宣告則同 `state(X, on)` 登記一個槽）；`if port(<名>)`：觸發元素作用域可見的值 port（call port → 錯誤），repeat 實例內映射到實例副本，比較值同 T-175（數字、`N%` 相對該 port 的 `_max`、port、eq／ne 的字詞）。`if button()` 在非指標觸發器 → 錯誤 `if button() is only valid in pointer triggers (...)`；`Document::fireTrigger` 沒有按鍵（帶鍵過濾的觸發器不觸發，`if button()` 為假）。`Document::cancelPointer()` 走與排入的 PointerCancel 相同的輸入路徑、立即生效；`Context::cancelPointer()` 仍排入佇列（保持與先前排入輸入的先後），派送時對每份文件呼叫 `Document::cancelPointer()`。

## 8.14 外部檔案拖入（2026-10-21 定案；Aux Duty XF-079；T-393）

- **核心**：`InputEvent::ExternalDragEnter / ExternalDragMove / ExternalDragLeave / ExternalDrop`（`pos` 視窗邏輯座標；`ExternalDrop` 帶 `paths`：`std::span<const std::string_view>`，宿主送入時有效，核心複製）。核心把它當成一場 `dragSource == kInvalidNode` 的拖曳：對 `drop-target` 元素派 `DragOver`（每次移動）／`DragLeave`／`Drop`，`Event::dropFraction`、`targetItem` 照 T-388；`Event::externalPaths`（Drop 時非空；`std::span<const std::string>`）；`DropHover` 狀態照舊。`HostEvent::Drop` 尾端加 `paths`。觸發器 `on:drag.over/.drop` 可用，`if external` 條件（本事件來自外部拖入）。離開視窗或取消 → `DragLeave` 清狀態（`Event::leftWindow` 為真；換目標的 DragLeave 為假），無 `DragCancel`；指標下沒有任何 `drop-target` 時外部拖曳不派事件、OS 顯示禁止游標，要「一進視窗就提示」在 `body` 設 `drop-target`（T-399）。
- **shell**：`WindowConfig::acceptFileDrop = true`。Windows：`platform_win.cpp` 註冊 OLE `IDropTarget`（`RegisterDragDrop`，`CF_HDROP`；`DragEnter/DragOver/DragLeave/Drop` 轉成上面四種事件，座標由螢幕轉視窗邏輯座標；回 `DROPEFFECT_COPY` 若指標下有 `drop-target` 元素，否則 `DROPEFFECT_NONE`）；需 `OleInitialize` 於建視窗前（記錄 shell 已呼叫，宿主不得重複）。其他平台：退回 SDL `SDL_DROPBEGIN/DROPFILE/DROPCOMPLETE`——只有 `ExternalDrop`（座標取 `SDL_GetMouseState`），無懸停高亮。多視窗：逐視窗註冊。
- `xtframe-view` 範例：`Examples/demos/borderless` 加一個 `drop-target` 區塊，`on:drag.drop` 顯示檔名數（`count` 來源若無，宿主印 log 即可）。
- 實作記錄（T-393，2026-10-21）：`InputEvent::ExternalDragEnter/Move/Leave/ExternalDrop`（`Kind` 尾端）與尾端 `paths`；`Context::pushInput` 對 ExternalDrop 把字串與 view 陣列複製進 `m_pathPool`（deque，與 composition 池同時清空）；`Context` 以 `pressTarget(pos)` 決定外部拖曳的 surface，換 surface 時先對舊的送 ExternalDragLeave。dispatcher（`handleExternalDrag`）：`DragState::external`、`source = kInvalidNode`；Enter 與 Move 同樣處理（未開始就開始）；指標路徑（PointerMove／第二鍵取消／PointerUp／PointerCancel）略過外部拖曳；內部拖曳進行中或按在 draggable 上（pending）時外部事件忽略；Drop 位置與上次 Move 的目標不同時先 `updateDragOver`；沒有任何 Move 的 Drop（SDL 退路）只派 Drop（無 DragOver、無 DropHover）；放在目標外不派任何事件；`ExternalDragMove` 與 PointerMove 一樣不先 flush 連續觸發器；靠邊自動捲動對外部拖曳也有效。`Event::externalPaths`、`HostEvent::paths`（皆 `std::span<const std::string>`，回呼期間有效；外部 Drop 的 `HostEvent::element` 為 kInvalidNode）。`if external`：`IfTerm::External = 5`（`kIfTermLast = 5`），只有項列表編碼；執行期為 DragOver／DragLeave／Drop 且 `dragSource == kInvalidNode`（以 `TriggerRuntime` 按鍵位元組的 0x80 位元帶入 Job，`then` 後仍有效），非拖曳觸發器恆為假、不報錯。`Document::hasDropTargetAt(pos)` = `dropTargetAt(pos).valid()`。shell：`WindowConfig::acceptFileDrop`；Windows `platform_win.cpp`：`App` 建構時 `SDL_Init` 後 `OleInitialize`（失敗只 log），並關掉 SDL 的 DROPFILE／DROPTEXT／DROPBEGIN／DROPCOMPLETE；`~App` 在視窗全部銷毀後 `OleUninitialize`；每個 Window 建立時 `RegisterDragDrop`（`FileDropTarget`，COM 引用計數，`CF_HDROP` 以 `DragQueryFileW` 轉 UTF-8），銷毀前 `RevokeDragDrop`；座標 `ScreenToClient` 後乘「SDL 視窗大小／client 像素」再經 fit 映射；回 `DROPEFFECT_COPY` 依該視窗**目前文件**（`setCurrentDocument`）的 `hasDropTargetAt`；非檔案拖曳回 NONE 且不送事件。其他平台：`App::handleDropEvent` 把 SDL_DROPFILE 累積到該視窗、DROPCOMPLETE 送一個 ExternalDrop（`SDL_free` 檔名；座標：指標在該視窗上用 `SDL_GetMouseState`，否則全域座標減視窗位置），`acceptFileDrop = false` 的視窗丟棄。`xtframe-view` 的 ContextListener 對帶 paths 的 Drop 印出目標與每個路徑；borderless 範例加 `#DropZone`（`DropHover` 高亮、`if external state(Dropped)`）。Windows 實測（2026-10-21，Win11、125% 縮放，以 WinForms `DoDragDrop`＋`CF_HDROP` 合成兩個檔案的拖曳到 `xtframe-view --borderless`）：區塊外 effect `None`、區塊內 `Copy`，懸停期間 `#DropZone` 帶 `DropHover`，放下後印出「drop of 2 file(s) on #DropZone」與兩個路徑，邏輯座標與指標位置一致。偏離／未涵蓋：視窗截圖工具在此機器擷取 GL 視窗為黑畫面，懸停高亮以狀態（`DropHover`）確認，未以截圖確認；未以實際 Explorer 拖曳測試；非 Windows 退路只在 Windows 上編譯（`platform_other.cpp` 未編譯驗證）。

## 8.15 拖曳重排第二輪、OutsidePress 的 consume、`on:pointerdrag`、虛擬化捲動（2026-10-21 定案；Aux Duty XF-081～084；T-394）

- **拖曳（XF-081）**：(a) `Event::dropIndex`（DragOver／Drop，目標為 `<repeat>` 或其實例時 = 插入索引：`targetItem + (dropFraction.y ≥ 0.5)`；指標在**來源列**上時以來源列幾何算，`targetItem` = 來源索引；非清單目標 = 0xFFFFFFFF）。(b) 放在目標外只有 `DragEnd{dropped=false}`，列表頭尾由宿主判（寫明）。(c) 門檻前 `Escape` 取消按壓（之後放開不派 Click）；取消後在放開前按下的其他鍵一律吞掉。(d) 只有主鍵（左鍵／第一指）能開始拖曳；`draggable="any-button"` 放寬。(e) 自動捲動每幀重算並重派 `DragOver`（幾何變了）。(f) `drag-ghost="none"` 不畫 ghost；(g) `<use>` 上 `draggable`／`drop-target`／`drag-threshold`／`drag-autoscroll`／`drag-ghost` 為裸屬性（不當 port）。
- **OutsidePress（XF-082）**：觸發器內 `consume()` 動作（或監聽器 `event.consume = true`）→ 該次 PointerDown 不再派給命中元素（其 PointerUp／Click 也抑制）；modal 式 light-dismiss 不必留 backdrop。同一次 PointerDown 才變可見的元素不收 OutsidePress（寫明）。
- **`on:pointerdrag`（XF-083）**：在元素上按下主鍵、移動 ≥ `drag-threshold`（預設 4）後，每幀一次（合併）帶最新座標；不需要 `draggable`、不畫 ghost、不派 Drag*；`pointer.fraction(#id)` 可用；放開 → `on:release`；`PointerCancel` 結束。`if dragging` 條件 = 本元素處於 pointerdrag 中。**in-port 變更通知**：`HostPorts::addInputListener(port, fn)`（文件內 `set()`／綁定寫入 in-port 時回呼；宿主自己 `set` 不回呼）。
- **`scrollIntoView(std::string_view repeatId, uint32_t index, ScrollAlign)`（XF-084）**：`ElementRef`（捲動容器）新 overload；虛擬化時用項高推算，先完成待處理的排版再算（剛插入的列同幀可用）。
- 實作記錄（T-394，2026-10-21）：
  - 拖曳：`Event::dropIndex`（尾端）由 `TriggerSink::dropIndexOf` 算：目標在 `<repeat>` 實例內 = `targetItem + (dropFraction.y ≥ 0.5)`；目標為 `<repeat>` 元素本身時以 `RepeatController::indexAt` 找指標下的項，再用該項幾何（實例盒，或虛擬化的 `itemHeight` × index）算上下半；其餘 0xFFFFFFFF。來源列：指標在來源子樹上、來源本身是 `drop-target` 且在 repeat 實例內時，**來源列即為目標**（`dragOver` = 來源、`DropHover` 照設、放下也派 Drop 給它，`targetItem` = 來源索引；`EventDispatcher::pointerDropTarget`）。門檻前 Escape：`m_drag.pending` 且有 active 時取消按壓（Escape 被吃掉、放開不派 Click）。取消後吞鍵：拖曳取消（Escape／第二鍵／`cancelDrag()`）或門檻前 Escape 之後，在全部指標鍵放開前，其他鍵的 PointerDown／PointerUp 不派送（原按壓鍵的 PointerUp 照派、無 Click）；只吞**指標鍵**，鍵盤不受影響；PointerCancel 清除此狀態。主鍵：`InputEvent::button == Left` 才記 pending；`draggable="any-button"`（compile 接受 `true|false|any-button|axis-x`）放寬，執行期讀屬性。自動捲動：上一幀有捲動時，本幀 `tick` 先以上一幀排版後的幾何重新 hit test 並 `updateDragOver`（外部拖曳亦同），再捲本幀——捲動期間每幀一次 DragOver。`drag-ghost="none"`（compile 只接受 `none`）：`Document` 交給 Painter 的 ghost 來源設為無（實作在 Document 而非 Painter 內判斷，效果相同）。`<use>` 的 strict 標準屬性加 `drag-threshold`／`drag-autoscroll`／`drag-ghost`（`draggable`／`drop-target` 原本就在）。
  - OutsidePress `consume`：`Event::consume`（尾端）；觸發動作 `consume()` = `TriggerAction::Consume = 17`（只在 `on:outsidepress`，否則編譯錯；執行時不做事）。OutsidePress 改為**在 PointerDown 之前**派送（T-387 原為之後）；派完若任一監聽器設 `consume` 或該元素的 `on:outsidepress` 觸發器含 `consume()`（`TriggerSink::consumesOutsidePress`，於派送當下查表——觸發器本身延後執行，**不評估 `if` 條件**）→ 不派 PointerDown、不設 active／焦點／預設動作，記 `m_consumedPress` 吞掉同鍵的 PointerUp（因此也無 Click）。因順序前移，「同一次 PointerDown 才變可見的元素不收」自然成立；T-387 的「PointerDown 監聽器刪掉命中元素則不派 OutsidePress」不再適用（OutsidePress 監聽器刪掉命中元素時，PointerDown 改派給空目標）。
  - `on:pointerdrag`：`EventType::PointerDrag`（尾端，不冒泡，target = 該元素，`localPos` 為其 border-box 座標）、`TriggerEvent::PointerDrag = 39`（觸發名 `pointerdrag`，列入指標觸發器：`pointer.fraction()`／`if button()` 可用）。主鍵按下時取命中元素向上第一個有 `on:pointerdrag` 觸發器或 PointerDrag 監聽器者為候選（本次按壓已成 draggable pending 則不取）；位移 ≥ 該元素 `drag-threshold`（預設 4）後每次 PointerMove 派一次 PointerDrag，觸發器走 T-389 連續事件合併（每幀一次、最後一個事件）。PointerUp、PointerCancel、新的按壓結束。`if dragging`：`IfTerm::Dragging = 6`（`kIfTermLast = 6`），只有項列表編碼，= `EventDispatcher::pointerDragNode()` 為觸發器元素。
  - in-port 通知：`PortEvent::fromDocument`（尾端）；`BindingRuntime` 以呼叫路徑區分——`setAction`（觸發器 `set()`）與 `writePort`（綁定寫 port）期間為真，宿主 API `set`／`setText` 期間為假（`setAction` 寫 in-port 原本經由 `set()`，改為直接 assign + notify，以免被當成宿主寫入）。`HostPorts::addInputListener(PortId, std::function<void(const PortValue&)>)`／`removeInputListener(id)`：只收 in-port 的 `Changed && fromDocument`；值 Bool→Bool、Number／Entity→Number、其他→String（`portText`）；未 bind 或非 in-port 回 0；rebind 後依 PortId 保留。list 操作（宿主 API）不回呼。
  - `scrollIntoView(repeatId, index, align)`：新增公開 `Document::layoutIfDirty()`（樣式 resolve、排版、after-layout 綁定與 repeat 視窗、必要時再排一次、世界變換；不清 dirty、不推進時鐘；08 §8.17 共用）。唯讀查詢 `RepeatController::itemSpan`：非虛擬化 = 該項實例根，轉呼既有 `scrollIntoView(child)`；虛擬化 = `repeat 內容框頂 + index × itemHeight`，只調整垂直位置（水平捲動保持不變）。未知 repeat、不在此容器內、index 越界、虛擬化項高尚未量測 → `kInvalidArgument`。

## 8.16 `<input placeholder>` 的繪製（2026-10-21 定案；Aux Duty XF-076；T-395）

值為空時繪製 `placeholder` 文字：顏色 = `--placeholder-color` 變數，無則 `color` 的 50% 不透明度；字級、行高、對齊同輸入文字；不佔游標位置（游標停在最前）；有任何輸入即隱藏；聚焦仍顯示；不影響內在寬度；無動畫。`@` 開頭的在地化鍵照 05 §5.9.4 解析；**鍵未解析（無字串表或缺鍵）時不畫 placeholder**（2026-10-21 補定，T-397；不畫原始鍵）。headless、視窗、編輯器一致。

實作記錄（T-395，2026-10-21）：`LayoutEngine::layoutTextNode` 在可編輯 input 值為空且未組字時，以 `placeholder` 屬性（在地化後的字串）另行塑形排版到 `TextNodeData::placeholderRuns`（獨立塑形快取；不進 `runs`／`lines`、不影響盒與內在寬度；未定可用寬度的量測一律從行首排）；Painter 在值的字形之後、游標之前以 `DrawGlyphs` 畫出（無文字效果與裝飾）；`--placeholder-color` 經 `StyleEngine::tokenColor` 依覆寫／主題／IR `VARS` 解析（token 為文件層級，非逐元素）；`setLanguage` 與 `setAttribute("placeholder")` 會標 kDirtyLayout 重排。T-397 補：`Document` 在載入與每次 `setLocalization` 後，把仍顯示鍵本身的 `placeholder` `@key` 槽位的字串 id 列成排序清單交給 `LayoutEngine::setUnresolvedPlaceholders`，排版遇到清單內的值就不產生 `placeholderRuns`（以字串 id 判定，所以 `<repeat>` 實例的複本也適用）；`sym_TextField_normal/focus` 金圖因此回到 T-395 之前的樣子，不需重產。

## 8.17 焦點冒泡事件、文字選取、隱藏文件、拖曳目標的 repeat 鏈（2026-10-21 定案；Aux Duty XF-086～088／090；T-397）

- **`FocusIn`／`FocusOut`**（`EventType` 尾端）：與 `Focus`／`Blur` 同時派，但**冒泡**（web `focusin/focusout`）；觸發名 `on:focusin`／`on:focusout`。`Focus`／`Blur` 不變。
- **文字選取 API**：`ElementRef::selectAll()`、`setSelection(uint32_t start, uint32_t end)`（UTF-8 位元組位移，夾在長度內）、`selection() → {start, end}`；只對可編輯 `<input>`。`setValue`／`bind:value` 在 `display: none` 時也可寫（進 EditState）；`focus()` 對未佈局元素回 `kUnsupported`（寫明：先顯示再聚焦，或用 `on:show="focus()"`）。
- **隱藏文件（XF-086）**：surface 隱藏的文件每幀仍跑觸發器、時間軸、flow、綁定（狀態要延續），但**跳過排版與繪製**（顯示時補一次完整排版）。`requestShow`／`requestHide` 可在事件派送中呼叫，於該次派送結束後套用（寫明）。視窗模式：`Window::setCurrentDocument` 只決定 hit-test 與 IME 的參考文件；`Context::render` 本來就畫所有可見文件，宿主不必切換。
- **拖曳目標的 repeat 鏈（XF-088）**：`Document::repeatChain(NodeId) → std::vector<RepeatRef{std::string_view repeatId; uint32_t index;}>`（由近到遠），`Event::targetItem` 仍是最近一層；自動捲動只作用最近的捲動祖先（寫明）。
- **（XF-090）** `scrollTo`／`scrollIntoView` 後 `boundingBox` 要到下一次 `update` 才反映；需要立即值用 `Document::layoutIfDirty()`（公開，T-394 若已加則共用）。
- 實作記錄（T-397，2026-10-21）：
  - `EventType::FocusIn`／`FocusOut`（尾端）、`TriggerEvent::FocusIn = 40`／`FocusOut = 41`（觸發名 `focusin`／`focusout`）；`EventDispatcher::setFocus` 依序派 Blur → FocusOut（冒泡）→ Focus → FocusIn（冒泡），Blur／Focus 監聽器刪掉該元素時不派冒泡版；觸發器在路徑上每個有 `on:focusin` 的祖先都會執行；不會讓元素變成可 focus。
  - `ElementRef::selectAll()`／`setSelection(start, end)` 回 `Status`（非可編輯 input `kUnsupported`、無效 `kInvalidArgument`）；`setSelection` 夾在長度內並退到 UTF-8 字元起點，`start > end` 收成 `end` 處的游標，游標在 `end`；標 kDirtyPaint。`selection() → TextSelection{start, end}`。`focus()` 改回 `Status`：自身或祖先解析後 `display: none` → `kUnsupported` 且不動焦點。`setValue` 原本就不依賴排版（加測試）。
  - 隱藏文件：`Document::update` 在 `!m_surfaceVisible`（hiding 期間仍算可見）時跳過排版區塊（排版、after-layout 綁定與 repeat 視窗、外部內容佔位、粒子、世界變換、骨架），樣式解析與其他照跑；記 `m_layoutSkipped`，粒子與骨架時鐘重置（顯示後不補跑）。顯示後第一次 `update`／`layoutIfDirty`／`paint` 先全樹標 kDirtyLayout 並強制變換；若顯示發生在 update 與 paint 之間，`paint` 先做樣式＋排版＋變換（不跑綁定、不回呼宿主）再畫。
  - 派送中的 `requestShow`／`requestHide`：宿主呼叫時若 `Context` 正在派送（`m_dispatchDepth > 0`，含整個 `Context::update`），記在文件上（後者覆蓋前者），於最外層派送結束時由 `Context::runPendingDocOps` 先於卸載／重載套用；觸發器動作（`show()`／`hide()`、成員規則）走內部 `applyShow`／`applyHide`，維持立即。無 `Context` 的獨立文件沒有派送深度可查，照舊立即套用。
  - `Document::repeatChain(NodeId) → std::vector<RepeatRef>`（由近到遠；`RepeatRef{repeatId, index}`，id 指向文件字串）；唯讀查詢 `RepeatController::chainOf`；任一層為 item-exit 中的實例 → 空；spawn 池略過。`Document::layoutIfDirty()` 沿用 T-394。

## 8.18 第十一批修正（2026-10-21 定案；Aux Duty XF-098～101；T-399）

- **焦點驗證時機（XF-101 bug）**：`validateFocus` 改在 style／layout pass **之後**執行；同幀因 port 變更而顯示的 `<input>` 呼叫 `focus()` 不再被清掉。`focus()` 對「本幀尚未排版但將顯示」的元素改為接受（延到該幀驗證），只有驗證後仍 `display: none` 才清焦點並回報。
- **`bind:value` 的重套（XF-100）**：`bind:value` 在元素**由隱藏變顯示**時、以及在 `restoreState` 後重新求值（覆蓋使用者殘留的編輯）；觸發動作 `reset(value)`（本元素，重套 `bind:value`，可寫在 `on:state-exit(Editing)`）。不做雙向綁定（in-port 由宿主所有）。
- **外部拖曳（XF-099）**：`Event::leftWindow`（DragLeave 時真 = 指標離開視窗／拖曳結束，假 = 換目標）；寫明：沒有任何 `drop-target` 時外部拖曳不派事件、OS 顯示禁止游標，要「一進視窗就提示」在 `body` 設 `drop-target`。
- **符號內 list in-port 的 `<repeat>` 用於外層範本（XF-098 bug）**：正式支援——符號宣告 `port:rows="list<…> in"`、內部 `<repeat port="rows">`，該符號在外層 `<repeat>` 範本內 `bind:rows="item(rows)"` 時，每個外層實例的內層 repeat 讀自己那份實例 port（T-380 的每實例 port ＋ T-391 的內層控制器）；路徑 `Rows/2/Cell/Sub/0/X`。做不到的情況必須是編譯錯誤，不得靜默部分展開。
- 實作記錄（T-399，2026-10-21）：
  - 焦點：`Document::update` 的 `validateFocus` 移到排版區塊內 after-layout 重排之後（隱藏 surface 則在樣式之後），先套 pending focus 再驗證；有焦點變化時再做一次樣式＋排版，讓 `:focus` 與 Focus／Blur 觸發器的變更同幀生效。`ElementRef::focus()` 對 `display: none` 的元素：若有尚未求值的 port 變更（`BindingRuntime::hasDirty`）或鏈上 `display: none` 的元素帶 kDirtyStyle → 回 kOk 並記 `EventDispatcher::setPendingFocus`（含 generation）；update 驗證時可 focus 才 `setFocus`，否則不動焦點、`Document::warnings()` 報一次「still display:none after layout」；否則照 T-397 回 kUnsupported。任何 `setFocus` 清掉該玩家的 pending。既有測試「setStyle(display:none) 要兩次 update 才 Blur」改為一次。
  - `bind:value`：`BindingRuntime::reapplyShownValues()` 在第二次樣式解析之後逐一檢查 Value 綁定的元素（自身或祖先 `display: none`），由隱藏變顯示時強制重套（即使 port 值未變）；`restoreState` 後 `reapplyAllValues()`；`reset(value)` = `TriggerAction::Reset = 18`（無參數；`reset()` 只接受 `value`，元素無 `bind:value` 屬性 → 編譯錯「reset(value) needs bind:value on the element」），執行期 `reapplyValue(node)`（含 repeat 實例的綁定）。IR 版本不變（舊執行期略過未知動作）。
  - `Event::leftWindow`（尾端）：`finishDrag` 的 DragLeave（外部 `ExternalDragLeave`、拖曳取消）為真，`updateDragOver` 換目標或移出目標的 DragLeave 為假。偏離：`Context` 的外部拖曳換 surface 時對舊 surface 送的 ExternalDragLeave 也是真（視窗內換文件）。
  - XF-098 根因：`RepeatController::createInnerRepeats` 對非 item-field 的內層 repeat 一律用 IR 的 `listPort`，即模板元素上宣告的 port（符號 `port:rows`）的**模板本身**那份；外層每實例的 `bind:rows` 寫的是 T-380 的實例副本，內層 copy 永遠聽不到（也無實例）。修法：該 port 若在本實例的 `inst.ports` 中就用實例副本，否則 `instancePortOf`（外層祖先實例的副本），document port 照舊共享。實例副本的 list 寫入走 `listSet` → `onListChanged(副本, Set)`，因此外層 `listUpdate`／`listInsert`／`listRemove` 與宿主寫 `Rows/1/Cell.rows` 都原地重綁、索引位移照 T-391。實測：上述重現、符號放在二層巢狀 repeat 內、符號內 repeat 再含 `item(k)` 巢狀、虛擬化外層回收列，皆正確；未找到需要編譯期拒絕的組合，故無新增編譯錯誤。
