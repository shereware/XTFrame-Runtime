# 21 · 內建效能追蹤

版本：0.2.1。狀態：設計定案，未實作。

目的：不靠外部 profiler 就能回答「這一幀花在哪裡」。對象是引擎整合者、UI 作者，以及用提線除錯的 AI。預設關閉；關閉時的成本是每個階段一個分支。

## 21.1 量什麼

每幀一筆 `FrameStats`，分三組：

| 組 | 欄位 | 來源 |
|---|---|---|
| 時間（毫秒） | `updateMs`（整個 `Document::update`）、其下 `inputMs`、`styleMs`、`layoutMs`、`animateMs`、`transformMs`；`paintMs`（`Document::paint` 產生命令清單）；`renderMs`、`effectsMs`、`textMs`（後端回報，見 21.3） | `std::chrono::steady_clock`，階段邊界在 `Document::update/paint` 內 |
| 工作量 | `styledElements`、`laidOutElements`、`activeAnimations`、`activeTransitions`、`commands`、`layers`、`effectLayers`、`projections`、`glyphs`、`pathPoints`、`imageDraws` | 各階段既有的迴圈計數；命令清單的統計取自 `CommandList` |
| 記憶體 | `arenaNodes`、`commandBytes`（命令清單各 vector 的 size×sizeof 總和） | 每幀取樣 |

`frameIndex`、`timeSeconds`（Context 時鐘）隨每筆記錄。

## 21.2 API（`Code/Runtime/core/include/xt/perf.h`，零相依）

```cpp
namespace xt {
struct FrameStats { /* 21.1 的欄位，全部 float / uint32_t，POD */ };
struct PerfSummary { FrameStats avg, max; float fpsAvg; uint32_t frames; };

class PerfTracker {
public:
    void setEnabled(bool);  bool enabled() const;
    void beginFrame(float timeSeconds);              // Context::update 開頭
    void endFrame();                                  // Context::render 結尾（或下一個 beginFrame）
    FrameStats& current();                            // 各階段填欄位
    std::span<const FrameStats> history() const;      // 環狀緩衝，最近 240 幀，由舊到新
    PerfSummary summary(uint32_t lastN = 120) const;
    void reset();
};

struct PerfScope {                                    // RAII：建構時取時間，解構時把經過的毫秒「加到」*field
    PerfScope(PerfTracker& t, float FrameStats::* field);
    ~PerfScope();
};
}
```

- `Context::perf()` 回傳該 Context 的 `PerfTracker&`；`Document` 經 Context 取得。未啟用時 `PerfScope` 的建構/解構各只有一個分支。
- 後端回報：`SwRenderer::render` / `GlRenderer::render` 多一個可選參數 `PerfTracker* perf = nullptr`；非 null 且啟用時填 `renderMs`（整個 render 呼叫）、`effectsMs`（PopEffect 內）、`textMs`（DrawGlyphs 內）。GL 的時間是 CPU 提交時間（`glFinish` 不呼叫），文件與面板上要標明。
- 核心不做輸出；呈現交給宿主。

## 21.3 呈現

| 位置 | 內容 |
|---|---|
| `xtframe-view --perf` | 視窗標題每 0.5 秒更新 `fps | update a.b ms | render c.d ms`；結束時（含 `--headless`）在 stdout 印一份 `PerfSummary` 表（各階段 avg / max、工作量 avg）；`--perf-csv <file>` 逐幀寫 CSV |
| 編輯器 | 新面板「Performance」（底部 tab）：啟用勾選、最近 240 幀的堆疊長條圖（style / layout / animate / paint / render 各一色，`QPainter` 自繪，16.6 ms 與 33.3 ms 參考線）、右側數字表（avg / max、工作量）；狀態列常駐 `fps`。面板關閉或未勾選時 tracker 關閉 |
| 編輯器自身 | `MainWindow::onFrame` 各段（`session.tick`、每個面板的 `tick`、`requestRender`）與各預覽 widget 的 `paint` 耗時，另列一組「Editor」長條與數字；事件迴圈的幀間隔（實際 fps）。理由：2026-09-22 的卡頓是 `FlowPanel::tick` 每幀 59 ms，引擎階段的數字完全看不出來 |
| 提線 | `perf.enable(bool)`、`perf.last`、`perf.history([n])`、`perf.summary([n])`、`perf.reset` |
| 公開 API | 宿主自行讀 `Context::perf()`；`Examples/engine_integration` 示範每 120 幀印一次摘要 |

## 21.4 任務

| 任務 | 內容 |
|---|---|
| T-76a | `perf.h`/`perf.cpp`、`Context::perf()`、`Document::update/paint` 的階段計時與工作量計數、SW/GL 後端回報、單元測試（啟用時欄位非零且階段和 ≤ `updateMs`；停用時 history 不成長）、`xtframe-view --perf/--perf-csv`、`engine_integration` 示範 |
| T-76b | 編輯器 Performance 面板與狀態列 fps、提線 `perf.*` 接點 |

## 21.x 實作記錄（T-239 #239／#240）：編輯器 SW 預覽效能

使用者 2026-10-01 回報 SW 預覽（獨立編輯器、宿主窗格、UI 瀏覽器）嚴重卡頓；調查（main@3801d64）：編輯器每幀整幀重畫不用 damage（實際變動 4–22%）、非 premul 目標（半透明 3.0 vs 1.3 ns/px）、放大時算圖尺寸 = 畫布×zoom×DPR、overlay 每 tick 整片重貼、layer／effect 不受 damage 限制、每 widget 各自 50% 節流。修正：A1 `SwPreviewWidget::requestRender()` 先建命令清單、只 `update()` damage 區域、`clearRects` + 帶 damage 的 render、`PreviewSession::renderSerial()` 偵測中途他人 render 則整幀；A3 `PreviewOverlay::refreshIfChanged()`（只在模式／尺寸／transform／選取幾何／EIP／向量模式／錄製／onion／revision 變時重畫，Demo 不重畫）；A4 `Format_RGBA8888_Premultiplied` + `premultiplied=true`（滴管 `pixelColor()` unpremul）；B3 `SwTarget::originX/originY`（命令、damage、clearRects 皆畫布座標，3D 投影還原原點）；A2 影像範圍 = widget ∩ 父窗格，`moveEvent` 重畫、螢幕外 damage 略過；B1 根層 PushLayer／PushEffect 緩衝框與 damage 無交集時整段跳過（巢狀或投影區段照常；mask 隨外層）；B4 `addLine` scratch；A5 選**全域節流預算**（所有 SwPreviewWidget 共用，佔空比 ≤ 50%）——背景執行緒需 per-worker resolver 與 IR 生命週期釘住，列後續。數字（Release、ms/tick）：hud 1080p 10.0→3.4、pause 1080p 18.5→8.9、hud 200% 27.4→0.36、pause 200% 96.4→20.3（效果模糊半徑隨 dpr 放大）。測試 `Tests/editor_qt/test_sw_preview_perf.cpp`（只印數字；斷言 damage < 25%、無變動不 render、他人 render 後整幀）。golden 與 partial_* 逐 byte 不變。宿主窗格的 60 Hz 上限與隱藏不 tick 由各宿主自己處理。

T-450 實作記錄：`FrameStats` 加編輯器分項 `editorDocChangedMs`（`documentChanged()` 仍同步執行的扇出）與 `editorPanelsMs`（預覽畫出編輯之後、合併延後的面板刷新），由編輯器 session 填寫、其他宿主恆為 0，列入 `XT_FRAME_STATS_FLOAT_FIELDS`（`--perf-csv` 與 `perf.*` 因此多兩欄），效能面板時間表顯示這兩列。

## 21.5 審查修正鏈補記（2026-10-07 審查修正鏈合併；T-270c #291、T-270d #302、T-263d #293）

- `FrameStats::runtimeStrings`、`maxDistinctBindingStrings`；`perf.probe` 的 `document` 物件多 `runtimeStrings`、`maxDistinctBindingStrings`、`unattachedNodes`。綁定每個 port 超過 1024 個不同新字串時記一次警告 `binding "<source>" produces unbounded distinct strings; bind it to text or a String port instead of a class / converter argument`，計數繼續。
- `xt_perf` 在 T-263d（來源表）前後皆零配置；dev_check 套件 `perf`。來源表的固定成本：mobile-hud 1080×2400 300 幀，update／paint／render 平均值在誤差內。

## 21.6 flow 預算（T-403，2026-10-21）

- **量什麼**：flow＋綁定 = `FrameStats::flowMs`（`FlowRuntime::tick`）＋綁定重算（`updateMs` 減去 input／style／layout／animate／transform 五個階段的剩餘，即 `Document::update` 開頭 `flushDirty`／`tick` 所在的未計時段；上限估計，也含連續觸發器結算與事件 tick）。sink 引起的下游 style／layout／paint 不算在此預算內，各歸其階段。
- **怎麼量**：`Tests/perf/flow_big/`（`gen_flow_big.py` 確定性產生）的兩份 fixture：典型 200 節點／40 sink、大圖 500 節點／100 sink，同一組 50 條值型來源（25 條每幀變化）。`xtframe-view --project <fixture> --headless --perf --frames 600 --synth-pointer 16 --synth-ports 20`，Release，取後 480 幀；`Tests/perf/flow_big/flow_bench.py` 算平均與 p95。`--synth-ports N` 是本單為量測加的開關（每幀寫前 N 個文件層 number port）。
- **預算**（平均 ms／幀，1080p SW 繪製之外）：**典型 ≤ 0.5**、**大圖 ≤ 2.0**。p95 只記錄不設線。超過即開 B17 的 B7（flow 整數化）。
- **基準（2026-10-21，i5-13600K）**：Windows MSVC 典型 0.141（p95 0.18）、大圖 0.257（p95 0.32）；WSL GCC 典型 0.088（p95 0.11）、大圖 0.167（p95 0.22）。餘裕 3.5～12 倍，B7 不開。報告 `Docs/reviews/2026-10-21-flow-benchmark.md`。
- ctest `xt_perf_flow_big_large`／`_typical`（標籤 `perf-report`）只印 `--perf` 摘要，不斷言、不是閘門。
- **M37（2026-10-25，27 §27.4.2）**：預算收緊為典型 ≤ 0.2、大圖 ≤ 0.8 ms；新增分段 `flowDeliverMs`／`flowEvalMs`／`flowSinkMs` 與計數 `flowSignals`／`flowNodeEvals`／`flowSinkWrites`／`flowAllocs`（debug）；T-475 記新基線、T-478 記優化後數字。
- **實作記錄（T-475）**：基線（Release、`xt_perf_flow`、2000 幀）behavior 1 典型 0.088（p95 0.140）、大圖 0.170（p95 0.254）ms，每 tick 配置 299／347 次；fixture 本身是 behavior 0（傳播在輸入派送與 `setPort()` 內，不在 `flowMs`）；前五熱點 fanOut＋deliverToNode、format、math、setStyle 沉降、`syncValueList()`；報告 `Docs/reviews/2026-10-25-flow-perf-baseline.md`。
- **新基線（T-478，2026-10-25）**：Release behavior 1、`xt_perf_flow`、2000 幀三輪取中：典型 flowMs 0.022（p95 0.046）、flow＋綁定 0.031；大圖 0.043（p95 0.084）、flow＋綁定 0.052 ms；穩態每 tick 配置 0（含沉降寫入，只排除文字路徑 setStyle）。`flow_bench.py --assert`（版號單用，`--keep 1800` 排除暖機）以典型 ≤ 0.2、大圖 ≤ 0.8 斷言 flow＋綁定平均，`--runner` 另斷言量測幀 0 配置；ctest 仍只報告。報告 `Docs/reviews/2026-10-25-flow-perf-after.md`。

## 21.7 `xt_wgpu`（M34；10 §10.10.3）

`FrameStats::renderMs/effectsMs/textMs` 對 `xt_wgpu` 與 GL 同義（CPU 提交時間，不等 GPU）。T-441 把 `xtui-bench` 第五輪加 wgpu 欄，與 GL、SW 並列記在 `Docs/reviews/`；目標：challenge-menu 4x 下 `renderMs` ≤ GL 的 1.2 倍。

> 實作記錄（T-441）：第五輪（`Docs/reviews/2026-10-23-wgpu-bench.md`，`xtframe-view --headless --renderer sw|gl|wgpu --msaa 1|4 --perf-csv`，1280×720）challenge-menu 4x `renderMs` wgpu D3D12 0.87 ms 對 GL 0.56 ms＝1.56 倍，未達標；最大三段為 queue（finish＋submit，wgpu-core 錄製與驗證）0.77、writeBuffer 0.17、FrameBuilder 0.14 ms（probe 分段）；Vulkan 在該場景觸發 uniform 動態位移越界的驗證 panic，另開單。
