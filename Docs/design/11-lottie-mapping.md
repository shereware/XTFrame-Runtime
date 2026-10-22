# 11 · Lottie 子集與映射

Lottie（bodymovin）JSON 由 `xt_compile` 在**離線**解析為 Scene（02.7）+ Timeline（02.8）。本章定義支援範圍與每個欄位的映射。參考實作：ThorVG `lottie` 模組的 `LottieParser` / `LottieBuilder`；lottie-web 的語意為最終依據。

## 11.1 支援範圍

| 功能 | 第一版 | 備註 |
|---|---|---|
| 版本 `v` ≥ 5.5 | 是 | 更舊版警告後嘗試 |
| Shape 圖層（`ty: 4`） | 是 | |
| Precomp 圖層（`ty: 0`） | 是 | 展開為子 Group；`tm`（time remap）**不支援**，出現時警告並忽略 |
| Solid 圖層（`ty: 1`） | 是 | → 矩形 Path |
| Image 圖層（`ty: 2`） | 是 | 資產 `p`/`u`，或 `e:1` 的 base64 內嵌 |
| Null 圖層（`ty: 3`） | 是 | → Group |
| Text 圖層（`ty: 5`） | 否 | 警告並跳過 |
| 圖層 `parent` | 是 | 變換父子鏈 |
| 圖層 `ip/op`（進出點） | 是 | → PropId 13 `Visible` 的 Enum Track（Hold 緩動），見 11.5 |
| 圖層 `st`（起始時間） | 是 | 關鍵幀時間平移 |
| 圖層 `sr`（時間伸縮） | 是 | 關鍵幀時間縮放 |
| 圖層 `hd`（隱藏） | 是 | 跳過 |
| 圖層 `bm`（混合模式） | 是 | → `NodeCommon::blend` |
| Track matte `tt` 1–4 | 是 | → Mask 節點（Alpha/AlphaInv/Luma/LumaInv） |
| `masksProperties` 模式 a/s/i | 是 | → Mask 節點（Add/Subtract/Intersect）；`f`（lighten）、`d`（darken）、`n`（none）警告跳過；`inv` 支援 |
| Shape：`rc el sh st fl gf gs tr gr tm` | 是 | |
| Shape：`sr`（星形/多邊形） | 是 | 編譯期展開為路徑（動畫時逐關鍵幀展開） |
| Shape：`rd`（圓角） | 是 | 編譯期套用到 rc 與 sh（動畫時逐關鍵幀） |
| Shape：`rp`（repeater） | 是 | 編譯期複製 `c` 份 Group 並套 offset 變換；`c` 動畫不支援（取第一幀） |
| Shape：`mm`（merge paths） | 否 | 警告，當作不存在 |
| Shape：`pb`（pucker/bloat）、`zz`、`op`（offset path） | 否 | |
| Expressions（`x` 欄位） | 否 | 忽略，用靜態值/關鍵幀 |
| 漸層 `gf/gs` 含 alpha stops | 是 | |
| 虛線 `d` | 是 | |
| 3D 圖層（`ddd:1`） | 是 | `p/a` 第三分量（取負）→ TranslateZ/AnchorZ；`rx/ry/rz` → RotationX/RotationY/Rotation；`or` → Orientation（三者可同時存在，依 19.2 公式相乘）；`p` 有 z 時 Translate3（PropId 20）取代 Translate（11）。見 19.10 對本章的修訂條款 |
| 攝影機圖層（`ty:13`） | 是 | → Camera 節點：`pe` → zoom、`p` → 位置（z 取負，寫入節點自身 translate/translateZ）、`a` → target（z 取負，`useTarget = 1`）；無 `a`（單節點攝影機）→ `useTarget = 0`，用 `rx/ry/rz` 決定朝向；本身不繪製，`ip/op` 忽略；一個場景多台攝影機取 `ind` 最小者，其餘警告並忽略 |
| Markers | 是 | → Timeline markers |
| 圖層 `ef[]`（Gaussian Blur `ty:29`、Drop Shadow `ty:25`） | 是 | → ContentGroup 的 SCEN 效果（`effectOffset/effectCount`）；映射公式見 18-effects.md §18.9。其他 `ty` 警告「unsupported effect ty N」並忽略，見 11.8 |

## 11.2 時間換算

```
sec(frame) = (frame - ip_root) / fr
```

所有關鍵幀時間換算為秒並減去根的 `ip`，Timeline `duration = (op - ip) / fr`，`frameRate = fr`。圖層的 `st` 與 `sr`：`localFrame = (frame - st) / sr`；圖層內關鍵幀 `time = sec(st + kfFrame * sr)`。Precomp 內圖層再遞迴套用。

## 11.3 節點建構

一個 Lottie 圖層 → 節點結構：

```
[LayerGroup]  ← 圖層變換 ks（a, p, s, r, o, sk, sa），parent 鏈以巢狀 Group 表達（父圖層 → 子圖層 Group 放於父變換 Group 下，但**不**繼承父的 opacity/masks；因此父圖層拆成兩個 Group：
              TransformOnlyGroup（只含 ks 變換）與 ContentGroup（opacity + 內容），子圖層掛在 TransformOnlyGroup 下）
   ├─ [Mask 節點 × n]   ← masksProperties（若有）
   └─ [內容]            ← shape 群組樹 / 矩形 / 圖片 / precomp 子圖層
```

Track matte：被遮的圖層 A 與遮罩圖層 B（`tt` 指向 B，B 緊接於 A 之上或以 `tp` 指定）→ 在 A 的父 Group 下插入 `Mask(mode)` 節點包含 B 的整個子樹，位於 A 之前；B 自身不再繪製。

圖層順序：Lottie `layers[]` 陣列 index 0 在**最上**。本專案子節點先者在下，因此**逆序**插入。

### 11.3.1 變換映射

| Lottie | 目標 | 說明 |
|---|---|---|
| `a` | Anchor（Vec2） | |
| `p` | Translate（Vec2）；若 `s:true` 分離則 `p.x`→TranslateX、`p.y`→TranslateY | 空間切線 `ti/to` → `tanIn/tanOut` |
| `s` | Scale（Vec2） | 值 /100 |
| `r` | Rotation | |
| `o` | Opacity | /100 |
| `sk`, `sa` | SkewX = sk，SkewY = sa | 依 03.3 的 `Sk(skew)` 定義 |

### 11.3.2 Shape 映射

Shape 群組 `gr` 的 `it[]` 由後往前處理（Lottie 語意：後面的 fill/stroke 套用於前面的幾何）：

1. 收集 `it` 中的幾何項（`rc el sr sh`），每個 → 一個 Path 節點。
2. 收集樣式項（`fl gf st gs`）：每個幾何 × 每個樣式 → 一個 Path 節點（幾何相同，paint 不同）。順序：lottie-web 語意為 `it` 陣列中**越前的項目畫在越上**，故節點依陣列逆序插入。
3. `tm` 套用於同群組內所有幾何：寫入各 Path 節點的 trim 屬性。`m: 2`（individually）第一版不支援，以 `m: 1` 處理並警告。
4. `tr` 群組變換 → 該群組 Group 節點的變換。
5. `rd` 對 `rc` 直接算圓角矩形；對 `sh` 用 Lottie 的圓角演算法（每頂點以 `r` 為半徑切角）。
6. `rp` → 複製 `c` 次，第 i 份的變換 = `tr` 的 `so/eo` 內插不透明度 + `offset + i` 倍的 `p/s/r/a`。

幾何生成（皆為 Cubic 路徑，方向 `d` 決定順逆時針）：
- `rc`：圓角矩形，4 段圓弧以 kappa = 0.5522847498 近似。
- `el`：4 段貝茲橢圓。
- `sr`：`sy:1` 星形（`pt` 尖點、`ir/or` 內外半徑、`is/os` 圓度）、`sy:2` 多邊形；圓度依 lottie-web 公式。
- `sh`：`ks` 的 `v/i/o/c` 直接轉為 Move + Cubic（`i/o` 為相對切線，絕對點 = v + o、next.v + next.i）。

### 11.3.3 Paint 映射

| Lottie | 目標 |
|---|---|
| `fl.c` `fl.o` | FillColor（rgb，/1 若已 0..1，舊版 /255）、FillOpacity /100 |
| `fl.r` | FillRule（1 NonZero，2 EvenOdd） |
| `st.c .o .w .lc .lj .ml .d` | Stroke 對應；`d` 陣列中 `n:"o"` 為 offset，`"d"/"g"` 交錯為 dash 陣列 |
| `gf/gs` `.t`(1 線性 2 徑向) `.s .e .g` | Paint kind、p0、p1、stops；`g.k` 的前 `g.p`×4 個值為 (offset, r, g, b)，其後每 2 個為 (offset, alpha)；alpha stops 與色 stops 以 offset 合併（線性內插缺的一方） |
| `gf.h .a` | highlightLength / Angle（讀入，後端忽略） |

## 11.4 動畫屬性 → Track

Lottie 屬性物件 `{a, k, ix}`：
- `a: 0` → 靜態，寫入 SceneNode/PathData 初始值，不產生 Track。
- `a: 1` → `k` 為關鍵幀陣列，每個 `{t, s, e?, i, o, h, ti, to}`：
  - `time = sec(t)`（含圖層 st/sr）；`value = s`（舊格式 `e` 為下一幀值，忽略 `e` 改用下一幀的 `s`；最後一幀無 `s` 時用前一幀 `e`）。
  - `h: 1` → Easing Hold；否則 CubicBezier(`o.x[0]`, `o.y[0]`, `i.x[0]`, `i.y[0]`)。多維 `i/o` 陣列只取分量 0（與 ThorVG 一致）。
  - `ti/to` 存在且屬性為 Vec2 → `hasSpatial=1`，`tanOut = to`，`tanIn = ti`。
- 路徑關鍵幀（`sh.ks`）：每幀 `s[0]` 為路徑物件；編譯器驗證所有幀頂點數與 `c` 相同，否則對該 Track 警告並取第一幀為靜態。
- 漸層 stops 關鍵幀（`g.k`）：Stops 型別 Track，stop 數固定為 `g.p`。
- 產生的 Track 寫入 `Timeline::tracks`，`target` 為場景局部 NodeId。

Path 型別的 Value 在 Keyframe 中指向 Timeline `floatPool`；verbs 不隨關鍵幀存（節點自身的 verbs 即模板）。

## 11.5 圖層可見性

`ip/op` → 若圖層不是全程可見，產生 PropId 13 `Visible`（Enum）的 Track。Keyframes：`(sec(ip), 1, Hold)`、`(sec(op), 0, Hold)`；若 `ip > root.ip` 則前置 `(0, 0, Hold)`。

## 11.6 資產

- 圖片：`assets[]` 中 `p/u` → `ASST` 條目，`embedded=1` 若 `e:1`（base64 解碼），否則 `name = u + p`，由宿主解析。
- Precomp 資產（`layers` 陣列）：內聯展開，不去重（同一 precomp 用兩次 → 兩份節點）。

## 11.7 驗證

編譯器對每個 Lottie 檔輸出 `warnings[]`；`--strict` 時任何警告即失敗。測試以 lottie-web 在固定幀渲染的 PNG 為黃金圖（15.3），第一版收錄 20 個涵蓋上表功能的樣本檔（放 `Tests/lottie/`，需 MIT 或 CC0 授權）。

## 11.8 圖層效果 `ef[]`（18-effects.md §18.9，T-55）

圖層 `ef[]` 每項是一個效果群組，自身有 `ty`，其 `ef[]` 為子參數（各自 `{ty, nm, v}`，`v` 為一般 Lottie 屬性物件 `{a, k}`）：

- `ty:29`（Gaussian Blur）：`ef[0].v` Blurriness、`ef[1].v` Blur Dimensions（1 both / 2 horizontal / 3 vertical，不視為可動畫）→ `Blur(radiusX, radiusY)`，`radius = blurriness / 2`；`dimensions` 非 1 時另一軸固定 0。`blurriness` 為關鍵幀 → `effectParam(i, 1)`/`effectParam(i, 2)` 各一條 Float Track（值除 2），只產生 `dimensions` 選中的軸。
- `ty:25`（Drop Shadow）：`ef[0].v` Shadow Color、`ef[1].v` Opacity（0–255）、`ef[2].v` Direction（度，0° 朝上、順時針）、`ef[3].v` Distance、`ef[4].v` Softness → `DropShadow(offsetX = dist·sin(dir), offsetY = −dist·cos(dir), blur = softness, spread = 0, opacity = op/255, color)`。softness/opacity/color 各自為關鍵幀時，分別產生 `effectParam(i,3)`/`effectParam(i,5)`（opacity 值除 255）/`effectParam(i,16)` 的 Track；distance/direction 任一為關鍵幀時，兩者共用一條 `effectParam(i,18)`（合成 Vec2 offset）Track，取樣點為兩者關鍵幀時間的聯集，逐點以 18.3 公式重新計算 offset，線性內插（不套用來源的 Bezier/Hold 緩動）。
- 其他 `ty`（Tint、Fill、Tritone、Stroke…）→ 警告「unsupported effect ty N」並忽略；`i`（`effectIndex`，`effectParam` 的第一個參數）只計入實際產生的效果，依 `ef[]` 中被支援的項目順序連續編號（被忽略的項目不佔位）。
- 效果掛在該圖層的 ContentGroup（與 opacity/blend 同一節點），不是 TransformGroup：`PushEffect` 因而包住整個已合成的圖層內容，而非只有其自身變換。

## 11.9 原生化匯入（2026-09-27 定案；對應 28.4）

匯入 Lottie 的預設路徑改為**原生化**：`xtframe-import lottie <json> -o <ui>/` 產生符號（每個 precomp 一個 `<symbol>`，主合成為根範圍）、向量群組圖層（12 章 SVG 子集）、一條 `@timeline main`（時長 `(op-ip)/fr` 秒，標記取 Lottie markers），關鍵幀寫入 `<ui>.xtanim.json`。`<anim>` 黑盒仍可用（`--blackbox`），且轉不了的圖層以 `<anim src segment>` 保留並在圖層上標示。

| Lottie | 原生 |
|---|---|
| shape layer：rect／ellipse／polystar／path | 向量群組；polystar 展開為 path |
| fill／stroke／gradient（含 stop 動畫） | paint；stop 數相同才動畫 |
| transform（a/p/s/r/o）、skew | `transform-origin` + translate/scale/rotate/skew、`opacity` |
| trim paths | `path-trim` |
| masks（add/subtract/intersect）、alpha/inverted matte | 18 章 mask 效果 |
| repeater | 展開為 N 個副本（烘焙） |
| precomp、time remap | 符號 + `clip-start/clip-rate`；非線性 remap 烘成關鍵幀 |
| text layer | 文字圖層（基本屬性） |
| expressions | 以 `fr` 取樣烘成關鍵幀 |
| 3D layer／camera | 19 章 3D 群組與 `<camera>` |
| 其他（merge paths、個別 trim、不支援效果） | 保留黑盒片段，圖層標示「未原生化」，`--strict` 為錯誤 |

**`.xtanim.json`**：`{ "v": 1, "timelines": [ { "name", "duration", "markers": [[t, name]], "tracks": [ { "layer": "<path id>", "prop": "<css name>", "keys": [[t, value, easing]] } ] } ] }`；`.xts` 以 `@timeline main from "<ui>.xtanim.json#main"` 引用；編譯器合併為一般 `@timeline`。編輯器把它當普通時間軸；AI 只拿摘要（軌道數、時長、標記）。反向匯出 Lottie 不保證。

### 11.9.1 T-190 實作記錄（#189）

- CLI／函式庫：`xtframe-import lottie <json> -o <ui> [--blackbox] [--fps n] [--strict]` → `xt::import::importLottie`（`Code/Editor/Import/include/xt/import/lottie.h`）；輸出 `<name>.xt/.xts/.xtanim.json`（`@timeline main from …`）、`import-report.json`；precomp → `shared/symbols/`（UI Project 內）或 `<ui>/symbols/`；寫出前編譯一次；`--strict` 遇黑盒 → `kUnsupported`（exit 1）。編輯器「匯入 Lottie」複製到 `shared/anim` 後原生化到 `ui/<id>/`（尚未登記進 `<name>.xtui`）。
- 覆蓋：shape（rect、ellipse、star、path、fill、stroke、漸層、dash、trim、圓角、repeater）以既有 shape builder 序列化為內嵌 `<svg>`（幾何與 `<anim>` 相同）；solid → svg path；image → `<img>`（內嵌解碼、外部複製）；null → 純變換包裝；text → 基本 `<span>`；transform 與 parenting；in/out point → opacity hold 鍵（opacity 也動畫時加 `_V` 包裝）；空間運動路徑依 `--fps` 烘焙；precomp → 符號 + `<use sync>` + `clip-start/clip-rate`（時長 = 最大 layer op，群組時間夾住）；blur（ty 29）與 drop shadow（ty 25）→ 原生 `effect` + `effect.<i>.<param>` 軌。**黑盒保留**：masks、track mattes、blend mode、time remap、merge/pucker/zigzag/offset path、個別 trim、其他效果；含 3D layer 或 camera 的檔整份黑盒；表達式不求值（依關鍵幀、標 medium）；動畫漸層點／stop、dash array、miter 的圖層退回黑盒。hold 鍵寫 `steps(1, end)`；無 autoplay 觸發（宿主 `playTimeline("", "main")`）。
- 驗證：`xt_lottie_tests`（`dev_check` 新 `lottie` 套件）對 38 個 fixture 原生與黑盒各於 25/50/75% 三時刻比對，容差 15 §15.3（≤ 2/255、≤ 0.5% 像素）：114 幀全過，最差 `trim` t=0.5 差 4 像素、最大 14/255（trim 端點 AA）；黃金圖 `ui_lottie_native_precomp/trim`。

> T-228（#234）：圖層 `ao`（auto-orient）→ `IrSceneNode::autoOrient`／`.xts` `auto-orient: path`。T-230 將把 shape modifiers、masks／mattes 改為原生元素（31 §31.9）。

### 11.9 T-230（#236）原生化映射更新

| Lottie | 原生輸出 |
|---|---|
| rc／el／sr | `<rect>`／`<ellipse>`／`<xt-star>`（參數動畫）；被 trim／zigzag 作用、虛線描邊、反向、圓度星形 → `<path>` |
| tm m=1／m=2 | `<xt-trim mode="parallel"/"serial">` |
| rp | `<xt-repeat>`（`copies` 動畫保留；接在巢狀群組之後或非等比縮放 → 仍編譯期展開並警告） |
| rd／pb／zz／tw | `<xt-round>`／`<xt-pucker>`／`<xt-zigzag>`／`<xt-twist>` |
| op／mm | `<xt-offset>`／`<xt-merge>`（未烘焙，編輯器存檔或 `xtframe-editor --bake-vectors` 烘焙） |
| masksProperties a／s／i／inv | 巢狀 `<clipPath>`（Subtract／inv 以外框 even-odd） |
| tt 1–4 | `<mask>`（alpha／luma、反向以外框） |
| ao | `auto-orient: path` |
| bm | `<g blend>` |
| 多 paint | 描邊緊接填色合成；其餘複製形狀 `_2` |

黑盒：表達式、time remap、text animator、不支援效果、3D／camera、Subtract／Intersect 後再 Add 的遮罩組合、非向量層的 mask／matte／blend；`import-report.json` `blackboxCategories`。`<anim>` 黑盒 builder 對 rp／rd／pb／zz／tw 亦產 Modifier 節點、tm m=2 為 serial。

## 11.10 Lottie 輸入上限與路徑限制（2026-10-02 審查修正；T-257 #267、T-258b #272、T-261a #275）

| 項目 | 上限 | 超限時 |
|---|---|---|
| shape group 巢狀（編譯器） | 64 | error，匯入失敗 |
| parent 鏈（編譯器 `lottie_build.cpp`） | 256，並偵測循環 | error |
| parent 鏈（原生匯入 `lottie_native.cpp`） | 128，並偵測循環 | note，其餘掛到根 |
| precomp 展開 | 100000 個圖層實例 | error（訊息含 `expansion exceeds`） |
| repeater | 單一 `copies` 夾到 1024；總份數 100000 | 夾限／error |
| star `pt` | 1024 | 夾限＋warning |

圖片資產 `u` + `p`：必須是相對路徑、不含 `..`、正規化後仍在根目錄內（編譯器：`rootDir`；原生匯入：Lottie 檔所在目錄，寫出時為輸出目錄）。不符合者不讀不寫、略過並發 warning，檔案其餘部分照常匯入。這條只管 Lottie 檔內寫的路徑；`.xt`／`.xts` 內手寫的絕對路徑仍然允許（T-273 處理發佈時打包）。
