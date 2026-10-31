# 04 · 統一時間軸

本章是整個專案的核心。CSS transition、CSS `@keyframes`、Lottie 動畫、程式 API 動畫，**全部**編譯或轉換為同一種 `Track` 結構，由同一個 `Animator` 評估。

## 4.1 緩動（Easing）

```cpp
enum class EasingKind : uint8_t { Linear=0, Hold=1, CubicBezier=2, Steps=3 };

struct Easing {
    EasingKind kind;
    uint8_t    stepsJumpEnd;   // Steps 專用：1 = jump-end（CSS 預設 `steps(n, end)`），0 = jump-start
    uint16_t   stepCount;      // Steps 專用
    float x1, y1, x2, y2;      // CubicBezier 專用
};
```

`Easing` 描述**從本關鍵幀到下一關鍵幀**的區段。最後一個關鍵幀的 easing 被忽略。

三次貝茲求值：給定進度 `p ∈ [0,1]`，求 `t` 使 `bezierX(t) = p`，再回傳 `bezierY(t)`。解法固定為：先以 8 次牛頓迭代（初值 `t = p`），若導數小於 1e-6 或超出 [0,1] 則退回 32 次二分法。與瀏覽器（WebKit `UnitBezier`）行為一致。

CSS 關鍵字對應：

| 關鍵字 | Bezier |
|---|---|
| `linear` | kind = Linear |
| `ease` | 0.25, 0.1, 0.25, 1.0 |
| `ease-in` | 0.42, 0, 1, 1 |
| `ease-out` | 0, 0, 0.58, 1 |
| `ease-in-out` | 0.42, 0, 0.58, 1 |
| `step-start` | Steps(1, jumpStart) |
| `step-end` | Steps(1, jumpEnd) |

## 4.2 關鍵幀

```cpp
struct Keyframe {
    float  time;          // 秒，相對 Timeline 起點；嚴格遞增
    Value  value;         // 見 03.10
    Easing easing;        // 到下一幀
    // 僅 Vec2 型別使用（Lottie 空間貝茲）；非 Vec2 時忽略
    Vec2   tanOut;        // 本幀出切線（相對本幀 value）
    Vec2   tanIn;         // 下一幀入切線（相對下一幀 value）
    uint8_t hasSpatial;
};
```

## 4.3 求值單一 Track

輸入：`Track`、局部時間 `t`。輸出：`Value`。

```
若 keyframes 為空                → 無輸出（Track 無效，編譯器不會產生）
若 t <= kf[0].time               → kf[0].value
若 t >= kf[last].time            → kf[last].value
否則找到 i 使 kf[i].time <= t < kf[i+1].time（二分搜尋；Track 快取上次 i，先檢查 i 與 i+1 再退回二分）
    p = (t - kf[i].time) / (kf[i+1].time - kf[i].time)
    依 kf[i].easing：
        Hold          → 回傳 kf[i].value
        Linear        → e = p
        CubicBezier   → e = bezierY(solveX(p))
        Steps(n,end)  → e = floor(p*n)/n；jumpStart 則 e = ceil(p*n)/n；夾到 [0,1]
    若型別為 Vec2 且 kf[i].hasSpatial：
        P0 = kf[i].value, P1 = P0 + tanOut, P3 = kf[i+1].value, P2 = P3 + tanIn
        回傳 cubic(P0,P1,P2,P3, e)
    否則回傳 lerp(kf[i].value, kf[i+1].value, e)（依 03.10 規則）
```

## 4.4 Track 與 Timeline

```cpp
struct Track {
    NodeId    target;        // 場景局部索引；kSelfNode = 0xFFFFFFFE 表示「套用的元素自身」
    PropId    prop;
    ValueType type;
    uint32_t  kfOffset, kfCount;   // 指向 Timeline::keyframes
    // 執行時快取
    uint32_t  lastIndex;
};

struct Marker { StringId name; float start; float duration; };

struct Timeline {
    StringId  name;
    float     duration;          // 秒
    float     frameRate;         // 提示，Lottie 來源保留原 fps，CSS 來源為 0
    std::vector<Track>    tracks;
    std::vector<Keyframe> keyframes;
    std::vector<float>    floatPool;    // Path / FloatArray 值的點資料
    std::vector<GradientStop> stopPool;
    std::vector<Marker>   markers;
};
```

Timeline 來源：
- **Lottie**：一個 Lottie 檔 → 一個 Scene + 一個 Timeline，`target` 為場景局部 NodeId。
- **CSS `@keyframes name`**：一個 `@keyframes` → 一個 Timeline，`duration = 1.0`（正規化，播放時以 `animation-duration` 縮放），所有 `target = kSelfNode`。
- **程式 API**：宿主呼叫 `Document::createTimeline()` 動態建立，格式相同。

## 4.5 TimelineInstance（播放器）

```cpp
enum class LoopMode : uint8_t { Once=0, Loop=1, PingPong=2 };
enum class FillMode : uint8_t { None=0, Forwards=1, Backwards=2, Both=3 };

struct TimelineInstance {
    uint32_t   id;              // 宿主持有的控制代碼
    TimelineId timeline;
    NodeId     baseNode;        // 場景實例的根偏移；track.target + baseNode = 實際 NodeId；kSelfNode 時直接用 selfNode
    NodeId     selfNode;        // kSelfNode 的實際對象（CSS animation 用）
    float      startClock;      // Context clock 時刻
    float      delay;           // 秒
    float      speed;           // 預設 1.0，可負
    float      durationScale;   // CSS 動畫為 animation-duration；Lottie 為 1
    float      segStart, segEnd;   // 播放區段（局部秒），預設 0..duration
    LoopMode   loop;
    uint16_t   iterations;      // 0 = 無限；Once 時視為 1
    FillMode   fill;
    uint8_t    paused;
    float      pausedAt;        // 暫停時的局部時間
    uint8_t    layer;           // 層疊層級，見 4.7
    uint8_t    finished;
};
```

局部時間計算：

```
elapsed = (clock - startClock - delay) * speed
若 paused：elapsed = pausedAt
segLen = (segEnd - segStart) * durationScale
若 elapsed < 0：
    fill 含 Backwards → localT = segStart（若 speed<0 則 segEnd）；否則本幀不套用
否則：
    iter = floor(elapsed / segLen)
    若 iterations != 0 且 iter >= iterations：
        finished = 1
        fill 含 Forwards → localT = 結束端值（依 loop/方向決定是 segEnd 或 segStart）；否則不套用且下一幀從池移除
    否則：
        frac = elapsed - iter*segLen
        PingPong 且 iter 為奇數 → frac = segLen - frac
        localT = segStart + frac / durationScale
```

`onFinished` 在實例**首次**進入 `finished` 時觸發一次，與是否移除無關；fill 不含 Forwards 的實例在該幀評估後從池中移除（回呼於移除後呼叫）；含 Forwards 者留在池中保持終值。`seek` 重設 `finished` 與回呼狀態。

邊界時刻：`elapsed` 恰為 `segLen` 的整數倍 `k·segLen` 時，視為第 `k−1` 次迭代的**結束幀**（`localT` 為該迭代的終點），不是第 `k` 次的起始幀；因此 `Once` 在 `elapsed == segLen` 時仍輸出終值且尚未 `finished`。`speed == 0` 時 `seek/setSpeed/resume` 不調整 `startClock`。`segEnd <= segStart` 的實例視為立即 finished 於 `segStart`。`stop()` 不觸發 `onFinished`。

## 4.6 Transition

CSS `transition` 不在 IR 存 Track，而是執行時動態產生：

```cpp
struct Transition {
    NodeId   node;
    PropId   prop;
    Value    from;          // 觸發當下的**最終值**（含正在跑的其他動畫效果）
    Value    to;            // 新基底值
    float    startClock, delay, duration;
    Easing   easing;
    uint8_t  finished;
};
```

觸發規則（在 StyleEngine::resolve 之後）：
1. 元素的 `ComputedStyle` 有 `transition` 宣告涵蓋該屬性（或 `all`）。
2. 該屬性的基底值與上一幀不同，且型別可插值（03.10 中非「離散」的情況；離散型別不觸發）。
3. 若同一 `(node, prop)` 已有 Transition 進行中：以當下最終值為新 `from`，取代舊 Transition（CSS 的「reversing」簡化版，不做 reversing shortening）。

求值：`e = ease((clock - startClock - delay) / duration)`，夾在 [0,1]；完成後移除。

## 4.7 屬性層疊順序（每幀）

對每個被寫入的 `(node, prop)`，最終值依以下順序覆蓋，**後者勝**：

| 層 | 來源 | 說明 |
|---|---|---|
| 0 | 基底值 | Element：ComputedStyle；場景節點：IR 初始值 |
| 1 | Transition | 4.6 |
| 2 | TimelineInstance，`layer = 0` | Lottie 動畫、CSS animation。同層多個實例依 `startClock` 升冪，晚啟動者勝 |
| 3 | TimelineInstance，`layer = 1` | 宿主 API 明確指定的高優先動畫 |
| 4 | Override | 宿主 `setProperty(..., sticky=true)` 設定的值，直到 `clearOverride` |

實作：Animator 每幀依上表順序依次呼叫 `setProperty`。不做混合（blend/weight），第一版不需要。

> **2026-10-30 補充（T-545；使用者裁決「效果參數綁定在 restyle 後遺失」只走正解）**：第 4 層 Override 是唯一在樣式重解析（class／狀態變化重建效果堆疊）後仍存在的層，所以**所有「資料驅動」的寫入都走第 4 層**：`bind:` 的目標（含 `bind:effect.<效果>.<參數>`，18 §18.16）與 flow 的 `#id.setProperty`／型別化 `setStyle` 寫入，一律 `sticky = true`；Animator 每幀重套，restyle 後效果堆疊重建也不會丟。為了讓 `| tween(d)` 與 Override 並存（第 1 層 Transition 會被第 4 層蓋掉），**Override 可帶補間**：`Animator::setOverride(node, prop, value, duration = 0, easing)`——duration > 0 時從目前 Override 值（沒有則從現值）補間到新值，補間中再設定即從目前補間值重新起算；Override 的值永遠是「目標值」，`clearOverride` 連補間一起清。效果 PropId（0x8000+）在 Override 表裡與一般 PropId 同樣以 `(node, prop)` 為鍵；效果當下不存在時每幀的重套為 T-211 的 no-op（警告每份文件一次），效果回來即生效。一般 `Document::setProperty(..., sticky=false)` 第 0 層語意不變。

## 4.8 Animator 介面（核心內部）

```cpp
class Animator {
public:
    uint32_t play(const PlayParams&);                // 回傳 instance id
    void     stop(uint32_t id);                      // 立即移除，不套 fill
    void     pause(uint32_t id);  void resume(uint32_t id);
    void     seek(uint32_t id, float localT);
    void     setSpeed(uint32_t id, float);
    bool     isFinished(uint32_t id) const;
    void     addTransition(const Transition&);
    void     setOverride(NodeId, PropId, const Value&);
    void     clearOverride(NodeId, PropId);
    void     evaluate(float clock, NodeArena&);      // 1.5 步驟 3.2
    // 回呼
    std::function<void(uint32_t id)> onFinished;
    std::function<void(uint32_t id, StringId marker)> onMarker;   // localT 跨過 marker.start 時觸發
};
```

`evaluate` 內部：先蒐集所有 Transition 輸出到 `pending` 陣列（`(node, prop, value)`），再依層與 startClock 排序 TimelineInstance，逐一求值追加到 `pending`，最後 Override 追加。然後順序遍歷 `pending` 呼叫 `setProperty`。`pending` 容量跨幀保留。

Marker 觸發：對每個實例記錄上幀 `localT`；本幀 `localT` 與上幀之間（考慮方向）若含 `marker.start`，觸發一次。循環回繞時視為兩段區間。

## 4.9 效能約束

- Track 求值不得配置記憶體。Path/Stops/FloatArray 插值寫入節點的既有 vector（長度由編譯器保證一致；若不一致，`XT_ASSERT` 並跳過）。
- 1000 條 Track 的 Timeline 在 Debug 建置下 `evaluate` 應在 1 ms 內（測試 15.4）。

## 4.10 編輯器藍圖新增（2026-09-27 定案；對應 28 章）

### 4.10.1 TimelineGroup

`@timeline`（05.9）編譯成一組 `@keyframes` + 每個元素的 `animation` 項，再以 **TimelineGroup** 綁在一起：`{ name, scope (root 或符號實例路徑), duration, markers[], sounds[], members: {node, timelineId, delay}[] }`。群組共用一個時鐘：`play(opts{reverse, loop, once, hold, from: marker})`、`stop`、`seek(t)`、`rate`；成員的 `delay` 換算為群組時間。事件：`end`、`marker(name)`（08.6 動畫事件擴充），`sounds[]` 到時發 `#scope.sound(cue)`（13.8）。群組播放時成員各自的 `animation` 宣告暫停（同屬性以群組為準；層疊順序在 4.7 之後、transition 之前）。

### 4.10.2 片段時鐘與 sync

`<use>`／`<anim>`／`<video>`／`<sequence>` 帶 `sync` 時，父群組 seek 即帶動片段本地時間 `(t - clipStart) * clipRate`；非 sync 為獨立時鐘（觸發或宿主控制）。

### 4.10.3 自動狀態過渡（FLIP）

`state-transition: <duration> [<easing>]` 或 `auto` 時，狀態切換（含 `:state()`、變體切換、`<repeat>` 增刪）在佈局前後各取一次快照（位置、尺寸、透明度、圓角），差異以 transform + opacity 補間（不重排），時長與緩動取自宣告；子項各自補間；新出現的元素以 `item-enter` 或淡入，消失的以 `item-exit` 或淡出後移除。實作為 4.6 Transition 的特例（來源值 = 快照）。

### 4.10.4 `tween()` 轉換器

綁定鏈中的 `tween(duration, easing)` 對數值型目標建立一段 transition（4.6），來源值變化即重新起算；用於計數器與血條。

### 4.10.5 T-172 實作記錄（#171）

- 不對元素寫入合成的 `animation` 宣告（會覆蓋元素自己的 `animation:` 清單）；成員資訊在 TGRP。
- 群組成員只在 `play`／`seek` 時建立；之前元素顯示樣式值（不是暫停的第 0 幀）。
- 同屬性的「暫停元素自身動畫」以求值順序實現：群組在元素動畫之後套用而勝出；元素自身時鐘照跑。
- 結束：無 `hold` 時釋放成員並明確寫回樣式值（效果參數取快照）；`hold` 停在末幀直到 `stopTimeline`。
- 標記在播放中與同方向 seek 時觸發；起點上的標記會觸發，`fromMarker` 起播除外。
- 預設 `duration` = max(最後關鍵幀, 最後標記, 最後音效)。
- 緩動寫在「到達的關鍵幀」上，編譯時移到前一個 stop（逐屬性）；屬性軌道比時間軸短時前後保持首末值。
- `<anim sync>` 目前固定 `clip-start 0`、`clip-rate 1`（屬性待 T-17x）；符號範圍內的 `@timeline` 自 T-190 起逐實例改寫（軌道路徑加前綴、群組 scope = 實例根），`<use sync>` 實例的群組隨父群組 `(t - clip-start) * clip-rate`；內嵌 `<svg>` 的場景時間軸以秒作為群組成員；音效 cue 已存於執行時但尚未送宿主（13.8.3 port 待做）。

> T-218（#219）：`Animator::Transition` 新增 `NodeId target`（實際寫入的節點；元素自身 transition 的 `target = node`）與 `written()`；`TransitionEnd` 仍送給擁有者元素 `node`；`clearNode` 一併清除以場景節點為 target 的條目。向量屬性（`d` 除外）可 transition。

> T-228（#234）：通用 PropId 14 `AutoOrient`（Enum none=0／path=1）；空間貝茲 key 的 `.xts` 語法 `motion-in`／`motion-out`（只在 key 內，兩軸 px，相對該幀 translate；帶 motion 的 `transform: translate` 軌合併成 Vec2 Translate 軌）；`.xtanim.json` key 第 4 元素 `[inX, inY, outX, outY]`。執行期 `NodeCommon::orientPathDeg` 由 Animator 從 Translate 軌導數寫入，組合變換時加到 rotation。

## 4.11 Animator 通知的順序與失效（2026-10-02 審查修正；T-260a #273）

`onTransitionEnd`、`onIteration`、`onMarker` 不在求值途中呼叫：先排入佇列，於第 6 步之後、`onFinished` 之前依發生順序送出。巢狀的 `evaluate()` 把自己的通知留給下一次。一則通知在下列情況被丟棄：

- 它的節點已被銷毀，或槽被重用（以 generation 判斷）；
- 它的 instance 被同一批較早的 callback `stop()` 了（第 6 步自己因播完而移除的 instance 仍送出最後的事件）；
- 較早的 callback 清掉了該節點的 transition（`clearNode()`／`cancelTransitionsForNodeRange()`）。

iteration listener 對於在同一幀結束的 instance 會看到 `isPlaying() == false`。

## 4.12 時間參數的非有限值（2026-10-07 審查修正鏈合併；T-255b #318）

`evalTrack`／`Animator::seek`／`Document::seekTimeline`：`+Inf` 表示結尾（`-Inf` 為 0）；NaN 讀為 0。取代 02 §2.w 記的「非有限＝0」。`seekMedia` 對非有限時間回 `kInvalidArgument`。
