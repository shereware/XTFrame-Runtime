# 53 · Flow 語言規格（`.xtflow`）

狀態：**整理（T-559，2026-10-31）**。本章不新增、不改變任何語意：它把散在舊 17 章 §17.6（git 歷史）、27 §27.4.1～§27.4.5、13 §13.8.12、22 章、43 章與 50 章的流圖語言收成一章，並逐條對照 `Code/Runtime/flow`（節點表、`parse()`／`validateGraph()`、`flow_ports.cpp`）與 `Tests/flow`、`Examples/demos/*/*.xtflow`。文件與實作不一致之處不在本文擇一，列在 §53.99 待主模型裁決；本文正文在那些點上**描述目前程式的行為並標註「見 §53.99 第 n 項」**。

來源（以下簡稱）：

| 簡稱 | 位置 |
|---|---|
| 舊 17.6 | `git show 56789f16^:docs/design/17-editor.md` 的 §17.6「Demo 模式與流圖（`xt_flow`）」（56789f16 重寫 17 章時移除；27 §27.4.1 宣告「全部維持」） |
| 27.4.x | `Docs/design/27-interaction.md` §27.4、§27.4.1～§27.4.5、§27.12 |
| 13.8.12 | `Docs/design/13-api.md` §13.8.12、§13.8.13、§13.10 |
| 程式 | `Code/Runtime/flow/`：`include/xt/flow/{flow_graph,flow_runtime,flow_ports,bundle_context,host_ports}.h`、`src/flow_graph.cpp`（`parse()`）、`src/flow_bindings.cpp`（`validateGraph()`、埠表）、`src/flow_nodes.cpp`（節點行為）、`src/flow_signals.cpp`（傳播、連續標記）、`src/flow_runtime.cpp`（`tick()`）、`src/flow_slots.h`（槽位表） |
| 測試 | `Tests/flow/*.cpp`（`xt_flow_tests`、`xt_flow_alloc_tests`），引用時寫 doctest 案例名 |

本章是給**外部實作者**（另一種執行器、編譯器、生成器）的唯一依據；讀者不需要編輯器原始碼。

## 53.1 定位與範圍

### 53.1.1 flow 是什麼

flow 是 UI 的**資料邏輯圖**：條件、計時、計數、狀態機式流程、跨 UI 協調（27 §27.4 角色表）。節點之間以邊傳遞訊號；圖的邊界是**連接點**（元素內建埠、`<anim>` 埠、文件埠、宣告 port，§53.5），與遊戲的介面永遠是連接點。

| 範圍 | 檔案 | 端點寫法 |
|---|---|---|
| UI 流圖 | 每個 UI 一份 `<ui>.xtflow`（bundle 內 `ui/<id>.xtflow`，22 章） | `#<id>.<port>`、`#document.<port>`；不得用 `ui#` |
| 全域流圖 | 每個 bundle 一份 `bundle.xtflow`（UI Project 根或 `shared/`；pack 後在 bundle 根，manifest `flows.global`） | `ui#<uiId>.<element>.<port>`、`ui#<uiId>.<port>`、`ui#<uiId>.shown|hidden`；不得用 `#id.port` |

flow 在遊戲端由宿主執行（宿主插件內建 `xt_flow`；宿主可關閉，只用自己的邏輯，27 §27.4、23 §23.10 第 10 條）。

### 53.1.2 flow 不是什麼（27 §27.4.3）

**flow 只能「觸發」與「選擇」，不能「插值」。** 純視覺反應不經 flow：補間屬於元素的 `transition`、`@timeline`／`animation`、綁定的 `tween(d)`／`delay(t)`。一條邊「來源是連續訊號、終點是視覺屬性」就是用 flow 做動畫，執行器報診斷 `flow.animatesWithContinuousSignal`（§53.6.3）。flow 作為遊戲邏輯模擬器的角色保留：模擬值應經 `set(port)`（寫入宣告 port 的邊）交給 UI，再由 `bind:` 讀它（§53.5.6）。

### 53.1.3 執行模型

- **tick**：宿主每幀在 `Context::update()`（輸入派送、綁定求值）之後呼叫一次 `tick(dt)`（13 §13.8.12；`BundleContext::tick` 註解「Call after Context::update()」）。
- **訊號種類**：`pulse`（一次觸發，不帶值）與值（`bool`／`number`／`string`）。值型輸出**只在值改變時**往下推（程式 `pushRawOutput()` 比對前值）；例外見各節點（`filter`／`throttle` 通過的值一律送出、`system` 的字串事件每次都發、`state.state` 狀態變更時不比較直接寫）。
- **值型訊號按幀合併（27 §27.4.1，T-389）**：文件層值型連接點（`#document.*`、元素 `hover`、`<anim>.localTime`、非 event 型宣告 port）是連續訊號——一個 tick 內只送最新值；被送到的重算型節點（`math`、`compare`、`toString`、`select`、`lookup`、`format`）只標 dirty，最後依載入時算好的拓樸序各求值一次。**脈衝**（Click、Finished、Marker、event port……）逐事件同步派送，但送出前先結算已暫存的值，維持「值先於脈衝」。
  - 合併只在綁定的文件 `behavior ≥ 1`（`kBehaviorFlowSignalCoalescing`，43 §43.3）時生效；`behavior 0` 維持舊行為：每次推送立即求值（測試 `T-407: behavior 0 -- the pointer ports and the format node follow each event at once`）。
- **tick 內順序**（程式 `FlowRuntime::Impl::tick()`）：(1) 清空每邊觸發計數（回饋保護窗口）；(2) 時鐘累加 `dt`；(3) 送出上一幀暫存的值型訊號；(4) 依序推進 `clock`／`timer` 節點；(5) 輪詢 `#document.*` 與 `<anim>.localTime`；(6) 結算批次；(7) 送出到期的 `throttle` trailing 值並再結算；(8) 同步 `values()`。
- **無環**：節點對節點的邊不得成環（載入時 Kahn 演算法），例外是同一節點的 pulse→pulse 自環（§53.6.1）。經過文件繞回的迴路無法靜態判定，執行期以回饋保護截斷（§53.6.4）。
- **初始推送（seeding）**：載入成功時每個節點先推一次初始輸出（`constant` 的值、`counter.initial`、`toggle.initial`、`radio.initial`、`state.initial`、重算型節點以未連線輸入求值一次……）。初始推送**不算**邊的投遞（`edgeLastFired()` 全為 −1），`compare` 不在此期間發 `true`／`false`，`state.set` 與 `timeline.seek` 在此期間忽略。

## 53.2 `.xtflow` JSON 格式

### 53.2.1 頂層

| 鍵 | 型別 | 必要 | 說明 |
|---|---|---|---|
| `version` | 正整數 | 否 | 缺 = 1。非正整數 → 錯誤；大於執行器支援的最新版（現為 `kFlowFormatVersion = 1`）→ 拒載並報「需要較新的 XTFrame」，其餘一概不檢查（43 §43.2，T-406）。寫出一律 `1`。 |
| `nodes` | 陣列 | 是 | §53.2.2 |
| `edges` | 陣列 | 是 | §53.2.3 |
| `groups` | 陣列 | 否 | 編輯器畫布註解框（T-193），執行期忽略；每項 `{title, rect: [x, y, w, h], color?}`，壞項**丟棄、不報錯** |

- 物件／陣列巢狀深度上限 32 層，超過 → `.xtflow: nesting too deep`（T-262c）。
- 其他頂層鍵：讀取時忽略；`toJson()` **不保留**（寫回只有 `version`、`nodes`、`edges`、非空時 `groups`，固定順序，兩格縮排）。

### 53.2.2 `nodes[]`

| 鍵 | 型別 | 必要 | 說明 |
|---|---|---|---|
| `id` | 字串 | 是 | 圖內唯一（重複 → `duplicate node id '<id>'`）。執行器不檢查字元，但端點以**第一個 `.`** 切開 `id` 與埠，且以 `#`、`ui#` 開頭的字串被當成連接點，所以含 `.` 或以 `#` 開頭的 id 無法被邊引用（§53.2.6 假設 3）。 |
| `type` | 字串 | 是 | §53.4 的 23 種之一；未知 → `unknown node type '<type>' for node '<id>'`，**整張拒載**（43 §43.7 凍結清單：節點型別只加；27 §27.4.4：舊執行期遇未知型別整張拒載）。 |
| `params` | 物件 | 否 | 純量（`bool`／`number`／`string`）落入 `params`；陣列落入 `arrayParams`，須為**非空、元素同型的純量陣列**（`lookup.values`）。`state` 節點的 `states`／`events` 為字串陣列、`transitions` 為物件陣列（§53.4.15）。`null`、物件（`transitions` 以外）→ 錯誤。未列在節點表的鍵：接受、執行期不用、寫回保留。 |
| `pos` | `[x, y]` | 否 | 編輯器畫布位置（T-193），執行期忽略；形狀不對即丟棄、不報錯。 |

### 53.2.3 `edges[]`

| 鍵 | 型別 | 必要 | 說明 |
|---|---|---|---|
| `from` | 字串 | 是 | 來源端點（輸出埠） |
| `to` | 字串 | 是 | 終點端點（輸入埠） |
| `params` | 物件 | 否 | 值一律字串（非字串 → `edge '<from>' -> '<to>' param '<k>' must be a string`）。認得的鍵見下表；其他鍵忽略、寫回保留。 |

| 邊參數 | 用在 | 意義 |
|---|---|---|
| `class` | 終點 `#id.addClass`／`removeClass`／`toggleClass` | 必要，class 名 |
| `prop` | 終點 `#id.setProperty` | 必要；PropId 名稱（`propertyByName()`），或效果參數位址 `effect.<效果名|堆疊索引>.<參數>`（T-547） |
| `state` | 終點 `#id.setState` | 必要；自訂狀態名或內建別名（同 27 §27.2.3 `state()` 動作） |
| `mode` | 終點 `#id.setState` | `on`（缺省）／`off`／`toggle` |
| `instance` | 任何文件端點 | 非負整數字串（≤ 0xFFFFFFFF）。來源只放行該實例的事件、終點只送該實例（多實例 UI，27 §27.3.1，T-193） |

### 53.2.4 端點語法

| 寫法 | 意義 | 在哪種圖 |
|---|---|---|
| `<nodeId>.<port>` | 節點自己的埠 | 兩種 |
| `#<elementId>.<port>` | 元素內建埠、`<anim>` 埠，或該元素宣告／暴露的 port（§53.5） | UI 流圖 |
| `#document.<port>` | 文件埠（`clock`、`viewport*`、`pointer*`），或文件層（無 id 元素）宣告的 port | UI 流圖 |
| `ui#<uiId>.<element>.<port>` | 另一個 UI 的 `#<element>.<port>` | 全域流圖 |
| `ui#<uiId>.<port>` | 該 UI 的 `#document.<port>`（含文件層宣告 port） | 全域流圖 |
| `ui#<uiId>.shown`／`ui#<uiId>.hidden` | UI 生命週期（pulse，只能當來源） | 全域流圖 |

切法：`ui#` 先去掉前綴，以第一個 `.` 切出 `uiId`，剩下若無 `.` 即兩段寫法；`#` 寫法以 `#` 之後第一個 `.` 切出 id 與埠；節點寫法以第一個 `.` 切開。

### 53.2.5 最小完整範例

即 `Examples/desktop_app/ui/app.xtflow`（舊 17.6 的範例圖；測試 `FlowGraph::parse: the Docs/design/17-editor.md §17.6 sample graph`、`FlowGraph::toJson: round-trips Examples/desktop_app/ui/app.xtflow`）：

```json
{
  "version": 1,
  "nodes": [
    { "id": "n1", "type": "counter", "params": { "step": 1 } },
    { "id": "n2", "type": "toString", "params": { "format": "{}" } }
  ],
  "edges": [
    { "from": "#inc.Click", "to": "n1.increment" },
    { "from": "n1.value", "to": "n2.in" },
    { "from": "n2.out", "to": "#count.setText" },
    { "from": "#darkToggle.Change", "to": "#root.toggleClass", "params": { "class": "dark" } }
  ]
}
```

按 `#inc` → 計數器 +1 → 轉字串 → 寫進 `#count` 的文字；`#darkToggle` 的 `Change`（string）接到 pulse 輸入 `toggleClass`：任何值到達即觸發、內容丟棄（§53.3.2）。

全域流圖的實例：`Examples/demos/npdo/ui-project/shared/bundle.xtflow`（`ui#main-menu.new_game → screen.set2`、`radio → lookup → ui#hud.active`）。

### 53.2.6 JSON Schema（由 `parse()`／`validateGraph()` 反推）

**以下 schema 是從 `flow_graph.cpp` 的 `FlowGraph::parse()` 與 `flow_bindings.cpp` 的 `validateGraph()`／埠表反推的，不是程式使用的檔案**；通過 schema 是載入成功的必要條件而非充分條件。反推時的假設與 schema 表達不了的規則：

1. JSON Schema 表達不了的檢查：節點 id 唯一、陣列元素同型、邊兩端的埠存在與型別相容、值型輸入扇入、無環、`select` 必須接 `a` 與 `b`、`state` 的 `states` 唯一與轉移一致、`filter` 依 `op` 的邊限制、需要文件才能答的檢查（id 存在、`<anim>` 限定埠、宣告 port 方向與型別、效果位址）。這些見 §53.6.2。
2. `additionalProperties` 一律允許（程式忽略未知鍵；頂層未知鍵寫回時不保留，節點／邊參數的未知鍵保留）。
3. 端點的 `pattern` 只描述切法（第一個 `.`），不排除程式會在埠名檢查時拒絕的字串；`id` 本身不設 pattern（程式不檢查），但含 `.` 或以 `#` 開頭者無法被引用。
4. `version` 的 `maximum` 寫目前的 `kFlowFormatVersion`（1）；超過時程式回 `kUnsupported`（不同於一般錯誤的 `kInvalidArgument`）。
5. `pos`、`groups` 壞項程式丟棄不報錯，schema 只描述「會被採用」的形狀。
6. 每種節點的必要參數與值域依 `parse()` 的 `switch`：`constant.value`、`timer.interval`、`math.op`、`compare.op`、`radio.count`（整數 2..64）、`lookup.values`、`filter.op`（+ `deadband` 要 `threshold` ≥ 0 有限、`quantize` 要 `step` > 0 有限）、`throttle.unit`（`rate` 若有須為 number、`trailing` 若有須為 bool）、`sound`／`haptic.cue`、`spawn.symbol`、`emit.port`、`timeline.name`（非空字串）、`state.states`（由 `checkStateNode()` 檢查）。其他參數不做型別檢查——型別不符時執行期以預設值取代（`paramNum()`／`paramBool()`）。

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://xtframe.dev/schema/xtflow-1.json",
  "title": ".xtflow (format version 1) -- reverse-engineered from FlowGraph::parse()/validateGraph(), not normative over the code",
  "type": "object",
  "required": ["nodes", "edges"],
  "properties": {
    "version": { "type": "integer", "minimum": 1, "maximum": 1 },
    "nodes": { "type": "array", "items": { "$ref": "#/$defs/node" } },
    "edges": { "type": "array", "items": { "$ref": "#/$defs/edge" } },
    "groups": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["rect"],
        "properties": {
          "title": { "type": "string" },
          "rect": { "type": "array", "items": { "type": "number" }, "minItems": 4, "maxItems": 4 },
          "color": { "type": "string" }
        }
      }
    }
  },
  "$defs": {
    "scalar": { "type": ["boolean", "number", "string"] },
    "scalarArray": { "type": "array", "minItems": 1, "items": { "$ref": "#/$defs/scalar" } },
    "ident": { "type": "string", "pattern": "^[A-Za-z_][A-Za-z0-9_]*$" },
    "nonEmptyString": { "type": "string", "minLength": 1 },
    "endpoint": {
      "type": "string",
      "anyOf": [
        { "pattern": "^ui#[^.]+\\.[^.]+(\\.[^.]+)?$" },
        { "pattern": "^#[^.]+\\..+$" },
        { "pattern": "^[^#.][^.]*\\..+$" }
      ]
    },
    "edge": {
      "type": "object",
      "required": ["from", "to"],
      "properties": {
        "from": { "$ref": "#/$defs/endpoint" },
        "to": { "$ref": "#/$defs/endpoint" },
        "params": {
          "type": "object",
          "additionalProperties": { "type": "string" },
          "properties": {
            "instance": { "type": "string", "pattern": "^[0-9]{1,10}$" },
            "mode": { "enum": ["on", "off", "toggle"] }
          }
        }
      }
    },
    "node": {
      "type": "object",
      "required": ["id", "type"],
      "properties": {
        "id": { "type": "string" },
        "type": {
          "enum": ["constant", "counter", "toggle", "timer", "clock", "random", "math", "compare",
                   "toString", "select", "radio", "lookup", "merge", "format", "system", "sound",
                   "haptic", "spawn", "emit", "timeline", "state", "filter", "throttle"]
        },
        "params": {
          "type": "object",
          "properties": { "transitions": { "type": "array" } },
          "additionalProperties": { "anyOf": [ { "$ref": "#/$defs/scalar" }, { "$ref": "#/$defs/scalarArray" } ] }
        },
        "pos": { "type": "array", "items": { "type": "number" }, "minItems": 2, "maxItems": 2 }
      },
      "allOf": [
        { "if": { "not": { "properties": { "type": { "const": "state" } } } },
          "then": { "properties": { "params": { "properties": { "transitions": { "$ref": "#/$defs/scalarArray" } } } } } },
        { "if": { "properties": { "type": { "const": "constant" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["value"],
            "properties": { "value": { "$ref": "#/$defs/scalar" } } } } } },
        { "if": { "properties": { "type": { "const": "timer" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["interval"] } } } },
        { "if": { "properties": { "type": { "const": "math" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["op"], "properties": {
            "op": { "enum": ["add", "sub", "mul", "div", "min", "max", "clamp", "map", "abs", "round"] } } } } } },
        { "if": { "properties": { "type": { "const": "compare" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["op"], "properties": {
            "op": { "enum": ["eq", "ne", "lt", "le", "gt", "ge"] } } } } } },
        { "if": { "properties": { "type": { "const": "radio" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["count"], "properties": {
            "count": { "type": "integer", "minimum": 2, "maximum": 64 } } } } } },
        { "if": { "properties": { "type": { "const": "lookup" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["values"], "properties": {
            "values": { "$ref": "#/$defs/scalarArray" } } } } } },
        { "if": { "properties": { "type": { "enum": ["sound", "haptic"] } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["cue"], "properties": {
            "cue": { "$ref": "#/$defs/nonEmptyString" } } } } } },
        { "if": { "properties": { "type": { "const": "spawn" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["symbol"], "properties": {
            "symbol": { "$ref": "#/$defs/nonEmptyString" } } } } } },
        { "if": { "properties": { "type": { "const": "emit" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["port"], "properties": {
            "port": { "$ref": "#/$defs/nonEmptyString" } } } } } },
        { "if": { "properties": { "type": { "const": "timeline" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["name"], "properties": {
            "name": { "$ref": "#/$defs/nonEmptyString" }, "scope": { "type": "string" }, "ui": { "type": "string" } } } } } },
        { "if": { "properties": { "type": { "const": "filter" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["op"],
            "properties": { "op": { "enum": ["deadband", "quantize", "sample"] } },
            "allOf": [
              { "if": { "properties": { "op": { "const": "deadband" } } },
                "then": { "required": ["threshold"], "properties": { "threshold": { "type": "number", "minimum": 0 } } } },
              { "if": { "properties": { "op": { "const": "quantize" } } },
                "then": { "required": ["step"], "properties": { "step": { "type": "number", "exclusiveMinimum": 0 } } } }
            ] } } } },
        { "if": { "properties": { "type": { "const": "throttle" } } },
          "then": { "required": ["params"], "properties": { "params": { "required": ["unit"], "properties": {
            "unit": { "enum": ["frames", "seconds", "hz"] }, "rate": { "type": "number" },
            "trailing": { "type": "boolean" } } } } } },
        { "if": { "properties": { "type": { "const": "state" } } },
          "then": { "required": ["params"], "properties": { "params": {
            "required": ["states"],
            "properties": {
              "states": { "type": "array", "minItems": 2, "maxItems": 64, "uniqueItems": true, "items": { "$ref": "#/$defs/ident" } },
              "initial": { "$ref": "#/$defs/ident" },
              "events": { "type": "array", "items": { "$ref": "#/$defs/ident" } },
              "transitions": { "type": "array", "items": { "type": "object", "required": ["from", "on", "to"],
                "properties": { "from": { "type": "string" }, "on": { "$ref": "#/$defs/ident" }, "to": { "type": "string" } } } }
            },
            "additionalProperties": { "$ref": "#/$defs/scalar" } } } } }
      ]
    }
  }
}
```

## 53.3 型別系統

### 53.3.1 四種埠型別

`PortType`：`pulse`、`bool`、`number`（IEEE double）、`string`。flow 沒有 struct／list：宣告 port 的 struct／list 型別會列在連接點清單，但**不可連接**（`port '#<id>.<p>' is a struct/list port (not connectable yet)`）。宣告 port 映到 flow 型別：`number`／`entity` → number、`bool` → bool、`event` → pulse、其他純量（string、enum、color、vec2、vec3、texture）→ string（其文字形）（程式 `declaredPortFlowType()`）。

### 53.3.2 邊的型別規則

依序判斷（程式 `validateGraph()`）：

1. **終點是 pulse 輸入**：任何型別的來源都可接；任何值到達即觸發，內容丟棄（舊 17.6）。
2. **終點是彈性輸入**——`toString.in`、`format.a`～`format.d`、`compare.a`／`compare.b`、`emit.value`：接受任何**值型**來源；pulse 來源 → `cannot connect a pulse output to '<to>'`。（`compare` 的運算元型別見 §53.99 第 1 項。）
3. 其他：兩端型別必須相同，否則 `type mismatch on edge '<from>' -> '<to>'`。尚未綁定文件時，宣告 port 端點的型別未知（`anyType`），此條延到 `load()` 才檢查。

### 53.3.3 型別推斷

- `select`：`a`、`b`、`out` 同一型別，取自接到 `a` 的來源（未知則取 `b`）；pulse 來源不用於推斷。迭代到固定點，故 `select` 鏈的宣告順序無關。推不出型別或少接 `a`／`b` → `node '<id>': select needs a and b`（測試 `FlowGraph::parse: select's type comes from its a/b edges`、`select: its output carries the inferred type straight into a string sink`）。
- `filter`：`out` 型別跟 `in` 的來源；`in` 不得是 pulse。
- `throttle`：`out` 型別跟 `in` 的來源，`in` 可以是 pulse（此時 `out` 為 pulse）。
- `constant.out` 型別 = `value` 參數的型別；`lookup.out` 型別 = `values` 陣列元素的型別。

## 53.4 節點目錄

共 **23 種**（`parseNodeType()`、`nodeTypeTable()`、`flow_slots.h`）。表中「預設」是程式在參數缺漏時用的值（`paramNum`／`paramBool` 的預設引數）；「必要」是 `parse()` 拒載的條件。輸出順序即 `values()`／`valueNames()` 的順序。「成本」欄：**重算**＝值輸入改變時求值（批次內每 tick 至多一次）；**事件**＝只在輸入到達時動作；**計時**＝每 tick 推進。

### 53.4.1 `constant`

| 項目 | 定義 |
|---|---|
| params | `value`（bool／number／string，**必要**） |
| inputs | — |
| outputs | `out`（型別 = `value` 的型別） |
| 語意 | 載入時推一次 `value`，之後不變 |
| 成本 | 只在載入 |

測試：`FlowGraph::parse: the Docs/design/17-editor.md §17.6 sample graph`（間接）、`compare: every op`（以 constant 餵值）。

### 53.4.2 `counter`

| 項目 | 定義 |
|---|---|
| params | `step`（預設 1）、`initial`（預設 0）、`min`、`max`（可省；省略即不夾） |
| inputs | `increment`、`decrement`、`reset`（pulse）、`set`（number，T-123） |
| outputs | `value`（number） |
| 語意 | `increment`／`decrement` 加減 `step`；`reset` 回 `initial`；`set` 直接寫值。**每一次寫入**（含 `initial`、`set`）都夾進 `[min, max]` |
| 成本 | 事件 |

測試：`counter: increment/decrement/reset, clamped to [min, max]`、`counter: the \`set\` input writes the value, clamped like every other write`。

### 53.4.3 `toggle`

| 項目 | 定義 |
|---|---|
| params | `initial`（bool，預設 false） |
| inputs | `toggle`、`setTrue`、`setFalse`（pulse） |
| outputs | `value`（bool）、`changed`（pulse） |
| 語意 | 值真的改變才推 `value` 並發 `changed` |
| 成本 | 事件 |

測試：`toggle: toggle/setTrue/setFalse; \`changed\` pulse only fires on an actual change`。

### 53.4.4 `timer`

| 項目 | 定義 |
|---|---|
| params | `interval`（秒，**必要**）、`autostart`（bool，預設 false）、`repeat`（bool，預設 true；false = 單次） |
| inputs | `start`、`stop`（pulse） |
| outputs | `tick`（pulse）、`count`（number，累計 tick 數） |
| 語意 | 執行中每 tick 累加 `dt`，每跨過一次 `interval` 就 `count += 1`、推 `count`、發 `tick`（同一個 `dt` 可跨多次）。`start` 開始並把累計時間歸零（`count` 不歸零）；`stop` 停止。`repeat: false`：發 `tick` **之前**先停，所以 `tick → start` 可重新啟動、`tick → stop` 合法（pulse 自環）。`interval` 在 (0, 0.001) 以 0.001 s 執行（寫回保留原值）；0、負數、非有限 → 永不觸發；單一 tick 最多觸發 1000 次，超過丟棄剩餘時間（T-258a）。 |
| 成本 | 計時 |

測試：`timer: ticks (and increments \`count\`) once per interval crossed within one dt`、`timer: repeat=false ticks once, and \`start\` runs it again`、`timer: tick -> its own stop is allowed (a pulse self-edge is not a cycle)`、`T-258a F1: parse keeps the authored timer interval; toJson round-trips it byte for byte`、`T-258a F1: an authored interval of 0.5 ms still fires every 1 ms`、`T-258a F1: a timer interval of 0, below 0 or non-finite never fires`、`T-258a F1: one tick fires a timer at most 1000 times, then drops the rest`。

### 53.4.5 `clock`

| 項目 | 定義 |
|---|---|
| params | — |
| inputs | — |
| outputs | `seconds`（number：自載入起所有 `tick(dt)` 的累計秒數） |
| 成本 | 計時（連續來源，§53.6.3） |

測試：`clock: \`seconds\` accumulates tick() dt`。

### 53.4.6 `random`

| 項目 | 定義 |
|---|---|
| params | `min`（預設 0）、`max`（預設 1）、`integer`（bool，預設 false） |
| inputs | `trigger`（pulse） |
| outputs | `value`（number；載入時為 `min`） |
| 語意 | 每次 `trigger` 推一個 `[min, max]` 均勻分布值（`integer` 時為整數，含兩端）。非有限邊界當 0；`min > max` 對調；整數邊界飽和（T-258a）。亂數源：每節點一個以 `std::random_device` 播種的 `std::mt19937`——**不可重現**（§53.8.2）。 |
| 成本 | 事件 |

測試：`random: trigger produces a value in [min, max] (degenerate range for determinism)`、`T-258a F16: random takes non-finite bounds as 0 and saturates integer bounds`。

### 53.4.7 `math`

| 項目 | 定義 |
|---|---|
| params | `op`（**必要**：`add`、`sub`、`mul`、`div`、`min`、`max`、`clamp`、`map`、`abs`、`round`）；`clamp` 用 `lo`（預設 0）／`hi`（預設 1）；`map` 用 `inLo`、`inHi`、`outLo`、`outHi`（預設 0、1、0、1） |
| inputs | `a`、`b`（number；未連線讀 0） |
| outputs | `out`（number） |
| 語意 | `div` 除以 0 得 0；`clamp`、`map`、`abs`、`round` 只用 `a`；`map` 在 `inHi == inLo` 時 t = 0（得 `outLo`），不夾限 |
| 成本 | 重算 |

測試：`math: every op`、`math: clamp and map`。

### 53.4.8 `compare`

| 項目 | 定義 |
|---|---|
| params | `op`（**必要**：`eq`、`ne`、`lt`、`le`、`gt`、`ge`） |
| inputs | `a`、`b`（彈性：任何值型；見 §53.99 第 1 項） |
| outputs | `out`（bool）、`true`、`false`（pulse） |
| 語意 | 兩邊都是 string → 以字串內容比較（字典序）；否則以數值比較（bool 當 0／1，未連線與 string 讀 0）。結果改變時發 `true` 或 `false`（載入初始推送期間不發） |
| 成本 | 重算 |

測試：`compare: every op`、`compare: true/false pulses only fire when the result actually changes`。

### 53.4.9 `toString`

| 項目 | 定義 |
|---|---|
| params | `format`（預設 `"{}"`；第一個 `{}` 為值的位置，沒有 `{}` 時輸出就是 `format` 字面、值不顯示）、`decimals`（number；有則固定小數位，無則最短表示、6 位有效數字） |
| inputs | `in`（彈性：任何值型） |
| outputs | `out`（string） |
| 語意 | bool → `true`／`false`；string 原樣；未到達的值 → 空字串 |
| 成本 | 重算 |

測試：`toString: decimals, shortest representation, format substitution, passthrough`。

### 53.4.10 `select`

| 項目 | 定義 |
|---|---|
| params | — |
| inputs | `cond`（bool）、`a`、`b`（同型，推斷，§53.3.3） |
| outputs | `out`（型別 = `a`／`b`） |
| 語意 | `cond` 真 → `a`，否則 `b`；未到達的輸入選到預設值 |
| 成本 | 重算 |

測試：`select: cond picks a or b`、`select: its output carries the inferred type straight into a string sink`、`FlowGraph::parse: select's type comes from its a/b edges`、`FlowRuntime: a 20-deep select chain in a wide graph ticks 1000 times quickly`。

### 53.4.11 `radio`（T-123）

| 項目 | 定義 |
|---|---|
| params | `count`（**必要**，整數 2..64）、`initial`（預設 0） |
| inputs | `set0`…`set<count-1>`（pulse，各一；`set01` 這種前導零寫法無效）、`set`（number） |
| outputs | `index`（number）、`changed`（pulse） |
| 語意 | `set<k>` 選 k；`set` 四捨五入後夾進 `0..count-1`。值改變才發 `changed`；載入時推 `initial` 不發 |
| 成本 | 事件 |

測試：`FlowGraph::parse: radio's count and its set<i> ports`、`radio: set<i>, set, clamping and the changed pulse`、`radio: the number \`set\` input is rounded and clamped into 0..count-1`。

### 53.4.12 `lookup`（T-123）

| 項目 | 定義 |
|---|---|
| params | `values`（**必要**，非空同型陣列：number／string／bool） |
| inputs | `index`（number） |
| outputs | `out`（型別 = 陣列元素型別） |
| 語意 | `index` 四捨五入後夾進 `0..len-1`，輸出該元素 |
| 成本 | 重算 |

測試：`FlowGraph::parse: lookup's values array types its out port`、`lookup: index picks a value, out of range clamps`、`FlowRuntime: the difficulty-select v2 flow loads and its lookups follow the radio`。

### 53.4.13 `merge`（T-123）

| 項目 | 定義 |
|---|---|
| params | — |
| inputs | `in`（pulse，可多條邊） |
| outputs | `out`（pulse） |
| 語意 | 任一來源到達即發一次 `out` |
| 成本 | 事件 |

測試：`merge: any incoming pulse fires one outgoing pulse`、`FlowGraph::parse: merge, format and the new element ports`。

### 53.4.14 `format`（T-123）

| 項目 | 定義 |
|---|---|
| params | `format`（預設 `"{a}"`；具名佔位 `{a}`、`{b}`、`{c}`、`{d}`，可重複、可缺），`decimals`（同 `toString`） |
| inputs | `a`、`b`、`c`、`d`（彈性：任何值型） |
| outputs | `out`（string） |
| 語意 | 同 `toString` 的值格式化；未連線或未到達的佔位 → 空字串 |
| 成本 | 重算（接到 `setStyle` 且格式可編譯時走型別化寫入，語意不變，27 §27.4.2 第 4 條） |

測試：`format: named placeholders, decimals and unconnected inputs`、`T-477: format -> setStyle hits write typed and paint as the text path`、`T-477: format -> setStyle misses go through setStyle and paint the same`。

### 53.4.15 `state`（27 §27.4.4，T-541）

| 項目 | 定義 |
|---|---|
| params | `states`（**必要**：字串陣列，2～64，識別字 `[A-Za-z_][A-Za-z0-9_]*`，不重複）；`initial`（字串，須在 `states` 內；缺省第 0 個）；`transitions`（陣列，每項 `{from, on, to}`；`from` 為狀態名或 `"*"`；同一 `(from, on)` 至多一條）；`events`（可省；缺省 = `transitions` 內 `on` 名依首次出現去重；給了就必須涵蓋每個 `on`，可多列） |
| inputs | 每個事件名一個 **pulse** 輸入 `<event>`；`set`（string：直接跳到該狀態，不經轉移表，不受 `guard` 影響）；`guard`（bool，可省；未連線視為 true；為 false 時吞掉所有事件輸入） |
| outputs | `state`（string）、`index`（number，在 `states` 中的序號）、`changed`（pulse），再依 `states` 順序每狀態 `enter<Name>`、`exit<Name>`（pulse，`Name` 原樣大小寫） |
| 語意 | 狀態 `s` 收到事件 `e`：查 `(s, e)`，無則查 `("*", e)`，再無則忽略；找到且 `to ≠ s` → 依序發 `exit<s>`、更新 `state`／`index`、發 `enter<to>`、發 `changed`（下游在 `exit` 看到舊值、在 `enter` 看到新值）；`to == s` 不發任何脈衝。載入時推 `initial` 的 `state`／`index`，**不發** `enter<initial>`。一個 tick 內多個事件依到達順序逐一處理。`set` 名稱未知 → 忽略，並每節點記一次 `runtimeErrors()`：`node '<id>': set: unknown state '<name>'`（見 §53.99 第 9 項）；`set` 在載入初始推送期間忽略（`constant → set` 不蓋 `initial`）。 |
| 規則 | 轉移寫在參數裡不是邊，不違反無環規則。事件名不得與固定埠 `set`、`guard`、`state`、`index`、`changed` 相同（見 §53.99 第 5 項）。`enter*`／`exit*` 直接接回自己的事件輸入屬 pulse 自環，**允許**；經第二個節點的迴路照報環（見 §53.99 第 4 項）。 |
| 成本 | 事件（O(1) 查表） |

載入錯誤（逐字，前綴 `state <id>: `）：`states must be 2..64 unique identifiers`、`initial "<x>" is not one of states`、`transition from "<x>" is not "*" or one of states`、`transition to "<x>" is not one of states`、`transition event "<x>" is not an identifier`、`duplicate transition (<from>, <on>)`、`event "<x>" collides with a fixed port`、`event "<x>" is not an identifier`、`duplicate event "<x>"`、`transition event "<x>" is missing from events`、`events must be an array of identifiers`、`transitions must be an array of {from, on, to} strings`。

測試：`state node: a valid minimal graph parses, toJson() reproduces it, ports follow the params`、`state node: every load-time error, verbatim`、`state: the game-flow machine walks its transitions and fires enter / exit / changed`、`state: a self-transition fires nothing, an explicit (from, on) beats the "*" fallback`、`state: guard false swallows the events (but not set); a missing edge means true`、`state: set jumps without the transition table; an unknown name is ignored and reported once`、`state: exit<s> reaches its downstream before state is rewritten, enter<to> after`、`state: events feeding back through enter / exit are the general cycle rule`、`FlowRuntime: a state node drives #Box.setState through enter<Name>`、`T-541: a state node fed an event every frame, with setState sinks, allocates nothing`；fixture `Tests/flow/fixtures/state_machine.xtflow`。

### 53.4.16 `filter`（27 §27.4.5，T-543）

| 項目 | 定義 |
|---|---|
| params | `op`（**必要**：`deadband`／`quantize`／`sample`）；`threshold`（`deadband` **必要**，有限 ≥ 0）；`step`（`quantize` **必要**，有限 > 0） |
| inputs | `in`（值型，不得是 pulse；`quantize` 只收 number）、`trigger`（pulse，只有 `sample` 可接）、`reset`（pulse：忘記上次送出值，下一個輸入必送） |
| outputs | `out`（型別跟 `in`）、`passed`（pulse，每次送出） |
| 語意 | `deadband`：number 與**上次送出值**差距 `< threshold` 不送；bool／string 值變了才送（見 §53.99 第 11 項）；第一個值必送。`quantize`：四捨五入到 `step` 倍數（`-0` 寫成 0），結果與上次送出相同不送。`sample`：`in` 只儲存，`trigger` 到達時送出目前 `in`（每次都送；尚無值則不送）。通過的值**一律送出**（不再做變更過濾）。 |
| 成本 | 事件（tick 內零成本） |

邊錯誤：`filter <id>: "in" takes a value, not a pulse`、`filter <id>: quantize takes a number`、`filter <id>: "trigger" is only used by sample`。

測試：`T-543 filter: a well-formed node of each op parses and its types follow \`in\``、`T-543 filter: every load error, verbatim`、`T-543 filter deadband 0.1: 0, 0.05, 0.2, 0.25, 0.31 send 0, 0.2, 0.31; reset sends the next input`、`T-543 filter deadband: bool and string send only when the value changed`、`T-543 filter quantize 0.5: 0.1, 0.2, 0.3, 0.76 send 0, 0.5, 1.0`、`T-543 filter sample: \`in\` stores, \`trigger\` sends the current value every time`、`T-543 filter: FlowRuntime::load() repeats the edge checks on a hand-edited graph`。

### 53.4.17 `throttle`（27 §27.4.5，T-543）

| 項目 | 定義 |
|---|---|
| params | `unit`（**必要**：`frames`／`seconds`／`hz`）；`rate`（number，可省——可由邊給）；`trailing`（bool，預設 true） |
| inputs | `in`（任何型別，含 pulse）、`rate`（number：接了邊就以最後一個值覆寫參數）、`reset`（pulse） |
| outputs | `out`（型別跟 `in`）、`passed`（pulse）、`dropped`（number，累計被吸收數；載入時 0） |
| 語意 | 窗口：`frames` = `max(1, int(rate))` 個 tick；`seconds` = `rate` 秒；`hz` = `1/rate` 秒（上限 1e9 s）。`rate` 缺、≤ 0、NaN、±inf → **不限制**（直通）。**leading**：窗口外第一個輸入立即通過並開新窗口。窗口內只留最後一個值（被覆蓋者計入 `dropped`；`trailing: false` 時每個都計入並丟棄）。**trailing**：窗口結束（tick 時檢查）若有暫存值就送出並開下一個窗口；沒有輸入就不輸出。運作中改 `rate`：下次允許時間 = 上次送出 + 新窗口（不重啟）；若已到期或變成直通且有暫存值，當下送出。`reset` 清窗口與暫存值，`dropped` 保留。時間用 flow 的 tick 時鐘（同 `timer`），比較加 1e-6 s 寬限。窗口剛結束、trailing 尚未送出前到達的新輸入：立即通過並取代暫存值（見 §53.99 第 8 項）。 |
| 成本 | 事件；只有持有 trailing 值的節點進一張小清單，每 tick 走一次 |

載入錯誤：`node '<id>' is missing required param 'unit'`、`node '<id>' has an invalid 'unit'`／`'rate'`／`'trailing'`。

測試：`T-543 throttle: pulses pass through \`in\`, types follow it, every load error verbatim`、`T-543 throttle hz 10, trailing: leading A, B and C held, C sent at the window's end, then silence`、`T-543 throttle hz 10, trailing false: only A, and the first input after the window passes at once`、`T-543 throttle frames 3 on a pulse: sent in ticks 1, 4 and 7; trailing false drops the rest`、`T-543 throttle seconds: the window is measured in seconds of tick time`、`T-543 throttle: the rate edge changes the window while it runs`、`T-543 throttle: a rate that is NaN, negative or infinite does not limit; frames 0.4 is 1`、`T-543 throttle reset: clears the window and the held value, keeps \`dropped\``、`T-543 throttle: a held value goes out once at the window's end, then nothing waits`、`T-543: 100 pointer moves in one frame reach a throttle once (T-389 coalescing comes first)`、`T-543 filter and throttle: toJson() round-trips and the node table lists them`。

### 53.4.18 系統事件節點 `system`（T-193、T-240、T-330）

| 項目 | 定義 |
|---|---|
| params | — |
| inputs | —（由宿主 `postSystemEvent(kind, value)` 驅動） |
| outputs | `loading`、`paused`、`resumed`、`disconnected`（pulse）；`languageChanged`、`platform`、`inputDevice`（string）；`back`（pulse，T-240）；`reducedMotion`（string，T-330） |
| 語意 | 每次 post 送到圖中所有 `system` 節點；字串輸出**每次 post 都發**（不比較前值）。未知 kind → `kInvalidArgument`；未載入或停用時 no-op（`kOk`）。`Context::postSystemEvent` 另接受 `language` 作 `languageChanged` 的別名（13 §13.8.13）。 |
| 成本 | 事件 |

測試：`system events: pulses and strings reach system nodes; unknown kinds are rejected`、`T-262b system events: back fires a pulse into a pulse input`、`T-330 system events: reducedMotion reaches the flow as a string`、`T-200: system events reach the Documents' system.* ports and the flows both ways`。

文件層的「系統事件」埠 `#document.clock`、`viewportWidth`／`viewportHeight`、`pointerX`／`pointerY`／`pointerNX`／`pointerNY`／`pointerDown` 不是節點，是連接點，見 §53.5.3。

### 53.4.19 動作節點 `sound`／`haptic`／`spawn`／`emit`（T-193）

| type | params | inputs | 送給宿主的 `FlowHostEvent` |
|---|---|---|---|
| `sound` | `cue`（**必要**，非空字串）、`volume`（預設 1） | `trigger`（pulse） | `{kind: Sound, name: cue, volume}` |
| `haptic` | `cue`（**必要**） | `trigger` | `{kind: Haptic, name: cue}` |
| `spawn` | `symbol`（**必要**） | `trigger` | `{kind: Spawn, name: symbol}`（生成位置由宿主決定） |
| `emit` | `port`（**必要**） | `trigger`（pulse）、`value`（彈性：任何值型，只儲存） | `{kind: Emit, name: port, value: value 最後收到的值（從未收到則 Pulse）}` |

共同：無輸出；每次 `trigger` 送一個事件給 `setHostEventListener` 註冊的接收者（無接收者、停用或替換中則不送）；事件帶 `uiId`（引發這次傳播的 UI；UI 流圖與計時器／系統事件為空）、`instanceId`、`nodeId`。成本：事件。

測試：`action nodes: sound, haptic, spawn and emit send host events with the UI and instance`、`caller audit: a global flow \`spawn\` loads and registers a UI from inside the dispatch`。

### 53.4.20 `timeline`（T-193）

| 項目 | 定義 |
|---|---|
| params | `name`（**必要**，`@timeline` 名）、`scope`（可省：元素 id，限定時間軸群組範圍）、`ui`（全域流圖**必要**，UI 流圖不得有）、`loop`（bool，預設 false）、`rate`（預設 1） |
| inputs | `play`、`stop`（pulse）、`seek`（number 秒；載入初始推送期間忽略） |
| outputs | `end`（pulse）、`marker`（string：標記名；每個標記都發） |
| 語意 | 對所屬 UI 的每個實例呼叫 `playTimeline(scope, name, {loop, rate})`／`stopTimeline`／`seekTimeline`。找不到時間軸 → 每節點一次 `runtimeErrors()`：`node '<id>': no @timeline '<name>'[ in scope '<scope>']`。`end`／`marker` 取自冒泡到根的 TimelineEnd／TimelineMarker，依 `name`（與 `scope`）篩選。 |
| 成本 | 事件 |

測試：`timeline node: play / seek / stop a named timeline, end and marker outputs`；範例 `Examples/demos/npdo/ui-project/{hud,main-menu,settings}/*.xtflow`（`timer(repeat false) → timeline.play`）。

### 53.4.21 節點表一致性

`nodeTypeTable()` 列出 23 種的固定參數名與埠（`state` 只列固定埠；陣列參數不列）。測試：`nodeTypeTable: the fourteen §17.6 node types, ports matching the shared port rules`（名稱沿舊，內容涵蓋全表）、`T-543 filter and throttle: toJson() round-trips and the node table lists them`、`xtflow: ui# references, lifecycle, instance and the new node types parse`。

## 53.5 連接點反射

連接點由文件反射產生（`connectionPoints(doc)`，`flow_ports.cpp`）：每個有 id 的元素（文件前序）列元素埠；`<anim>` 元素另列 `<anim>` 埠；最後列一次文件埠；再列宣告 port（PORT 表順序）。同一份反射產生 bundle manifest 的 `ports`（22 章：宿主綁定契約；manifest 寫 `document.<port>`，`.xtflow` 寫 `#document.<port>`，兩種都接受）。

### 53.5.1 元素埠（`#<id>.<port>`）

| 方向 | 埠 | 型別 | 行為 |
|---|---|---|---|
| 輸出 | `Click`、`PointerEnter`、`PointerLeave`、`Focus`、`Blur`、`AnimationEnd` | pulse | 對應事件逐筆 |
| 輸出 | `Change` | string | input 為其 value；`type="checkbox"` 為 `"true"`／`"false"` |
| 輸出 | `hover` | bool | 連續值，按幀合併 |
| 輸入 | `setText` | string | `ElementRef::setText` |
| 輸入 | `setStyle` | string | `ElementRef::setStyle`；**允許多條邊**，各自逐宣告合併；同一元素連續相同字串為 no-op（27 §27.4.1 (d)）；含 `animation` 的字串例外（再宣告即重播） |
| 輸入 | `setValue` | string | `ElementRef::setValue` |
| 輸入 | `addClass`、`removeClass`、`toggleClass` | pulse | 邊參數 `class`（必要） |
| 輸入 | `setState` | pulse | 邊參數 `state`（必要）與 `mode`（on／off／toggle）；執行 `Document::setCustomState`（狀態群組互斥、enter／exit 觸發器照 27.1）；未宣告的狀態名執行期靜默忽略、綁定時進 `loadWarnings()`（T-546） |
| 輸入 | `focus` | pulse | `ElementRef::focus` |
| 輸入 | `setProperty` | number | 邊參數 `prop`（必要）：PropId 名或 `effect.<效果|索引>.<參數>`（只收數值參數）；扇入以 `(元素, prop)` 計，不同 `prop` 各自一條 |
| 輸入 | `playAnimation` | string | `@keyframes` 名；等同 `setStyle("animation: <name> <duration>s <easing>")`，時長／緩動的來源見 §53.99 第 7 項；未知名稱由核心警告、不播 |
| 輸入 | `stopAnimation` | pulse | `setStyle("animation: none")` |

測試：`connectionPoints: runtime_doc.xt's ids, <anim> and document ports`、`FlowRuntime: end-to-end against runtime_doc.xt`、`FlowRuntime: setProperty fan-in is counted per property`、`FlowRuntime: playAnimation plays the element's @keyframes, stopAnimation clears it`、`FlowRuntime: playAnimation with the same name again restarts the animation every time`、`FlowRuntime: playAnimation with an unknown @keyframes name warns and plays nothing`、`FlowRuntime: setStyle takes several edges, each merged declaration by declaration`、`FlowRuntime: setState toggles a custom state, a state group stays exclusive`、`FlowRuntime: setState reaches the built-in aliases (Disabled)`、`setState edge: needs a \`state\` param, \`mode\` is on / off / toggle`、`T-547: setProperty prop effect.<name>.<param> writes the parameter and survives a restyle`、`T-547: effect.0.p0y is the same address by stack index`、`T-547: setProperty effect address errors, verbatim`、`T-547: a color effect parameter is refused (setProperty carries a number)`。

### 53.5.2 `<anim>` 埠

只對 tag 為 `anim` 的元素有效（綁定時檢查：`'#<id>' is not an <anim> element (port '<p>')`）。

| 方向 | 埠 | 型別 | 行為 |
|---|---|---|---|
| 輸出 | `Marker` | string | 標記名 |
| 輸出 | `localTime` | number | 每 tick 輪詢；連續值 |
| 輸出 | `Finished` | pulse | 播完 |
| 輸入 | `play`、`pause`、`resume` | pulse | `play` 開一個場景實例；`pause`／`resume` 作用於最近一次 `play` 的實例（尚未 play 則 no-op） |
| 輸入 | `seek` | number | 秒 |
| 輸入 | `setSpeed` | number | 倍率 |

### 53.5.3 文件埠（`#document.<port>`，只有輸出）

| 埠 | 型別 | 值 |
|---|---|---|
| `clock` | number | 該文件端點自載入起累計的 `dt` 秒數 |
| `viewportWidth`、`viewportHeight` | number | 根元素 border box 寬高 |
| `pointerX`、`pointerY` | number | 最後一次指標位置（視口邏輯 px）；第一次指標事件前為視口中心 |
| `pointerNX`、`pointerNY` | number | 相對視口中心、以半寬／半高為 1，夾在 −1..1；退化視口為 0 |
| `pointerDown` | bool | 指標按下中 |

全部是連續值（`pointerDown` 除外的七個是 §53.6.3 的連續來源）。對 `#document` 用輸入方向 → `the document has no input port '<p>'`。測試：`FlowRuntime: #document pointer ports follow the pointer`、`T-389: 100 pointer moves in one frame reach a shared format node once, with the last value`。

### 53.5.4 宣告 port（`port:`，27 §27.3）

`#<id>.<port>` 在內建埠名之外，解析為該元素宣告（或暴露）的 port；`#document.<port>` 解析為文件層（無 id 元素）宣告的 port。名稱解析順序：`<anim>` 埠 → 元素埠 → 宣告 port（`#document`：文件埠 → 宣告 port），所以與內建埠同名的宣告 port 無法從 flow 定址。

| 宣告方向 | flow 中的角色 | 寫入 |
|---|---|---|
| `out` | 來源（值型為連續值、按幀合併；event 為 pulse 逐筆） | — |
| `in` | 終點＝**`set(port)`** | number／entity／bool／event 走 `Document::setPort(Value)`，其他走文字 `setPortText()`；兩路結果相同（T-477） |
| `call` | 終點 | 值型來源先寫值，再 `callPort`；pulse 來源只 `callPort` |

方向錯 → `port '#<id>.<p>' is an output (out) port`／`is an input (in/call) port`。event 型 in port 的寫入觸發 27 章的 `port(<name>)` 觸發器。沒有文件時（`parse()`）宣告 port 的型別未知，型別檢查延到 `load()`（扇入規則的例外見 §53.99 第 2 項）。測試：`connectionPoints: declared ports are reflected off the PORT table`、`FlowRuntime: edges into and out of declared ports`、`T-389: a declared value port and an element's hover coalesce like the document ports`、`T-477: set(port) typed gives the same Document as the 17-digit text path`、`T-540: a continuous signal into a declared port is never reported`。

### 53.5.5 跨 UI 位址、生命週期與多實例

- **`ui#<uiId>.<element>.<port>`**、**`ui#<uiId>.<port>`**（= 該 UI 的 `#document.<port>`，含文件層宣告 port）只在全域流圖有效；UI 流圖用了 → `'<t>': ui# references are only valid in the global flow (bundle.xtflow)`；全域流圖用 `#id.port` → `'<t>': the global flow addresses a UI's ports as ui#<uiId>.<element>.<port>`；未註冊的 UI → `unknown UI '<ui>' in '<t>'`。一個 `ui#` 端點綁到該 UI 的**每個實例**。
- **`ui#<uiId>.shown`／`hidden`**：pulse 來源，隨 Context 生命週期（`OnShown`／`OnHidden`）觸發；當終點 → `'<t>' is a lifecycle output and cannot be an edge target`。
- **多實例**：邊參數 `instance`（§53.2.3）。flow 本身沒有 `itemIndex`；`itemIndex` 只出現在 port 事件（`PortEvent`、C ABI `xt_port_event.item_index`）。`FlowHostEvent.instanceId` 帶發出事件的實例。
- 測試：`global flow: three-segment ui# references drive one UI from another`、`global flow: two-segment references reach document-level declared ports`、`global flow: binding errors`、`global flow: ui#<id>.shown / hidden follow the Context lifecycle`、`multi-instance: instance-filtered sources and sinks`、`T-356 BundleContext: UIs in another Context take part in the global flow`。

### 53.5.6 `bind:` 與 flow 的分工（27 §27.4.3）

- 模擬或真實的遊戲值：flow（或宿主）寫進宣告 `in` port，UI 用 `bind:` 讀它——Demo 驗證過的綁定到遊戲裡一條線都不改。`set(port)` 是非視覺終點，連續性診斷永不報。
- 指標視差、時鐘驅動的裝飾、延時狀態：走觸發器 `on:pointermove` + `bind:rotate-*`／`translate-*`、`@timeline` `loop`／`animation`、`then` 鏈與 `tween`／`delay`，不經 flow。
- 同幀順序（27 §27.4.1，T-552）：`Context::update()` 先結算指標與 hover，再求值綁定；flow 的元素 `hover` 仍是按幀合併的值型訊號，在 flow `tick` 時送出。

## 53.6 規則

### 53.6.1 結構規則

1. **扇入**：值型輸入最多一條邊（`value input '<to>' has more than one incoming edge`）。例外：元素 `setStyle` 允許多條；`setProperty` 以 `prop` 區分；有 `instance` 的邊以實例區分。pulse 輸入允許多條（任一來源觸發）。
2. **無環**：節點對節點的邊做 Kahn 拓樸排序，失敗 → `cycle detected in the node graph`（只報一次）。同一節點的 pulse 輸出 → 同一節點的 pulse 輸入（如 `timer.tick → timer.stop`、`state.enterB → state.back`）不算環。文件端點不參與。
3. **邊參數**：`class`／`prop`／`state` 缺 → `edge to '<to>' is missing the '<name>' param`（`setState` 為 `#<id>.setState needs a "state" edge param`）；`prop` 不是已知屬性 → `edge to '<to>' names an unknown property '<p>'`；效果位址形狀錯 → `edge to '<to>' has a malformed effect address '<p>' (expected effect.<effect>.<param>)`；有文件時再解析效果（`#<id>.setProperty: the element has no effect`／`effect index out of range`／`unknown effect "<e>"`／`the element has no such effect`／`unknown effect parameter`／`the effect parameter is not a number`）。
4. **全域 timeline**：全域流圖的 `timeline` 節點需 `ui` 指向已註冊 UI（`node '<id>': a global timeline node needs a 'ui' param naming a UI`）；UI 流圖不得有 `ui`（`node '<id>': 'ui' is only valid in the global flow`）。

### 53.6.2 載入檢查與錯誤格式

- 兩階段：`FlowGraph::parse(json)`（無文件：JSON、形狀、節點型別與必要參數、端點語法與固定埠名、型別、扇入、環）與 `FlowRuntime::load(graph, doc)`／`loadGlobal(graph, bundle)`（重做全部結構檢查，再加 id 存在、`<anim>` 限定、宣告 port 方向與型別、效果位址、全域位址）。
- `parse()` 先檢查 `version`（不合即停），再檢查頂層形狀，再收集所有節點與邊的形狀錯誤；**有任何形狀錯誤就不做** `validateGraph()`（所以節點錯誤會蓋掉邊錯誤）。之後所有錯誤收集到 `errors`（每項一行英文），回 `kInvalidArgument`，**整張拒載**：沒有任何東西被綁定或執行，失敗的 `load()` 讓 runtime 為空（T-260c）。`version` 過新回 `kUnsupported`。
- 錯誤字串全集（逐字）：
  - JSON／版本：`JSON parse error: <detail>`、`.xtflow: nesting too deep`、`.xtflow: 'version' is not a positive integer`、`.xtflow format version <N> is newer than this XTFrame supports (<M>); a newer XTFrame is needed`、`malformed .xtflow document: expected an object with 'nodes' and 'edges' arrays`。
  - 節點：`malformed node entry (missing 'id'/'type')`、`duplicate node id '<id>'`、`unknown node type '<type>' for node '<id>'`、`node '<id>' 'params' must be an object`、`node '<id>' param '<k>' must be a non-empty array of same-typed values`、`node '<id>' param '<k>' has an unsupported JSON type`、`node '<id>' is missing required param '<p>'`、`node '<id>' has an invalid '<op|count (expected 2..64)|threshold|step|unit|rate|trailing>'`（`count` 的完整字串為 `has an invalid 'count' (expected 2..64)`）、`node '<id>' param '<p>' must be a non-empty string`、`node '<id>': select needs a and b`、`state <id>: …`（§53.4.15）。
  - 邊：`malformed edge entry (missing 'from'/'to')`、`edge '<from>' -> '<to>' 'params' must be an object`、`edge '<from>' -> '<to>' param '<k>' must be a string`、`malformed endpoint '<t>'`、`malformed UI reference '<t>' (expected ui#<uiId>.<port> or ui#<uiId>.<element>.<port>)`、`malformed connection point '<t>'`、`malformed node port reference '<t>'`、`unknown node id '<id>'`、`node '<id>' has no <output|input> port '<p>'`、`unknown document port '<p>'`、`the document has no input port '<p>'`、`unknown <output|input> port '<p>' on '#<id>'`、`unknown element id '#<id>'`、`'#<id>' is not an <anim> element (port '<p>')`、`port '#<id>.<p>' is a struct/list port (not connectable yet)`、`port '#<id>.<p>' is an output (out) port`、`port '#<id>.<p>' is an input (in/call) port`、§53.5.5 的 `ui#` 錯誤、§53.4.16 的 `filter` 錯誤、`cannot connect a pulse output to '<to>'`、`type mismatch on edge '<from>' -> '<to>'`、`edge '<from>' -> '<to>' has an invalid 'instance' (expected a non-negative integer)`、`value input '<to>' has more than one incoming edge`、`#<id>.setState needs a "state" edge param`、`#<id>.setState has an invalid "mode" edge param "<x>" (expected on, off or toggle)`、§53.6.1 第 3 條的參數錯誤、`cycle detected in the node graph`。
- 測試：`FlowGraph::parse: malformed JSON is rejected`、`FlowGraph::parse: type mismatch (string source -> a strict number sink) is one error`、`FlowGraph::parse: a value input with two incoming edges is one error`、`FlowGraph::parse: a node-to-node cycle is one error`、`FlowGraph::parse: unknown node type is an error`、`FlowGraph::parse: unknown port on a known node type is an error`、`FlowGraph::parse: setStyle takes several edges, other value inputs do not`、`FlowGraph::parse: a pulse self-edge is not a cycle`、`FlowGraph::parse: T-262c a 1100-level .xtflow is rejected and the process lives`、`FlowGraph::parse: T-262c 32 levels parse; 33 levels is rejected`、`T-406 .xtflow version: missing = 1, newer refused, malformed rejected, written as the latest`、`T-260c: FlowRuntime::load failure leaves the runtime empty`。

### 53.6.3 連續性診斷 `flow.animatesWithContinuousSignal`（27 §27.4.3，T-540）

- 連續來源：`#document.{clock, viewportWidth, viewportHeight, pointerX, pointerY, pointerNX, pointerNY}`（`ui#` 兩段寫法同）、元素 `hover`、`<anim>.localTime`、`clock.seconds`。
- 傳播（依拓樸序）：值型邊經 `math`、`toString`、`format`、`select`（只看 `a`／`b`）、`filter`／`throttle`（只看 `in`，標記在 `out`）延續；`compare`、`lookup`、`select.cond`、`merge` 及其他節點切斷；pulse 邊不帶標記（`select` 的細節見 §53.99 第 3 項）。
- 終點為元素 `setProperty`／`setStyle`／`playAnimation` 且來源帶標記 → 一行：`flow.animatesWithContinuousSignal: edge <from> -> <to> drives a visual property from a continuous signal; author it as a timeline or a bind: instead`，依 `edges` 順序、去重。
- 嚴格度：`xtframe-view`、`xtframe-pack`、編輯器印**警告**、不拒載；`--strict` 時為錯誤（view exit 2、pack 失敗）。不是格式變更。
- 測試：`FlowRuntime::continuityWarnings: continuous sources driving visual inputs are reported`、`FlowRuntime::continuityWarnings: cut, pulse, discrete and non-visual paths report nothing`、`T-543 continuityWarnings: filter and throttle keep the mark like math; a port write is silent`、`T-540: continuityWarnings is recomputed by rebind()`、`T-540: a continuous signal into a declared port is never reported`。

### 53.6.4 執行期保護與錯誤

- **回饋保護**（經文件繞回的迴路）：同一條邊在一個 tick 內投遞超過 1000 次，或傳播巢狀超過 256 層 → 該邊本 tick 停止投遞，並在 `runtimeErrors()` 記一次（每邊每次載入一次）：`flow feedback loop: edge '<from>' -> '<to>' fired more than 1000 times in one tick; propagation stopped`／`… nested deeper than 256 pushes; propagation stopped`。測試：`FlowRuntime: a feedback loop through the document is stopped and reported`。
- 其他 `runtimeErrors()`：`state.set` 未知名稱、`timeline` 找不到時間軸（各一次）。
- 停用（`setEnabled(false)`／`enableFlow(false)`）：保留綁定與節點狀態，`tick()` 不做事、沒有輸出傳播、期間到達的文件事件丟棄。測試：`host flow interface: enableFlow stops propagation and ticking; per-UI flows follow`。

### 53.6.5 與綁定的交界

- `tween` 首寫（27 §27.3.3，T-546）：綁定的初始求值與第一次 port 驅動寫入直寫，第二次起才補間——flow 經 `set(port)` 送的第一個值不會從預設值補間進場。
- hover 與綁定同幀順序：§53.5.6。

## 53.7 宿主介面

### 53.7.1 `FlowRuntime`（`xt/flow/flow_runtime.h`）

| 成員 | 語意 |
|---|---|
| `Status load(const FlowGraph&, Document&, std::vector<std::string>& errors)` | 單一文件模式（UI 流圖）；驗證 + 綁定；失敗 → runtime 為空。在自己的派送中呼叫 → 延到派送結束（回 `kOk`，最後一次勝出）。JSON 先經 `FlowGraph::parse()`（見 §53.99 第 6 項） |
| `Status loadGlobal(std::string_view json \| const FlowGraph&, BundleContext&, errors)` | 全域模式（`bundle.xtflow`） |
| `void unload()` | 拿掉所有 listener、丟掉圖 |
| `Status rebind(Document&, std::vector<std::string>& warnings)` | 文件重載後重綁；節點狀態保留；消失的端點警告（`#id.port: element not found`／`not an <anim> element`／`port not found`）並丟掉該邊；`setState` 未宣告狀態、效果位址失效亦警告；未成功載入 → `kInvalidArgument`；全域 runtime 不支援 |
| `void tick(float dt)` | §53.1.3 |
| `std::span<const PortValue> values()`、`const std::vector<std::string>& valueNames()` | 每節點輸出值（節點宣告順序 × 節點輸出順序），名稱 `<nodeId>.<port>` |
| `const std::vector<std::string>& runtimeErrors()` | §53.6.4 |
| `std::span<const float> edgeLastFired()` | 每條邊最後投遞的 runtime 時間（秒），從未 = −1 |
| `void continuityWarnings(out)`、`void loadWarnings(out)` | §53.6.3；`loadWarnings` = 連續性 + `setState` 未宣告狀態（全域模式無後者） |
| `Status postSystemEvent(kind, value = {})`、`std::span<const std::string_view> systemEventKinds()` | §53.4.18 |
| `void setHostEventListener(FlowHostEventListener)` | §53.4.19；跨 `load()` 保留 |
| `void setEnabled(bool)`、`bool enabled()`、`bool isGlobal()`、`void setPerfTracker(PerfTracker*)` | — |

`FlowHostEvent { kind: Sound|Haptic|Spawn|Emit, name, volume, value, uiId, instanceId, nodeId }`。

### 53.7.2 `BundleContext`（`xt/flow/bundle_context.h`）

uiId → `DocumentHandle`（多實例可多個）、擁有全域 runtime 與每 UI runtime、統一開關：`addUi(uiId, handle)`／`addUi(uiId, owner, handle)`、`removeUi`、`uiIds`／`handles`／`instances`／`uiOf`、`enableFlow(bool)`、`flowRuntime()`、`loadGlobalFlow(json, errors)`、`loadUiFlow(uiId, json, errors)`（每實例各載一次）、`uiFlowRuntime(handle)`、`tick(dt)`（在 `Context::update()` 之後）、`postSystemEvent`（經 Context 路由給所有流圖）。生命週期與重入規則見 13 §13.10。

### 53.7.3 `HostPorts`（`xt/flow/host_ports.h`）

不跑流圖、只用連接點的宿主：`bind(doc)`、`setInput("#id.port", PortValue, param)`、`addOutputListener("#id.port" | "document.port", fn)`（pulse 每事件一次、值只在改變時）、`tick(dt)`、`ports()`、`rebind`；宣告 port 的新一代 API（`lookup`／`set`／`get`／`call`／`list*`……）見 13 章。測試：`test_host_ports.cpp` 全部、`HostPorts: lookup / set / get / call / emit listeners / converters / portTable / instanceId`。

### 53.7.4 C ABI 對應（50 章）

| C ABI | 對應 |
|---|---|
| `xt_surface_desc.run_flow = 1` | 載入該 UI 的 `.xtflow` 與 `bundle.xtflow`（`BundleContext`） |
| `xt_ui_info.has_flow` | `UiInfo::flow` 非空 |
| `xt_surface_post_system_event(s, kind, value)` | `postSystemEvent` |
| `xt_surface_set_host_event_listener` + `xt_host_event{kind, name, volume, ui_id, instance_id, node_id}` | `setHostEventListener`／`FlowHostEvent`（C 結構不含 `value`，`emit` 的值目前到不了 C 宿主——50 章與程式一致，記為觀察） |
| `xt_surface_connection_set_*`／`_pulse`／`_watch`／`_unwatch` | `HostPorts::setInput`／`addOutputListener`（連接點名稱同 manifest） |
| `xt_surface_set_port_listener`（`XT_PORT_CONNECTION`） | 連接點 watch 的回呼（`value_type` 為 `PortType` 0 pulse／1 bool／2 number／3 string） |
| `xt_surface_hot_reload` | `reloadUi` + flow `rebind` + ports `rebind` |
| `xt_capi_set_log` | flow 錯誤、rebind 警告、載入診斷 |

## 53.8 Conformance：外部執行器的最小等價證明

### 53.8.1 方法

外部執行器（另一語言的直譯器、40 §40.6.2 的生成碼、Atrium Script 編譯產物）證明與 `xt_flow` 等價的方式：同一張圖、同一份文件、同一串輸入事件與 `dt`，**逐 tick 比較** `values()` 序列（節點宣告順序 × 輸出順序）、每個沉降點（setText／setStyle／set(port)……）收到的值序列、`FlowHostEvent` 序列、以及載入錯誤字串集合。

### 53.8.2 `random` 不比值

`random` 不參與值比較：每個節點自有一個以 `std::random_device` 播種的 `std::mt19937`（`flow_internal.h` 的 `NodeRuntime::rng`），兩次執行 `xt_flow` 自身就不同。conformance 對其他節點逐 tick 比較；對 `random` 只檢查每次 `trigger` 都送出一個落在 `[min, max]`（依 §53.4.6 的非有限與對調規則）的值，`integer` 時為整數；下游節點的比較以「把實際送出的 `random.value` 當作輸入」重放。候選（不屬本章）：`random` 加 `seed` 參數，讓 conformance 也能比值。

### 53.8.3 最小測試集

| 類別 | 必須重現的 `Tests/flow` 案例（行為與錯誤字串） |
|---|---|
| 格式 | `FlowGraph::parse: the Docs/design/17-editor.md §17.6 sample graph`、`FlowGraph::toJson: round-trips Examples/desktop_app/ui/app.xtflow`、`T-406 .xtflow version: missing = 1, newer refused, malformed rejected, written as the latest`、`xtflow: pos and groups round-trip and are optional tail keys`、`FlowGraph::parse: T-262c 32 levels parse; 33 levels is rejected` |
| 結構規則 | `FlowGraph::parse: type mismatch (string source -> a strict number sink) is one error`、`FlowGraph::parse: a value input with two incoming edges is one error`、`FlowGraph::parse: a node-to-node cycle is one error`、`FlowGraph::parse: a pulse self-edge is not a cycle`、`FlowGraph::parse: unknown node type is an error`、`FlowGraph::parse: unknown port on a known node type is an error`、`FlowGraph::parse: setStyle takes several edges, other value inputs do not`、`FlowGraph::parse: select's type comes from its a/b edges`、`state node: every load-time error, verbatim`、`T-543 filter: every load error, verbatim`、`T-543 throttle: pulses pass through \`in\`, types follow it, every load error verbatim` |
| 節點行為 | §53.4 每節表後列出的 `test_flow_nodes.cpp` 與 `test_flow_runtime.cpp` 案例（`random` 依 §53.8.2） |
| 傳播與合併 | `T-389: 100 pointer moves in one frame reach a shared format node once, with the last value`、`T-389: pulses stay one delivery per event, and see the values reported before them`、`T-389: a declared value port and an element's hover coalesce like the document ports`、`T-407: 100 pointer moves -- behavior 0 evaluates downstream 100 times, behavior 1 once`、`FlowRuntime: edgeLastFired() timestamps each edge's last delivery`、`FlowRuntime: a feedback loop through the document is stopped and reported` |
| 連接點 | `FlowRuntime: end-to-end against runtime_doc.xt`、`FlowRuntime: #document pointer ports follow the pointer`、`FlowRuntime: edges into and out of declared ports`、§53.5.1 列的元素輸入案例 |
| 全域與宿主 | `global flow: three-segment ui# references drive one UI from another`、`global flow: two-segment references reach document-level declared ports`、`global flow: binding errors`、`global flow: ui#<id>.shown / hidden follow the Context lifecycle`、`multi-instance: instance-filtered sources and sinks`、`system events: pulses and strings reach system nodes; unknown kinds are rejected`、`action nodes: sound, haptic, spawn and emit send host events with the UI and instance`、`timeline node: play / seek / stop a named timeline, end and marker outputs` |
| 診斷 | `FlowRuntime::continuityWarnings: continuous sources driving visual inputs are reported`、`FlowRuntime::continuityWarnings: cut, pulse, discrete and non-visual paths report nothing`、`T-543 continuityWarnings: filter and throttle keep the mark like math; a port write is silent`、`T-546: loadWarnings() reports an undeclared setState state right after load()` |

範例圖（必須載入成功且 `--strict` 無連續性警告）：`Examples/desktop_app/ui/app.xtflow`、`Examples/demos/game-ui/hud/hud.xtflow`（19 節點／36 邊：counter、timer、clock、compare、select、toggle、toString、constant）、`Examples/demos/challenge-menu/ui-project/challenge-menu/challenge-menu.xtflow`（26／102：radio、lookup、select、timer、toggle、compare、constant）、`Examples/demos/npdo/ui-project/{hud,main-menu,settings}/*.xtflow`（timer + timeline）、`Examples/demos/npdo/ui-project/shared/bundle.xtflow`（全域：`ui#` + radio + lookup）、`Examples/demos/fairy-forest-menu/ui-project/demo3-menu/demo3-menu.xtflow`（空圖）、`Tests/flow/fixtures/state_machine.xtflow`。

不在 conformance 內：效能與零配置（`T-476`／`T-477`／`T-478` 的 `xt_flow_alloc_tests`、`T-475` 計數器）、生命週期重入（`test_flow_lifetime.cpp`）——它們是 `xt_flow` 的實作品質，不是語言語意。

## 53.99 文件與實作不一致，待主模型裁決

本節每項列：文件怎麼說、程式怎麼做、哪個測試（或實測）顯示。正文在這些點上描述程式現況。

| # | 文件怎麼說 | 程式怎麼做 | 測試／證據 |
|---|---|---|---|
| 1 | 舊 17.6：`compare` 的 `a`、`b` 為「number 或 string，兩者同型」 | `compare.a`／`b` 是彈性輸入（`isFlexibleSink()`），接受任何值型（含 bool）且不檢查兩者同型；混型時以數值比較（bool = 0／1，string 讀 0） | 無 `Tests/flow` 案例涵蓋混型；實測：`xtframe-view`（Release）載入 `toggle.value → compare.a`、`constant("x") → compare.b` 無錯誤 |
| 2 | 舊 17.6／§53.6.1：pulse 輸入允許多條邊（event 型宣告 port 映為 pulse） | `parse()` 無文件時宣告 port 型別一律暫記 string（`anyType`），扇入規則照值型計，兩條邊接同一個 event 型 `in` port 被拒（`value input '#Hud.fire' has more than one incoming edge`）；而 `load()` 有文件時同一張圖會放行。因宿主與 `xtframe-view` 都先 `parse()`，實際上被拒 | 無 `Tests/flow` 案例；實測：`xtframe-view ev.xt --flow ev.xtflow`（`port:fire="event in"`，`#a.Click`、`#b.Click` 都接 `#Hud.fire`）印出上列錯誤 |
| 3 | 27 §27.4.3 正文：「`compare`／`select` 的 bool／pulse 輸出與 `lookup` 的輸出切斷標記」 | `select.out` 只要 `a` 或 `b` 帶標記就延續（不論型別），只有 `select.cond` 切斷；27 §27.4.3 的 T-540 實作記錄寫的是程式這版 | `FlowRuntime::continuityWarnings: cut, pulse, discrete and non-visual paths report nothing` 只測 `select.cond` 切斷；沒有測 `select.out` 帶 bool 的情形 |
| 4 | 27 §27.4.4 規則列：「`enter*/exit*` 回接自己的事件輸入視為環」 | pulse 自環規則放行（同 `timer.tick → timer.stop`）；經第二個節點的迴路照報環。T-541 實作記錄說「本節『視為環』一句改以既有規則為準」，但規則列原文未改 | `state: events feeding back through enter / exit are the general cycle rule` |
| 5 | 27 §27.4.4：事件名「不得與固定埠衝突」，載入檢查列「事件名不與 `set`／`guard` 衝突」 | 也拒絕 `state`、`index`、`changed`（輸出名）：`state <id>: event "<x>" collides with a fixed port` | `state node: every load-time error, verbatim`（五個名稱全測） |
| 6 | 舊 17.6：`FlowRuntime::load(std::string_view json, Document&)` | `load(const FlowGraph&, Document&, std::vector<std::string>& errors)`；JSON 入口只有 `FlowGraph::parse()`、`loadGlobal(json, …)`、`BundleContext::loadGlobalFlow`／`loadUiFlow` | 標頭 `flow_runtime.h`；所有測試以 `parse()` + `load()` 兩步 |
| 7 | 舊 17.6：`playAnimation` 的時長／緩動「取該元素樣式表中同名 `animation` 宣告，否則 1s linear」 | 先取元素目前解析出的同名 `@keyframes` 的 `animation` 宣告；沒有（從未宣告或已被 `stopAnimation` 換成 none）就取整份樣式表中第一個編譯出的同名宣告；都沒有才 `1s linear` | `FlowRuntime: playAnimation plays the element's @keyframes, stopAnimation clears it`、`FlowRuntime: playAnimation with the same name again restarts the animation every time`（未分辨兩種回退） |
| 8 | 27 §27.4.5：trailing「窗口結束時若有暫存值就送出並開下一個窗口」 | 窗口已到期、但本 tick 的 trailing 送出還沒執行時到達的新輸入：立即通過、取代暫存值（暫存者計入 `dropped`），暫存值不送。T-543 實作記錄列為「偏離」 | 無專門案例；`T-543 throttle hz 10, trailing: leading A, B and C held, C sent at the window's end, then silence` 不涵蓋這個時序 |
| 9 | 27 §27.4.4：`set` 未知名稱「忽略並記一次**警告**」 | 記進 `runtimeErrors()`（`node '<id>': set: unknown state '<name>'`），不進 `loadWarnings()`／`rebind()` 的 warnings；名稱只在事件到達時比對 | `state: set jumps without the transition table; an unknown name is ignored and reported once` |
| 10 | 27 §27.4.1 指向「git 歷史 `Docs/design/17-editor.md` v0.1 §17.6」 | 該路徑在歷史中是小寫 `docs/design/17-editor.md`，而流圖版 §17.6 最後存在於 `56789f16^`；T-432（b41201a0）前一版的 §17.6 已是「空間、設計／動畫切換」 | `git show 56789f16^:docs/design/17-editor.md`；本章建議 27 §27.4.1 該句改為「見 53 章」（由主模型改） |
| 11 | 27 §27.4.5 `filter` inputs：「`deadband`／`quantize` 只准 number，bool／string 的 `deadband` 視為『值變了才送』」（前後半句互斥） | `deadband` 接受 bool／string（值變了才送）；只有 `quantize` 在載入時要求 number（`filter <id>: quantize takes a number`） | `T-543 filter deadband: bool and string send only when the value changed`、`T-543 filter: every load error, verbatim` |

### 53.99.1 裁決（2026-10-31，主模型）

以實作為準、改文件者（實作有測試且語意合理）：

1. `compare` 的 `a`／`b` 是彈性輸入（任何值型，含 bool）；混型時數值比較（bool 當 0／1、string 讀數）。
3. 連續標記：只有 `select.cond`（與 `compare` 的 bool 輸出、`lookup` 的輸出）切斷；`select.out` 在 `a` 或 `b` 帶標記時延續——「選兩個連續值之一」仍是連續。27 §27.4.3 正文改。
4. `state` 的 `enter*`／`exit*` 直接回接自己的事件輸入是 pulse 自環，依一般規則放行；經第二個節點繞回才是環。27 §27.4.4 規則列改。
5. `state` 的事件名不得與 `set`、`guard`、`state`、`index`、`changed` 衝突。
6. `FlowRuntime::load(const FlowGraph&, Document&, errors)`；JSON 經 `parse()`／`loadGlobal`／`BundleContext` 進來。
7. `playAnimation` 時長／緩動：元素目前宣告 → 樣式表第一個同名宣告 → 1s linear。
8. `throttle` trailing：窗口已到期而暫存值未送出時到達的新輸入立即通過並取代暫存值，被取代者計入 `dropped`。27 §27.4.5 改。
9. `state.set` 未知名稱記進 `runtimeErrors()`（一次），不是 `loadWarnings()`。
10. 27 §27.4.1 的歷史路徑改為指向本章。
11. `filter` 的 `deadband` 收 bool／string（值變了才送），只有 `quantize` 要求 number。27 §27.4.5 的前半句刪。

以文件為準、修程式者：

2. **T-560**：`parse()` 在沒有文件可查時，對宣告 port（`#Ui.<port>`、`ui#id.<port>`）的扇入檢查**延後到 `load()`**（那時知道型別：event 型 = pulse，允許多邊）；`parse()` 只檢查節點埠的扇入。測試：兩條邊接同一個 event in port 經 `xtframe-view` 載入成功；兩條邊接 number in port 仍在 `load()` 被拒、訊息不變。

順帶（不屬本章）：C ABI `xt_host_event` 沒有 `value` 欄位，`emit` 節點的值到不了 C 宿主 → 併入 T-558（C ABI 補充）。

