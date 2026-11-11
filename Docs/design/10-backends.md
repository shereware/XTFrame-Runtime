# 10 · 參考後端：SW 與 OpenGL 3.3

兩個後端都只消費 09 的 `CommandList`，不 include 場景圖。

## 10.1 共用幾何工具（`xt_core/geom`）

因兩個後端與引擎整合者都需要，以下放在 `xt_core` 內：

```cpp
// 路徑扁平化：三次貝茲 → 線段。tolerance 為最大弦誤差（px，於 xf 套用後）
void flattenPath(std::span<const PathVerb>, std::span<const float>, const Mat2x3& xf,
                 float tolerance, std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts);
// 描邊 → 填充輪廓（NonZero）。實作：每段線段生成四邊形 + join + cap，輸出多個閉合輪廓
void strokeToFill(std::span<const Vec2> flat, std::span<const uint32_t> contours, bool closed,
                  float width, LineCap, LineJoin, float miterLimit,
                  std::vector<Vec2>& out, std::vector<uint32_t>& outContours);
// 虛線：先切分扁平化線段
void applyDash(std::span<const Vec2>, std::span<const uint32_t>, std::span<const float> dashes, float phase,
               std::vector<Vec2>& out, std::vector<uint32_t>& outContours, std::vector<uint8_t>& outClosed);
// 貝茲細分容差：固定 0.25 px
```

扁平化細分數（Wang 公式）：`n = ceil(sqrt(0.75 · L / tolerance))`，其中 `L = max(|P0 − 2P1 + P2|, |P1 − 2P2 + P3|)`（控制多邊形的最大二階差分長度），夾在 [1, 64]。半徑 50 的四分之一圓在 0.25 px 容差下約 9 段。

## 10.2 SW 光柵器（`xt_sw`）

```cpp
class SwRenderer {
public:
    void setTarget(uint8_t* rgba, int width, int height, int strideBytes);   // 非預乘 RGBA8，row-major，y 向下
    void render(const CommandList&, TextShaper*, AssetResolver*, const IrView* ir = nullptr);   // ir 供 AssetResolver 解碼內嵌資產；為 null 時 DrawImage 被跳過
};
```

### 10.2.1 掃描線覆蓋率演算法

參考 ThorVG `sw_engine` 與 stb_truetype v2 的做法，實作**有號面積累積（signed-area accumulation）**光柵器：

1. 對每個填充命令：扁平化 → 線段清單（已套 xf 與 DPR）。
2. 配置 `float coverage[w+1]` 累積緩衝與 `float area[w+1]`（每列重用）。
3. 對每條線段，走過其覆蓋的每個掃描列，計算與該列每個像素格的相交面積與方向，累加到 `cell(x).cover` 與 `cell(x).area`（與 FreeType `ftgrays` 相同的 cell 模型）。
4. 每列從左到右累積 `winding`：NonZero 取 `min(1, |acc|)`；EvenOdd 取 `acc mod 2` 的三角波。
5. 覆蓋率 × paint 顏色（漸層每像素求值）→ 以 `src-over` 混合到目標（先預乘再混合，寫回時反預乘；alpha 為 0 時寫 0）。

**守恆要求**：閉合輪廓每一列的 `cover` 總和必為 0。實作上每個切分點的 24.8 定點 y 只轉換一次（端點直接由頂點座標取整，相鄰邊共用同一值），cell 的 `cover` 為相鄰切分點的整數差；不得對每個 cell 各自四捨五入（T-11b 的殘留覆蓋即由此而來）。測試以 `debugRowCoverSum` 驗證。

Cell 儲存採**稀疏列**：每列一個 `std::vector<Cell>` 在首次觸碰時 append，列末排序合併（與 ftgrays 一致），避免對整個視口配置。

### 10.2.2 描邊

`StrokeCmd` → `applyDash`（若有） → `strokeToFill` → 走填充路徑（NonZero）。

### 10.2.3 Clip

Clip 堆疊維護一張 `uint8_t clipMask[w*h]`（惰性建立，全 255 表示無 clip）。`PushClipRect/Path` 時把新形狀光柵成臨時 mask 並與當前相乘；`PopClip` 還原上一層（堆疊保存 mask 副本，最多 8 層，超過則 `XT_ASSERT`）。填充寫入時覆蓋率再乘 clipMask。T-133 起每層 mask 附帶裝置像素包圍盒 `clipBounds`（形狀 AABB 與上一層包圍盒的交集）：光柵化與相乘只處理包圍盒內的列、盒外視為 0 不逐位元組寫入；緩衝自池取用；填充前先以 `clipBounds` 裁掉路徑包圍盒（盒外的 span 不進光柵器）。

### 10.2.4 Layer 與 Mask

- `PushLayer`：配置與 `boundsHint`（夾到視口、對齊裝置整數像素）同大的 RGBA float 離屏緩衝，目標切換；緩衝帶裝置座標原點（同 `PushEffect` 的有界圖層，T-53b），繪製命令仍用全視口座標。`PopLayer`：以 `opacity` 與 `blend` 合成回上一層。混合模式公式依 W3C Compositing Level 1 的 separable/non-separable 定義，實作於 `blend.cpp`，全部 17 種。
- `PushMask`：配置兩個離屏緩衝（mask、content）。`EndMaskContent` 切到 content。`PopMask`：依 `mode` 算出 `m`（Alpha：mask.a；Luma：`0.2126R+0.7152G+0.0722B` × a；Inverted 取 `1-m`），`content.rgba *= m` 後 src-over 回上一層。`Add/Subtract/Intersect` 由 Painter 已展開為巢狀，這裡 Subtract 視為 AlphaInverted，Intersect 視為 Alpha。
- 離屏緩衝以池重用。

### 10.2.5 圖片

`DrawImage`：`AssetResolver::decodeImage(asset)` 取得 RGBA8；反向映射每個目標像素到來源座標，`smooth` 時雙線性取樣，否則最近鄰。目標矩形套 xf 後可能旋轉：以 `dst` 四角做四邊形填充，每像素反算 uv。

T-149（2026-09-24）：`DrawImage`/`DrawImageQuad` 的取樣分三路——(a) 軸對齊（`xf.b == c == 0`，含負縮放）：每列來源 v 固定、u 為 32.32 定點增量，雙線性 = 兩來源列預混 + 每像素一次水平 lerp，強縮小（觸及欄數 > 2× 目的像素）改每像素四點；(b) 投影四邊形若四角仍為軸對齊矩形（容差 1e-3 px）轉成等價 `ImageCmd` 走 (a)；(c) 一般（旋轉/剪切/投影）逐像素反算、四點 lerp 用 `simd::f32x4`。三路都先取樣成 premultiplied float 暫存列，再依 clip 切 run 交給 T-102 的 span kernel（全不透明 run 直接寫）。challenge-menu 1080p `sw:image` 55 → 9 ms。未做 mip（畫板皆為放大）。

### 10.2.6 文字

`DrawGlyphs`：對每個 glyph 取 `glyphOutline`，縮放 `size`、平移到 `glyphPos`，走填充路徑。外框快取由 FontProvider 負責。

0.3（T-101）：`rendering == Auto`、裝置字級 < 24 px、無旋轉/傾斜且不在 3D 投影內時改走**點陣路徑**：`glyphBitmap(font, id, size, subpixelX)`（FreeType light hinting、水平 1/4 px 子像素位置四個變體），以覆蓋率貼圖混合，混合前覆蓋率套 gamma（文字比目的像素亮 → `pow(c, 1/1.2)`，否則 `pow(c, 1.2)`）。其餘情況與 `text-rendering: geometricPrecision` 走上面的外框路徑。GL 的 atlas 用同一組載入旗標與子像素相位，gamma 以整條 run 依文字色決定。

### 10.2.7 精度目標

與黃金圖比對容差：每通道 ≤ 2/255，且差異像素比例 ≤ 0.5%（15.3）。

## 10.3 OpenGL 3.3 後端（`xt_gl`）

```cpp
class GlRenderer {
public:
    bool init(GlLoader loadProc);        // loadProc(const char* name) → void*；xt_gl 自帶 gl_loader.h 宣告所需的 GL 3.3 函式指標，不用 glad
    void resize(int fbWidth, int fbHeight);
    void render(const CommandList&, FontProvider*, AssetResolver*);   // 畫到目前綁定的 FBO
    void saveState(); void restoreState();  // 見 10.3.5
};
```

### 10.3.1 填充：Stencil-then-Cover

參考 NanoVG 與 ThorVG GL 後端：

1. 扁平化路徑，以扇形三角（每個輪廓從第一點出發）寫入頂點緩衝。
2. Pass 1：關閉顏色寫入，stencil 依 NonZero（`GL_INCR_WRAP` 正面 / `GL_DECR_WRAP` 反面）或 EvenOdd（`GL_INVERT`）累積。
3. Pass 2：畫路徑 AABB 四邊形，stencil test `!= 0`（NonZero）或 `& 1`（EvenOdd），著色器計算 paint 顏色，之後清 stencil。
4. 抗鋸齒：**採用 MSAA**（宿主 FBO 建議 4x；`init` 讀取 `GL_SAMPLES`）；不做 NanoVG 的 fringe 抗鋸齒。無 MSAA 時鋸齒為已知限制。

### 10.3.2 描邊

CPU 端 `strokeToFill` 後走 10.3.1，避免 GPU 描邊的 join 問題。

### 10.3.3 Clip / Layer / Mask

- Clip：走 stencil 高位元。stencil 8 位元：低 4 位給 cover 計數（NonZero 只需 ±7；超過視為已知限制），高 4 位當 clip 深度計數（最多 15 層）。`PushClip` 時把形狀區域的高位元 +1，繪製時 test 高位元 == 當前深度。
- Layer：FBO 池（每個含 RGBA8 紋理 + 共用深度模板 RBO；宿主 FBO 有多重取樣時，池內 FBO 用相同取樣數，`PopLayer`/`PopEffect` 先 `glBlitFramebuffer` resolve 成單一取樣紋理再合成，T-80）。`PopLayer` 畫全螢幕四邊形，著色器實作 17 種混合（讀取下層需 `glBlitFramebuffer` 複製當前目標到臨時紋理，因無 `GL_KHR_blend_equation_advanced` 保證）。
- Mask：兩個 FBO，`PopMask` 用著色器合成。

### 10.3.4 紋理與字形

- 圖片：首次使用時上傳，`AssetId → GLuint` 快取；`AssetResolver` 通知失效時刪除。
- 字形：`xt_text::GlyphAtlas`，每幀 `flushUploads` 以 `glTexSubImage2D` 上傳；`DrawGlyphs` 批次為一個 VBO，著色器取 R8 覆蓋 × 顏色。若 FontProvider 不提供點陣，退回外框填充。

### 10.3.5 狀態管理

`render` 開頭 `saveState()` 記錄：目前 program、VAO、array/element buffer、texture unit 0 綁定、blend 開關與函式、stencil 開關與函式/mask、scissor、viewport、depth test、cull face、FBO。結尾 `restoreState()` 全部還原。不使用 `glGet*` 於命令迴圈中。

### 10.3.6 批次

同 paint 型別、同 blend 的連續 FillPath 合併為單一 stencil pass 不可行（stencil 會互相影響），因此第一版**不批次填充**，只批次 `DrawGlyphs` 與 `DrawImage`。效能目標：1000 個簡單路徑 60 fps 於整合顯示晶片。

T-160：`FileAssetResolver::imageSize` 只讀檔頭（PNG/JPEG/GIF/BMP）並依 AssetId 快取；已解碼者以解碼結果為準。

## 10.5 裝置無關 GPU 層 `xt_gpu`（0.5，T-146 #151 / T-147 #152）

`CommandList` → `xt::gpu::FrameBuilder::build()` → `GpuFrame` → 宿主 `GpuDevice::submit()`。**規範文本是標頭本身**：`Code/Runtime/gpu/include/xt/gpu/{gpu_frame.h, gpu_device.h, frame_builder.h}`（每個列舉值與 `*Params` 結構的語意都寫在註解裡）。摘要：

- `Vertex{x,y,w,u,v,rgba}` 24 bytes：目標像素、左上原點；`w` 齊次分量（19 §19.7）；顏色 premultiplied、位元組序 r,g,b,a。**整條管線 premultiplied**（貼圖、transient、shader 輸出），`Blend::SrcOver = (One, OneMinusSrcAlpha)`。
- 索引 32-bit、相對 `baseVertex`，每個 `DrawCmd` 的頂點/索引範圍連續（裝置可逐命令上傳）。
- `Shading`：Solid、Gradient（256×1 LUT 貼圖 + `GradientParams`，stops 內容雜湊去重）、Texture、Glyph（R8 atlas + gamma）、StencilOnly、Composite（W3C 17 種混合，Normal 走硬體 SrcOver、其餘讀 `aux` backdrop 副本）、Effect（`EffectProgram` 12 個：GaussScalar/GaussColor/Dilate/AlphaExtract/Sdf/ColorMatrix/GradientMask/Shine/ShadowGlow/Outline/Backdrop/MaskApply；語意 = `gl_shaders.h` T-54 區塊）。
- Stencil 8 位：高 4 位 clip 深度、低 4 位 winding；`StencilOp`：None、WriteIncrWrap（NonZero，以高 4 位 == 深度閘控）、WriteInvert（EvenOdd）、TestNotEqualZero（cover）、TestEqualRef、ClearRect、ClipPush（REPLACE 一次完成升深度+清低位）、ClipPop。FillPath = 3 個命令；路徑 clip push = 2 個。軸對齊矩形 clip 只用 scissor（像素中心規則 `[ceil(lo-0.5), ceil(hi-0.5))`）；開新圖層目標時「重放」現有 clip 層而非複製 stencil。
- `Pass{target, firstCmd, cmdCount, clear, copy, copySource, viewport}`；transient 目標按幀在 `transients[]` 描述（`TargetHandle i>0 = transients[i-1]`，Rgba8/R32F、needStencil），builder 幀內重用槽位，裝置於 `submit` 配置。
- `GpuDevice`：`createTexture/updateTexture/destroyTexture/submit` 四個純虛函式；貼圖 row 0 = 頂列；全螢幕命令的 uv 左上 (0,0)。
- `FrameBuilder`：`init(device)/shutdown/resize/setGlyphAtlas/build/invalidateAsset/lastError`；`PerfTracker` 新分項 `gpuBuildMs`。遮罩改為 `MaskApply` pass + Normal composite；字形 atlas 幀內 reset 改為整幀重建（最多兩次）；clip 深度 > 15 推 no-op 層保持平衡並記 `lastError`。
- 結構快照測試：`Tests/gpu/golden/*.txt`（37 個）。
- **視窗化暫時目標（T-151 #155）**：`Pass::viewX/Y/W/H` = 該 pass 的視窗（表面像素）：ortho 恰涵蓋視窗、transient 尺寸 = 視窗（向上取 32 倍數、最小 64×64、不超出表面）、頂點與 `clip*` 仍為表面座標（裝置把 scissor 與視窗交集後平移）、0 = 整張、target-0 恆整張；同一 transient 編號在同一幀可於不同視窗重用（以最後寫入的 pass 為準）；copy pass 複製兩視窗交集。視窗 = `boundsHint`×dpr ∩ 目前 scissor ∩ 父視窗；BackdropBlur 外擴 3σ。無效 pass 剔除：spread=0 的 Dilate 換 AlphaExtract、σ=0 跳 Gauss、恆等 stack 直接合成。challenge-menu 1920×750 全畫布填充 137.9 → 4.4 Mpx/幀；`Code/Tools/gpu-probe` 量測。大 σ 降解析度未做（16.3）。
- **凸路徑直繪、合批與 fringe AA（T-155 #158）**：單一凸輪廓（去重後 3～512 頂點、轉向同號、x/y 各最多變向兩次）、互不相碰的多個凸輪廓、凸環（內外輪廓，EvenOdd 或反向 NonZero）直接以 Solid/Gradient 扇形畫，不走 stencil；連續同狀態命令（shading/blend/stencil/texture/sampler/program/paramIndex/src/aux/scissor 全等且幾何連續）合成一個 `DrawCmd`；`ClearRect` 延後合併（待清矩形與下一個 winding AABB 相交、scissor/pass 變更或幀末時一次清）；軸對齊的 `PushClipPath` 與投影後仍軸對齊的 `PushClipRect` 走 scissor。**Fringe AA**（`FrameBuilder::setEdgeFringe`，預設開；GL 在 `GL_SAMPLES > 1` 時關）：核心內縮 0.5 px、外圈外擴 0.5 px alpha 0，厚度 < 1 px 收成軟四邊形；`Gradient` 語意改為乘頂點 alpha（裝置 shader 必須跟進）。數字：challenge-menu draw 159 → 87、hud 119 → 59、pause 65 → 32；GL-vs-SW 差異像素 4367 → 1461。§10.3.6「第一版不批次填充」自此作廢。
- **GL 裝置狀態快取與增量上傳（T-159 #161）**：`gl_state_cache.{h,cpp}` 影子 program/FBO/VAO/viewport/scissor/blend/stencil/color mask/texture unit 與每 program 的 uniform 影子（submit 開頭 reset）；pass 只綁 draw FBO、`FboPool` 重用不重綁；resolve blit 關 scissor 執行；VBO/EBO 三組輪替 + CPU 影子逐元素比對只上傳變動區段（challenge-menu 51 KB → 0.8 KB/幀）；`GlRenderer::frameStats()` 回 `GlFrameStats`（draws、binds、uniform sets、resolves、blits、clears、passes、上傳量）；`xtframe-gpu-probe --gl [--msaa N]` 走真正的 GlRenderer 量 render/glFinish。bench：fringe CPU 0.90 → 0.73、GPU 0.54 → 0.49；剩餘 CPU 大半在 paint（core）與 4x 的 22 次 resolve。MSAA resolve 數已是 builder 結構的最小；再減需 builder 提示「只被 blur 讀的 layer 不必 MSAA」（16.3）。
- **MSAA resolve 改 sampler2DMS draw、FboPool 閒置淘汰（T-161 #163）**：4x 雙峰的真因是 NVIDIA 驅動 threaded optimization 逐行程開關，而 22 次 `glBlitFramebuffer` resolve 每次在驅動工作執行緒與 GPU 各 ~30 µs 使幀時間被工作執行緒綁住；MSAA transient 改 `GL_TEXTURE_2D_MULTISAMPLE`，`textureOf` 以 `sampler2DMS` 全螢幕三角形 resolve（`kResolveVert/Frag`），blit 只剩 copy pass；4x 交錯 GPU 1.19 → 0.59 ms、CPU 1.55 → ~1.0。+70 MB 是 T-160 讓視窗每幀變化後 `FboPool` 精確尺寸池無限成長：加 `endFrame(idleFrames)` 閒置 > 120 幀刪除（`transientCreates` 統計）。§10.3.3「`glBlitFramebuffer` resolve」自此作廢。增量上傳與 FBO 快取以 fence 量測排除嫌疑。
- **靜態幀重用與效果結果快取（T-162 #164）**：(1) `FrameBuilder::lastFrameReusable(assets, ir)`——上次 build 後無 resize/shutdown/invalidateAsset/setGlyphAtlas/setEdgeFringe、atlas 無待上傳（`GlyphAtlas::hasPendingUploads`）、`engine://` 綁定未變、且無「待快取」區段時為 true；宿主在 `DamageList` 非 full 且 rects 空時跳過 build 直接 `submit` 舊 frame（`GlRenderer::render(..., const DamageList*)` 尾端參數；`xt::shell::App` 已走）。(2) 效果結果快取：每個 `PushEffect..PopEffect` 區段以 FNV-1a 64 雜湊（dpr、表面、fringe、父視窗、scissor、clip 層、投影 mapping、區段內命令——重用 core 的 `hashCommandList`）；**連續第二次**出現才存（動畫不白存）、第三次起命中：整段換成一個 `Shading::Texture` 視窗 quad 取樣快取貼圖。不快取：BackdropBlur 開頭、含 `engine://`、恆等 stack。一幀未命中即釋放；上限 64 MB / 256 項；`resize`/`invalidateAsset`/`shutdown` 全清。`GpuFrame::cacheStores[]{pass, src, texture}`（尾端新增）；`GpuDevice::acquireCachedTarget(w,h,fmt)`/`releaseCachedTarget`（尾端、預設回 0 = 不支援）。GL：cached target = RGBA8 貼圖 + 單取樣 FBO，store 以上下翻轉 blit（多取樣先 resolve）。數字（bench 4x）：靜止 CPU 0.90 → 0.28（有 damage 0.18）、GPU 0.60 → 0.04；全動畫 0.95 → 0.42 / 0.59 → 0.17；靜止已為 RmlUi 的 0.4–0.6×。
- **尾端尖峰與池預算（T-163 #165）**：動畫場景 p99 尖峰的真因是 T-161 的 `FboPool` 120 幀閒置淘汰——循環動畫每 216 幀才重訪同一組視窗尺寸，整組被 `glDelete*`，驅動工作執行緒執行刪除，下一幀 `saveState()` 的 `glGet*` 等它做完（1.2～4.7 ms）。改為**位元組預算池**（`setBudget` = max(64 MB, 2×整表面目標)，只在超額時 LRU 刪；閒置安全網 1800 幀、每幀最多刪 1 個）、只有 `needStencil` 的 transient 才配 stencil、cached target 釋放後按尺寸 free list 重用；`GlFrameStats` 加分段計時（build/upload/target/draw/endFrame/state）與物件變動計數；probe 印 p99/max 與最差幀。bench：steady p99 1.85 → 0.72、interact 2.10 → 1.07、4x 記憶體 363 → 306 MB。`glGet*` 每幀 ~0.19 ms 是 §10.3.5 的隱含同步，維持。`kStencilOnlyRenderbuffers`（S8）可再省 30 MB 但 NVIDIA GPU +9–15%，預設關。
- **旋轉圓環被填成實心（T-165 #166）**：`fillRing`（T-155 直接環填）以內心為原點按角度合併走訪內外輪廓；內外圓角矩形從同一角落起畫，某個內頂點角度與外頂點 0 只差浮點誤差，落在 `ao[0]` 之下時起點搜尋以 `-2π` 正規化、展開迴圈卻只加 `2π`，整串內角領先一圈，於是整個外多邊形從單一內頂點扇出成實心圓；每 360° 約 8% 的角度會中，動畫轉到就閃（與效果快取無關，SW 無此路徑）。修：內角一次正規化到 `[ao[0], ao[0]+2π)` 再取 argmin。`Tests/gpu`/`Tests/gl` 各加 360° 掃描回歸；`xtframe-view` 加 `--warmup N`、`--warmup-dt`、`--screenshot-every N` 抓即時迴圈幀。
- **外部貼圖（T-150 #153）**：`Shading::TextureStraight = 7`（straight-alpha 貼圖：`(s.rgb*s.a, s.a)` × 頂點色）。`FrameBuilder` 對 `engine://<name>` 資產改查 `AssetResolver::externalTexture(name)`，用宿主給的 `TextureHandle`（可能不是本裝置建立的，裝置必須能取樣任意有效 handle）；未綁定則不畫、不記 `lastError`。

`xt_gl`（T-147）：`GlRenderer` 公開介面不變，內部 = `FrameBuilder` + `GlDevice`（`gl_device.cpp`；transient 由 `FboPool`、MSAA 懶惰 resolve、`glDrawElementsBaseVertex`）；`gl_renderer.cpp` 1641 → ~130 行、`gl_text.*` 刪除。與舊 GL 的行為差異（premultiplied composite、256 階 LUT、scissor 矩形 clip）皆在黃金圖容差內；`FrameStats::effectsMs/textMs` 在 GL 端現為 0（併入 `renderMs`）。第二個裝置：CryEngine `RenderDll/XUI2D`（`CE_XTUI/docs/05-stage2-design.md`）。

## 10.4 AssetResolver

外部貼圖表（T-150）：`AssetResolver` 基底類別（非虛擬、含資料）提供 `setExternalTexture(name, texture, width, height, straightAlpha)`、`clearExternalTexture`、`externalTexture(name | AssetId+IrView)`；`kExternalAssetScheme = "engine://"`、`isExternalAssetName`、`externalAssetName`、`isExternalAsset`。放在 resolver 而非 `FrameBuilder` 的理由：排版在 `Context::update` 內經 `imageSize` 取內在尺寸，第一幀就要有。`FileAssetResolver` 對 `engine://` 短路不碰磁碟；SW 對外部資產畫透明；宿主在文件第一次 `update` 前綁定，之後改尺寸要等下一次排版（不自動重排）。`<anim src="engine://">` 不支援。

```cpp
class AssetResolver {
public:
    virtual ~AssetResolver() = default;
    virtual bool readBytes(std::string_view name, std::vector<uint8_t>& out) = 0;  // 非內嵌資產
    virtual bool decodeImage(AssetId, const IrView&, ImageData& out) = 0;          // RGBA8；預設實作用 stb_image
    virtual Vec2 imageSize(AssetId, const IrView&) = 0;
};
struct ImageData { int width, height; std::vector<uint8_t> rgba; };
class FileAssetResolver : public AssetResolver { /* 相對某根目錄讀檔 + stb_image */ };
```

- **文字效果（T-178 #177）**：`GlyphParams {gamma, effect, radius, rectX0, rectY0, rectX1, rectY1, texSize}`（尾端新增；快照參數數 1 → 8）；帶效果的字形獨立一個 DrawCmd、quad 依核半徑外擴；GL `kGlyphFrag` 以 `texelFetch` 精確讀 atlas、限制在該字形矩形內；裝置端半徑上限 8 px；SW 對 FreeType 點陣做相同運算；GL 與 SW 只算效果的差異 ≤ 2/255。

## 10.6 實作記錄（T-199 #197）：`imageView(dpr)`、DDS、圖集

- `AssetResolver`：`ImageMeta` 表（`setImageMeta`／`imageMeta`：切片、變體、圖集子矩形）、`imageView(asset, view, dpr)`（`dpr ≥ 1.5` 選 `@2x` 變體／圖集 2x 頁／`img@2x.png` 慣例；固有尺寸維持 1x）、`compressedImage()` 交出 level-0 BCn 區塊；`FileAssetResolver` 快取改為每檔共用項、1x/2x 槽、DDS 區塊；新 `Code/Runtime/core/include/xt/dds.h` 標頭解析。
- `GpuFrame`：`TextureFormat::Bc1=3, Bc3=4, Bc7=5` 尾端新增，**無 Shading 變更**（以既有 `Shading::TextureStraight` 取樣）；`FrameBuilder` 壓縮上傳、傳 dpr、每圖集頁共用一張貼圖；GL `glCompressedTexImage2D`，GL 錯誤時回 0 → RGBA8 後備；DDS 有 mip 鏈但只上傳 level 0。SW 以 rgbcx／bc7decomp 解回 RGBA（`sw_dds`）。

## 10.7 T-202（#202）：GPU 粒子

尾端新增：`Shading::ParticleTexture = 8`、`Shading::ParticleSolid = 9`、`Blend::Additive = 2`、`enum DrawKind {Triangles = 0, Particles = 1}`、`DrawCmd` 尾端 `kind`、`_pad2[3]`、`firstInstance`、`instanceCount`；`GpuFrame::particleInstances`（`gpu::ParticleInstance` 40 bytes）、`ParticleParams`（含 `straight` 供 straight-alpha 貼圖）。GL：一個 `DrawKind::Particles` = 一次 `glDrawArraysInstanced`，頂點由 `gl_VertexID` 產生、每實例屬性用 divisor、每次 submit 上傳一次 instance buffer；additive = `GL_ONE, GL_ONE`；loader 補 instanced 入口；`xtframe-gpu-probe` 同步。GL 與 SW 對同一 fixture 差異像素 1.8%（門檻 3%，皆為邊緣 AA）。

> T-215（#214）：`FrameBuilder::invalidateAllAssets()`（銷毀 owned 貼圖、清 textureCache、`flushEffectCache()`、`lastBuildValid=false`、asset epoch++），`GlRenderer::invalidateImages()` 轉呼叫；編輯器每次 IR serial 變化呼叫。effect cache 的全清條件包含它。

## 10.8 審查修正鏈補記（2026-10-07 審查修正鏈合併；T-263a～T-263g #282 #285 #289 #293 #297 #303 #311）

**裁剪**：`xt::kMaxClipDepth` = 15（`command_list.h`）是 GPU 的 stencil 裁剪上限，只計非軸對齊矩形的裁剪（路徑、圓角、旋轉／傾斜矩形）；軸對齊矩形是 scissor 層、不限深度。超過上限的 stencil 裁剪退化為其包圍盒 scissor 層（`lastError`：`clip depth limit (15) exceeded; clip reduced to its bounding box`），PopClip 仍配對。SW 無上限、無 assert。

**SW**：glyph 繪製（點陣與字形效果路徑）遵守 damage 遮罩；`setTarget()` 與 `render()` 開頭 `resetStacks()`（裁剪堆疊回 pool、layer 堆疊釋放所有 pooled surface、projection 對應、effect 幀）；layer pool 的 surface 不再殘留為使用中。SW 不觸碰舊的 `target.rgba`。

**文字**（§10.3.4）：atlas 字形的光柵尺寸 = `size × |xf.a| × dpr`（`glyphRunBitmapSize`），「走點陣路徑」的判斷 `glyphRunBitmapApplies` 與 SW 共用（鏡射 `a = d ≤ 0` 兩端都走外框）；24 px 上限仍 SW 獨有，故 24～96 px 之間 SW 畫外框、GPU 畫 atlas 點陣，兩端不逐像素相同。尺寸自上一次建置改變的字形序列先以外框繪製直到尺寸維持一次建置；光柵尺寸超過 96 裝置 px 一律外框，縮放中的文字不會塞滿 atlas。字形效果序列（文字陰影）只能從 atlas 畫：一條序列最多佔四分之一頁（查詢前以每個 (glyph, subpixel) 一個 em 方估算），超過則以外框、不帶效果繪製，`lastError`：`glyph effect run too large for the atlas; drawn without its effect`（每次建置一次）。

**GL 狀態**（§10.3.5）：save／restore 涵蓋 unit 0～2 的 2D 綁定與 sampler 物件（繪製期間綁 0）、兩面 stencil、alpha 混合方程式、unpack row length／skips／pixel unpack buffer；宿主貼圖的 filter 在其繪製後還原；`render` 存檔後將 unit 0 設為作用中；`updateTexture` 以 `GL_UNPACK_ROW_LENGTH` 一次上傳；`glBindSampler` 為必要的 loader 項目。GL 裝置讀一次 `GL_MAX_TEXTURE_SIZE`。

**GPU 貼圖快取**（§10.5）：鍵為 (resolver, 來源的 `IrView::loadSerial()`（表為空時為呼叫 IR 的）, AssetId, hiDpi)；保留 max(2, 本次建置用到的來源數) 個來源，本次清單的來源先標記使用再騰位；`invalidateAsset(id, const IrView* source = nullptr)`（null = 所有來源）；`invalidateAllAssets()` 全丟；貼圖只在同一 atlas 頁的 cell 之間共享（預設 `imageView()` 的 scratch 位址不再被當作同一頁）。`lastFrameReusable(assets, ir)`：來源表建置後 `ir` 須為該次來源之一，其他來源的 `engine://` 綁定答 false；它每幀呼叫一次並推進重試計數，重試到期時答 false 一次。

**裝置拒絕**：`GpuDevice::maxTextureSize()`（0 = 未知；GL 回 `GL_MAX_TEXTURE_SIZE`），每次建置問一次；超過上限的圖不呼叫 `createTexture`、每個資產記一次 `image <w>x<h> exceeds the device texture limit <n>` 並記住到失效為止（壓縮圖同）；上限內 `createTexture` 回 0 視為暫時失敗，60 幀後重試、最多 3 次（第 1、61、121、181 幀），之後記住到失效或裝置重設。

**builder 與 `releaseSource`**：Context 不把 `releaseSource` 轉給 builder；宿主要立刻丟貼圖就呼叫 `invalidateAllAssets()`。

## 10.9 Direct 目標模式與帶圖層（2026-10-15；23 §23.aa；T-343）

`FrameBuilder` 與 `xt_sw` 新增 Direct 模式：目標 0 為宿主場景目標、不清除；繪製序切帶（23 §23.aa.2），帶圖層走 `acquireBandTarget`（預設 = T-162 持久目標）；S 元素在 transient 內裁切後以 Composite 貼上，場景目標不需 stencil；backdrop 區域拷貝合併、共用金字塔（18 §18.15）。`GpuDevice` 追加 `supportsDirectTarget`／`acquireBandTarget`／`releaseBandTarget`（皆預設實作，舊裝置不變）。`lastFrameReusable` 在 Direct 模式表示「帶圖層可重用」。

## 10.10 WebGPU 後端 `xt_wgpu`（M34；使用者 2026-10-22「來上點難度吧：wgpu backend」；定案 v1.0，2026-10-23 批准）

**定位**：第三個 `GpuDevice` 實作，與 `xt_gl` 同階——`CommandList` → `FrameBuilder` → `GpuFrame` → `WgpuDevice::submit()`。目的：(1) 一條不靠 OpenGL 的桌面路徑（Windows D3D12、macOS Metal、Linux Vulkan 全由 WebGPU 抽象），(2) 給編輯器第三條預覽路徑（17 §17.34），(3) 作為 45 §45.7 Backends 的 `wgpu` renderer 範例與未來瀏覽器目標的種子。層：R（`Code/Runtime/wgpu/`，MIT）。

### 10.10.1 SDK 與建置

- API 用 `webgpu.h`（WebGPU C 標頭）＋ wgpu-native 的 `wgpu.h` 擴充；實作用 **wgpu-native** 的預建二進位（動態庫 `wgpu_native.{dll,so,dylib}` + 標頭），不用 Dawn（建置太重）。版本釘在 `Code/SDKs/versions.cmake`（每個平台一個 zip URL＋sha256），落在 `Code/SDKs/_fetched/wgpu-native/<platform>/`；授權 MIT／Apache-2 進 `NOTICE.md`。
- CMake 選項 `XT_BUILD_WGPU`：預設 ON；該平台沒有預建包或 `XT_SDK_OFFLINE` 下缺目錄 → 自動 OFF 並印訊息，不擋整棵樹。動態庫由 `xt_runtime_output.cmake` 複製到 `bin/<tag>/<Config>/`（與 Qt DLL 同一機制）。
- 公開標頭：`Code/Runtime/wgpu/include/xt/wgpu/{wgpu_device.h, wgpu_renderer.h}`。`WgpuRenderer` 的介面對齊 `GlRenderer`（10.3）：`init(const WgpuInit&)`（給定 `WGPUDevice`／`WGPUQueue`／目標格式／取樣數，或 `WgpuInit::headless()` 自建 instance＋adapter＋device）、`resize`、`render(const CommandList&, FontProvider*, AssetResolver*, WGPUTextureView target)`、`invalidateImages()`、`stats()`；另有 `readback(WGPUTexture, std::span<uint8_t>)`（`copyTextureToBuffer` + `mapAsync`，256 位元組列對齊，供編輯器與測試）。adapter 選不到（無 GPU、無驅動、沙箱）→ `init` 回 false 並填 `lastError()`，**不**中止程式。後端選擇：預設 D3D12／Metal／Vulkan，環境變數 `XT_WGPU_BACKEND=vulkan|d3d12|metal|gl` 覆寫，`XT_WGPU_FALLBACK=1` 允許軟體 adapter（WARP／lavapipe，CI 用）。

### 10.10.2 `GpuFrame` → WebGPU 對應（與 10.5 的語意一一對應，不改 `xt_gpu`）

| `GpuFrame` | WebGPU |
|---|---|
| `Vertex` 24 bytes、32 位索引、`baseVertex` | 一個頂點緩衝（`vertexBufferLayout` stride 24：pos×2 f32、w f32、uv×2 f32、rgba u8×4 normalized）、一個索引緩衝；`drawIndexed(count, 1, firstIndex, baseVertex)`。三組緩衝輪替＋CPU 影子只上傳變動區段（同 T-159）。 |
| `Shading`×`Blend`×`StencilOp`×`DrawKind`×目標格式×取樣數 | **管線快取**：鍵 = 上述組合；fragment shader 依 `Shading` 一支 WGSL（Solid／Gradient／Texture／TextureStraight／Glyph／StencilOnly／Composite 17 種／Effect 12 個 program／Particle×2）；`StencilOnly` 關 color write。 |
| 8 位 stencil：高 4 位 clip 深度、低 4 位 winding | 深度模板附件 `Stencil8`；`StencilOp::WriteIncrWrap` = front `IncrementWrap`／back `DecrementWrap`、`compare=Equal` 以 `stencilReadMask=0xF0` 閘控深度；`WriteInvert` = `Invert`；`TestNotEqualZero` = `compare=NotEqual` ref 0 mask 0x0F；`TestEqualRef`；`ClearRect` = 畫矩形 `Replace` ref；`ClipPush`／`ClipPop` = `Replace` 寫新深度值（10.5 的一次完成語意）。`setStencilReference` 每命令。 |
| `Pass{target, viewport, clear, copy}` | 一個 `RenderPassEncoder`；`target 0` = 呼叫者給的 view（或 Direct 模式的宿主 view）；transient = `Rgba8Unorm`／`R32Float` 紋理（`RENDER_ATTACHMENT | TEXTURE_BINDING | COPY_SRC`），MSAA 時 4x 附件 + `resolveTarget`；`copy` = `copyTextureToTexture` 進 `aux`。視窗化 transient（T-151）同 GL：尺寸向上取 32、scissor 與視窗交集。 |
| `uniform`／`paramIndex` | 一個大 uniform 緩衝（每幀重填）+ bind group 的 **dynamic offset**（256 對齊）；group 0 = 幀常數（ortho、viewport），group 1 = 每命令參數（`GradientParams`／`EffectParams`／composite 模式），group 2 = 紋理＋sampler（src、aux、LUT、atlas）。 |
| 貼圖、字形 atlas、256×1 LUT | `writeTexture`；atlas R8；`TextureHandle` → 內部表（含宿主給的外部 `WGPUTextureView`，T-150 的 `engine://` 語意）。 |
| 粒子（10.7） | 實例化：第二個頂點緩衝 `stepMode=Instance`（`ParticleInstance` 40 bytes），每實例 6 個頂點（同 GL；T-438 修正，原寫 `draw(4,…)`）。 |
| Direct 目標模式（10.9） | `supportsDirectTarget()=true`；`acquireBandTarget` 走持久紋理池；backdrop 區域拷貝 = `copyTextureToTexture`。 |
| `prepareCustomEffect(glsl…)` | v1：wgpu-native 的 GLSL 前端（`WGPUShaderModuleGLSLDescriptor`，Naga）直接吃 18.7 翻譯器產的 GLSL；v2：40 §40.18 的 HLSL → DXC → SPIR-V → Naga（SPIR-V 入口）。失敗 → 套 `fallback`（同 SW）。 |
| 池預算（T-163） | 同 GL 的位元組預算池；`maxTextureSize()` 讀 `limits.maxTextureDimension2D`。 |

**T-438 對帳後的修正**：4x 只用在需要 stencil 的 transient（與 GL 的 `acquireDrawTarget` 一致）；自訂效果的頂點階段也走 GLSL 包裝（WGSL 輸出與 GLSL 輸入的插值取樣過不了 wgpu 驗證）；4x 表面未設清除色時先以全螢幕 blit 把宿主內容載入 4x 附件，因此宿主 view 需要 `TEXTURE_BINDING`（Direct 模式同）；bind group 跨幀快取（相關 view 釋放即整批清掉）。效能：challenge-menu 1920×750 render CPU wgpu D3D12 0.50 ms／Vulkan 0.33 ms／GL 0.17 ms，差距在 wgpu-core 的指令錄製（submit 時約 0.30 ms），21 §21.7 的 1.2 倍目標以「render＋等待」計（0.66／0.54 對 GL 0.46 ms）。

與 GL 的刻意差異：沒有 `saveState/restoreState`（WebGPU 無全域狀態）；MSAA resolve 由 pass 的 `resolveTarget` 做，不需 sampler2DMS；premultiplied 管線、LUT、scissor 矩形 clip 全同 10.5。黃金圖：與 GL 同一組 `Tests/golden`，容差同 GL（15 章）。

### 10.10.3 測試、效能、CI

- `Tests/wgpu/`：`xt_wgpu_tests`——headless 建裝置；`GpuFrame` 結構快照（`Tests/gpu/golden/*.txt`）本來就裝置無關，不重做；像素黃金圖以 headless 渲染＋`readback` 比對（GL 容差）；stencil／clip 深度、transient 視窗、Direct 模式、粒子、自訂效果各一案。**找不到 adapter 時整個測試執行檔印 `SKIP: no WebGPU adapter` 並回 0**（CI 的 hosted runner 可能沒有 GPU）；`XT_WGPU_FALLBACK=1` 時改用軟體 adapter 跑完。
- 效能：`FrameStats::renderMs/effectsMs/textMs` 與 GL 同義（CPU 提交時間）；21 章新增一行 `xt_wgpu` 對照；`xtui-bench` 加 wgpu 欄（第五輪）。
- CI（15 §15.8）：`windows` 腿多跑 `xt_wgpu_tests`（D3D12；hosted runner 用 `XT_WGPU_FALLBACK=1` 走 WARP）；`linux` 腿裝 `mesa-vulkan-drivers` 用 lavapipe；`macos` Metal；`sanitizers` 腿不建 wgpu（動態庫外部碼）。`local_ci.sh` 同步。
- 鏡像倉：`xt_wgpu` 屬 R，隨 runtime 匯出；`versions.cmake` 的 wgpu-native 條目一起。

### 10.10.4 分期

| 單 | 內容 |
|---|---|
| T-437 | SDK 接入（`versions.cmake`、`_fetched`、`XT_BUILD_WGPU` 守衛、動態庫複製）＋`xt_wgpu` 骨架：裝置／佇列／instance、`createTexture/updateTexture/destroyTexture`、空 `submit`、headless 建立與 `lastError`、`readback`；`xt_wgpu_tests` 的 SKIP 機制 |
| T-438 | `submit` 全部：管線快取、緩衝輪替、stencil 表、pass／transient／MSAA、Composite 的 aux 拷貝、12 個效果 program 的 WGSL、LUT／atlas／外部貼圖、粒子、Direct 模式、自訂效果 v1（GLSL 前端）、池預算；黃金圖全過 |
| T-439 | 45 §45.7 Backends：`xt_renderer_wgpu.{h,cpp}`、`xt_backend_sdl2_wgpu.cpp`（SDL 視窗 → `WGPUSurface`：Win32 HWND／X11／Wayland）、`xt_backend_win32_wgpu.cpp`；`xtframe-view --renderer sw\|gl\|wgpu`（執行期選擇已編進來的 renderer；`XT_SHELL_GL` 維持） |
| T-440 | 編輯器 17 §17.34：`WgpuPreviewWidget`、`--preview-backend`、偏好設定、About、狀態列、退回鏈 |
| T-441 | 測試與 CI：三個平台的 CI 腿、`local_ci.sh`、21 章對照、`xtui-bench` 第五輪、`Docs/COMPATIBILITY.md` 實驗表加「wgpu 預覽（實驗）」 |

順序：T-437 → T-438 → T-439 ∥ T-440 → T-441。每張 Opus；T-438 是大單，可拆 a（填充／描邊／clip／layer）、b（效果／composite／粒子／Direct）。

> 實作記錄（T-437）：wgpu-native 釘 v29.0.1.1（`versions.cmake` 新增 `PREBUILT` 種類，`xt_sdk_prebuilt` 解到 `_fetched/wgpu-native/<platform>/`，失敗只關 `XT_BUILD_WGPU`）；`xt_wgpu` 骨架（`WgpuInit::headless`、`WgpuDevice` 貼圖／transient 池／4x 附件／pass 清除、`WgpuRenderer::readback`）完成，`submit` 的繪製命令、copy 與 cache store 只回報 `submit: not implemented` 留給 T-438；wgpu-native 未實作 `wgpuDeviceGetAdapterInfo`（呼叫即 panic），adapter 名稱改由 `WgpuInit::adapter` 取得。

> 實作記錄（T-438）：`WgpuDevice::submit` 全部完成——三組輪替緩衝＋影子差異上傳、單一 uniform 緩衝以 dynamic offset 供三個 bind group、管線快取、`Code/Runtime/wgpu/src/shaders/*.wgsl` 逐檔對照 `gl_shaders.h`、stencil 表逐項同 `gl_device.cpp`、只有需 stencil 的 transient 走 4x（同 GL）、`copy` 能用 `copyTextureToTexture` 就用、否則（4x 目的地、表面、異格式）以 blit 繪製、粒子每實例 6 頂點（同 GL，非表中的 4）、自訂效果經 Naga GLSL 前端（片段與頂點都用 GLSL：WGSL 輸出與 GLSL 輸入的插值取樣在 wgpu 驗證下不相容），4x 表面未設清除色時第一個表面 pass 前先以全螢幕 blit 把宿主內容載入 4x 附件（宿主視圖因此與 Direct 模式一樣需 `TEXTURE_BINDING`），`xt_golden --backend wgpu` 159 個 fixture 在 D3D12／Vulkan／WARP 以 GL 容差（16、3%）全過，challenge-menu 1920×750 的 render CPU 時間 D3D12 0.50 ms、Vulkan 0.33 ms、GL 0.17 ms（多出的部分在 wgpu-core 於 finish/submit 時錄製指令）。

> 實作記錄（T-439）：`Code/Backends/xt_renderer_wgpu.{h,cpp}` 以 `WgpuInit::headless()` 建裝置（環境變數只在那裡讀）後在同一個 instance 上建 `WGPUSurface` 並以 `wgpuSurfaceGetCapabilities` 確認 adapter 能呈現，表面取 BGRA8Unorm／RGBA8Unorm（非 sRGB）、Fifo、有 COPY_SRC 就加上以便讀回，每幀都清除所以 4x 表面不需 TEXTURE_BINDING；`xtframe-view --renderer sw|gl|wgpu` 的預設不變（`--headless` 為 sw、視窗為殼層的 gl），`--headless` 下 gl／wgpu 走離屏（隱藏 GL 視窗的 FBO／headless 裝置），hud.xt 640×360 三者截圖兩兩在 GL 容差（16、3%）內；`WgpuRenderer`／`GlRenderer` 未改。

> 實作記錄（T-456）：shader module 與每支 render pipeline 的建立都包在 Validation＋OutOfMemory 兩層 error scope（wgpu-native 不接受 Internal 過濾器，傳入即 panic）裡，失敗的管線鍵快取為「不可用」、由第一個需要它的 `submit` 在 `lastError` 報一次（之後不再重報）、其 `DrawCmd` 略過而其餘照畫，無效管線永遠到不了 `wgpuQueueSubmit`；`WgpuDevice::init`（因此 `WgpuRenderer::init`）先以一支無繫結的最小 WGSL 管線預檢後端的 shader 編譯器，失敗即回 false 並說明（T-440 的 D3Dcompiler ps_5_1 情況由此交給編輯器的退回鏈），預檢 WGSL 可由測試掛點 `WgpuDevice::setProbeShaderForTesting` 替換。

> 實作記錄（T-457）：uniform 緩衝成長時舊緩衝先釋放再建新緩衝，新 `WGPUBuffer` 可能重用舊位址，原本以 handle 比對是否重建 group 0／1 bind group 因此漏判（Vulkan challenge-menu 的「offset 65536 超出 65536 位元組緩衝」panic），改為 `ShadowBuffer::generation` 每次建立遞增、bind group 依代數重建，並在上傳大小超過緩衝時略過該幀繪製；編輯器的 windeployqt 加 `--no-system-d3d-compiler` 並刪除殘留的 `D3Dcompiler_47.dll`，`xt_runtime_output.cmake` 在共用 bin/ 仍有該檔時發 configure 警告；`GlRenderer` 的 `effectsMs`／`textMs` 填入 Effect／Glyph 命令的 CPU 繪製時間（與 xt_wgpu 同義）。

## 10.11 不靠 MSAA 的抗鋸齒（2026-11-11 定案；Sherefox 記憶體與 GPU 調查；使用者：參考 Godot 的 UI 抗鋸齒、細線用 mip 與過濾；`Docs/reviews/2026-11-11-ui-antialiasing-godot.md`）

**目標**：UI 表面預設以 1x 繪製，品質不低於現在的 4x，並以 SW 光柵化（09）為像素基準。換得：全螢幕 surface 由 22 B/px 降為 5 B/px（1440p 88.5 → 18.4 MB）、layer／mask transient 由 24 降為 5 B/px、所有 resolve 與 4x load／store 消失；XTAtrium 實測 4x 比 1x 多約 256 MB GPU 專用與 280 MB private。

**現況（調查結論）**：1x 時 `edgeFringe` 已開——凸形填色、凸環邊框、單邊邊框、單段線、軸對齊與旋轉／傾斜的圖集文字都有 1 裝置像素、變換後計算的覆蓋率邊緣，次像素寬度以 alpha = 寬度補償而不消失。Godot 4.7.2 不開 MSAA 時靠的也是這一類（CPU 羽化幾何），且它的羽化寬度是本地單位、直角方塊與多邊形沒有 AA；所以本節不是照搬 Godot，而是把我們已有的做法補到涵蓋全部內容。1x 仍是硬邊的只有走 stencil-then-cover 的內容與無邊緣處理的影像 quad。

**定案**（每項一張單，依序做，每張併入並確認像素後才開下一張）：

1. **影像 quad 邊緣羽化（T-659）**：非軸對齊的 `DrawImage`／`DrawImageQuad` 外加一圈 1 裝置像素、alpha 0、UV 夾在邊緣的頂點（與 `fillConvex` 的 fringe 同規則：核心內縮 `woff`）。軸對齊且落在像素格上的影像不變。只在 `edgeFringe` 開時生效。
2. **覆蓋率圖集（T-660 基礎＋凹形填色／SVG）**：`xt_gpu` 新增 R8 覆蓋率圖集（與字形圖集同型，獨立頁）；走 `fillShapeAndCover` 的填色改為：以 SW 的解析光柵化器（`sw_raster` 的 signed-area 累積；抽成 `xt_core` 可共用的無相依單元，SW 行為不變）把「路徑 × 當前變換的線性部分」光柵化成覆蓋率位圖，存入圖集，以一個貼圖 quad 繪製（著色與漸層照原 `Shading` 乘上覆蓋率）。
   - 鍵：路徑內容雜湊、fill rule、變換線性部分（量化到 1/64）、次像素位移（量化到 1/4 px）。平移不重新光柵化。
   - 上限：位圖單邊 > 1024 裝置像素、或同一幀新光柵化的像素總量超過預算（預設 2 Mpx／幀）時，該形狀本幀退回 stencil-then-cover（硬邊），下一幀再試；避免形變動畫造成尖峰。
   - 淘汰：頁滿時以未使用幀數最久者整頁回收（同字形圖集）；`xt_gpu_memory_stats.atlas_bytes` 計入。
   - 只在 `edgeFringe` 開（1x）時啟用；4x 行為不變。
3. **筆畫（T-661）**：`doStroke` 中未被凸形直繪接住的筆畫（折線、封閉、有接合與端點）走定案 2 的圖集（對 `strokeToFill` 的輪廓做 NonZero 覆蓋率）。次像素寬度由覆蓋率自然得到「不斷、alpha 隨寬度」。
4. **輪廓文字（T-662）**：`doOutlineGlyph` 的各種情形（> 96 px、縮放中、直排／四分之一轉、鏡像、非 auto）改走定案 2 的圖集，以字形輪廓為路徑；3D 投影維持輪廓路徑（另案）。縮放動畫中每幀變換不同會持續新光柵化，受定案 2 的每幀預算約束。
5. **圓角與旋轉裁切（T-663）**：
   - (a) 「單一影像＋圓角裁切」（圓角圖片、頭像）不建裁切層：以影像著色的凸圓角矩形填色（`fillConvex` 加貼圖座標）直繪。
   - (b) 其餘圓角矩形／旋轉矩形裁切：stencil 仍決定內部（二值），另以定案 2 的覆蓋率位圖作邊緣——裁切層結束時，對裁切輪廓外緣 1 px 環帶內被畫到的像素乘上覆蓋率。實作方式（解析式 rrect 於片段著色器，或 R8 裁切遮罩合成）在 T-663 開單前由主模型依 T-660 的實測再定，本節先不定。
6. **L 形與每邊不同色的圓角邊框（T-664）**：走定案 2 的圖集；順手修 `ringMinWidth` 以最薄邊縮放整環 alpha 的問題（改為各邊各自的寬度）。
7. **縮小取樣（T-665）**：影像貼圖在第一次被以 ≥ 1.5× 縮小繪製時才產生 mip 鏈（預乘後 2×2 box，GPU blit 鏈），取樣器改三線性；沒被縮小過的貼圖維持單層（不白付 33% 記憶體）。DDS 既有 mip 鏈直接上傳。SW 基準同步：`drawImage` 在縮小 ≥ 2× 時改用與 GPU mip 等價的 box 預過濾（SW golden 會變，T-665 內重產並逐張人工確認是變好）。合成器把 surface 縮小畫進 `dest` 的情形不在本單。
8. **預設取樣數改為 1（T-666）**：定案 1～6 併入後，`xt_gpu_target_desc.sample_count` 的 0 改為代表 1；4 仍可指定（行為與今天相同，`edgeFringe` 關）。`xtframe-view --msaa` 預設同步改 1。屬行為變更，隨下一個 minor 版號並通知宿主。
9. **驗收方式**：每張單加 1x 的 GPU vs SW golden（既有 wgpu golden 本來就是 1x 對 SW、16 階／3%），並在報告附 4x／1x／SW 三方裁切對照圖的路徑；`Tests/gpu/golden/*.txt` 結構快照依單重產。CPU 成本以 `view --perf` 的 build ms 報告，靜態畫面（圖集全命中）不得高於修改前。

**不做**：MSDF／SDF 文字（失去 hinting、第二條文字路徑）；選擇性 MSAA（保留了要拿掉的東西）；全面改為著色器解析式圖元（三個後端都要動，效益與圖集重疊）。GL 後端（10.3）沿用 `xt_gpu` 的同一套，隨各單自動取得；CE 的 Host 後端不在本倉。

**T-659 實作記錄（#621，2026-11-11）**：`frame_builder.cpp` 的 `imageQuadOnPixelGrid`／`drawImageQuadEdges`／`drawFringedImageQuad`。「在像素格上」＝裝置空間每邊 |dx| 或 |dy| ≤ 1e-3 且每個角的 x、y 距整數 ≤ 1e-3（同 SW 的 `kAxisEps`）；羽化條件：`edgeFringe` 開、`smooth != 0`、非投影、不在格上、嚴格凸。新 fixture `ui_image_transform`（旋轉 7°、skewX、非整數縮放、格上對照、圖集格旋轉）：1x GPU 對 SW 超差像素 620 → 7（餘下在 quad 角，miter 高估，同 `fillConvex`）、最大通道差 133 → 31；4x 逐位元組不變；既有結構快照無一變動。未處理：3D 投影影像、`DrawImageMesh`、粒子；旋轉九宮格的格縫與 SW 同樣可見。


## 10.12 GPU 路徑的每幀成本：只畫變動的部分（2026-11-11 定案；Sherefox debate 調查與 T-658 計數）

**事實**（debate、1440p、T-658 之後）：每幀 18.8 個 render pass、146 個 draw call、132 次 pipeline 綁定、效果快取命中 16.4／未命中 2.0；任何一個節點變動就整張 surface 重建重畫（C ABI 的 surface 走 Layer 模式，damage 只在 Direct 模式使用）；約 98% 的 tick 是髒的。SW 路徑已有 damage 局部重繪（23 §23.2，T-642／T-644），GPU 路徑沒有對應物。

**目標**（同一場景、同一解析度，穩態每幀）：重畫的像素量與 damage 面積同階而不是整張；render pass ≤ 4；動態參數的方塊陰影 0 個離屏 pass；靜止的幀 0 個 pass（已是）。

**定案**（每項一張單，依序；排在 10.11 的 T-660b 之後穿插，順序由主模型依量測調整）：

1. **GPU damage 局部重繪（T-667）**：Layer 模式的 surface 貼圖跨幀保留為上一幀內容；有 damage 且非全髒時，frame builder 只建「包圍盒與 damage 矩形集合相交」的命令，surface pass 以 `LoadOp::Load` 開啟，先以 damage 矩形清除（scissor＋clear quad，混合模式 Copy），再於同一 scissor 內繪製。
   - damage 來源與 SW 相同（`Context` 的多範圍 damage，T-644）；矩形數 > 8 或總面積 > surface 的 60% → 當全髒。
   - 與 damage 相交的 PushEffect／PushLayer 區段整段重建（效果快取照常命中）；不相交者整段跳過。
   - 4x 時 MSAA 色彩附件同樣跨幀保留（已是持久附件）；resolve 仍整面一次（wgpu 無局部 resolve）。1x 無此成本。
   - 讀取 surface 的 pass（backdrop、非 Normal 混合）出現在該幀時，該幀當全髒（先求正確；之後再放寬）。
   - trim／resize／裝置重建／`edgeFringe` 或取樣數變更後第一幀全髒。
   - 驗收：對同一段動畫，局部重繪與強制全髒逐位元組相同（`xtframe-view --partial` 擴到 wgpu）；`WgpuStats` 加 `damagePixels`、`skippedCommands`。
2. **方塊陰影不走離屏（T-668）**：painter 在發出 `effect: shadow()`（drop shadow）時判定內容輪廓是否為「一個（圓角）矩形」——節點有不透明度 1 的背景填滿自己的邊框盒、沒有會超出該盒的子內容（`overflow` 非 visible，或所有子孫的可繪包圍盒都在盒內）、背景自身 alpha = 1。成立時不發 PushEffect，改發 `DrawBoxShadow`（圓角矩形、位移、模糊、擴散、色彩）命令，然後照常畫內容：
   - 模糊 = 0：以陰影色畫位移並擴張後的圓角矩形（凸形填色，沿用 fringe）。
   - 模糊 > 0：一個 quad、片段著色器以圓角矩形高斯的閉式近似計算覆蓋率（矩形部分用 erf 的解析積分，圓角以徑向修正；公式與常數寫進任務單），不建任何離屏目標、不斷 surface pass。
   - SW 以同一公式實作 `DrawBoxShadow`（兩個後端同源，互為基準）；與原本「離屏＋高斯」的差異在任務單內量測，超過 golden 容差的情形回報由主模型裁決（改公式或限縮判定條件），不得自行放寬容差。
   - 判定不成立的陰影（文字、SVG、透明或不規則內容）維持現有離屏路徑。
   - 動畫只改陰影參數（如 Sherefox 的 `litpulse`）時，每幀成本 = 一個 draw。
3. **不重疊的不透明度群組不建圖層（T-669）**：`opacity < 1` 且有 2 個以上可繪子項的節點，若各子項的可繪包圍盒兩兩不相交且群組內沒有混合模式／效果／裁切層，painter 把群組 alpha 乘進各子項，不發 PushLayer。相交或無法判定時維持圖層。像素與圖層合成在數學上相同（各自預乘後 SrcOver 於同一背景），以逐位元組或 1 階內為驗收。
4. **批次**（先量再定，不在本節定案）：T-660b 併入後（stencil 路徑大幅減少）重新取 `view --perf` 的 draw／pipelineBinds，另派唯讀調查列出 pipeline 切換的實際序列，再決定是否把 Solid／Gradient／Image／Coverage 併成單一 pipeline。
5. **不做**：為 4x 做局部 resolve 的變通；跨 surface 的合併繪製（surface 直畫目標，PENDING B6）。

**T-660a 實作記錄（#623，2026-11-11）**：`xt::CoverageRasterizer`（自 `sw_raster.cpp` 原樣搬入 `core/src/coverage_raster.cpp`；`sw_raster.h` 留別名）、`xt::rasterizeCoverage(verbs, points, deviceFromLocal, rule, CoverageBitmap&)`（與 SW `fillPath` 同一個 `flattenPath`／`kFlattenTolerance` 0.25／輪廓封閉；包圍盒外擴 1 px；單邊上限 1024）、`xt::gpu::CoverageAtlas`（首頁 512² 長到 1024²、最多 4 頁、shelf 裝箱、鍵排序向量二分搜尋、整頁 LRU 回收、每幀預算 2 Mpx）。五個形狀的覆蓋率與 SW alpha 逐位元組相同；SW golden 175 全過。SW 與 frame builder 的展平相同，唯一差別是 SW 畫進偏移的圖層緩衝時多減一個原點。**10.11 定案 2 修正（T-660b 起）**：鍵的線性部分不量化，改存四個 float 的位元樣式（−0 正規化為 +0）——1/64 的量化在 512 px 的形狀上會造成數像素的誤差；靜態形狀本來就逐位元相同，動畫中的形狀無論如何都要重新光柵化。位移仍量化到 1/4 px。

**T-660b 實作記錄（#624，2026-11-11）**：`DrawCmd::coverage`（R8 貼圖槽，只用於 Solid 與 Gradient；`PaintKind` 沒有影像著色，故不含 image pattern）、`CoverageGradientParams`（漸層參數＋ uv → 路徑本地的仿射）；wgpu `coverage_solid.wgsl`／`coverage_gradient.wgsl`、GL `kCoverageSolidFrag`／`kCoverageGradientFrag`（兩個裝置都完整實作）。frame builder `fillCoverage`：條件 `edgeFringe && antialias != 0 && !projecting()`；鍵為內容雜湊＋線性部分位元樣式＋1/4 px 位移（`splitCoverageTranslation` 處理進位）；頁在幀中成長時該幀重建一次。1x GPU 對 SW 的超差像素／最大通道差：`ui_coverage_fill`（新）1064／140 → 27／18（餘下為 1/4 px 位移量化）、`scene_taper` 1593／216 → 0／1、`lottie_path_bezier` 392 → 0、`lottie_star_polygon` 285 → 0、`ui_border_styles` 307 → 0；仍高者（`scene_modifiers`、`ui_svg_modifiers`、`ui_game_hud`）是筆畫與其他未接路徑（T-661 起）。4x 逐位元組不變；debate 靜態 build ms 無退步。既有測試改動：capi `gpu text 2` 的星形改為斷言 1x 與 SW 的中間 alpha 像素數相當。**待處理**：> 1024 px 的凹形退回硬邊（debate 1440p 每幀 1 個）——T-661 開單時一併決定（分塊或提高上限）。未編譯：Clang／GCC／Release。

**10.11 定案 2 補充：大形狀分塊（2026-11-11；T-660b 實測 debate 1440p 每幀一個 > 1024 px 的凹形退回硬邊；筆畫常見「又長又細」同樣會超限；T-672）**

1. `rasterizeCoverage` 的位圖單邊上限由 1024 提高到 **4096**（暫存位圖由呼叫端重用；超過者退回 stencil）。
2. 位圖任一邊 > 512 的形狀**分塊**存入圖集：以位圖左上角為原點切成 256×256 的磚（邊緣的磚較小），每塊磚各自是一個圖集項，鍵＝形狀鍵＋磚座標 `(tx, ty)`。整個形狀一次光柵化後切磚。
3. **均勻磚不佔空間**：全 0 的磚記為「空」（不畫、不存）；全 255 的磚記為「滿」，繪製時取樣圖集裡一個保留的全 255 texel 區（每頁建立時預留 4×4、UV 取其中心），與其他覆蓋率 quad 同一批次。只有跨越邊緣的磚才真正佔圖集。水平相鄰的「滿」磚合併成一個 quad。
4. 任一磚未命中（例如所在頁被回收）→ 整個形狀重新光柵化並補回缺的磚（已在的磚不重插）。
5. **預算**：每幀預算仍為 2 Mpx，但**該幀第一個需要光柵化的形狀不受預算阻擋**（否則大於預算的單一形狀永遠畫不出來）；之後的形狀照舊受限。
6. 上限：磚數 > 1024 的形狀退回 stencil。
7. 統計加 `coverageTiles`（存入的邊緣磚）、`coverageUniformTiles`。

**T-672 實作記錄（#627，2026-11-11）**：`kMaxCoverageBitmapSide` 4096；`CoverageKey` 加 `uint8_t tile`（0＝整個形狀或分塊形狀的表頭項、1＝磚）與 `tileX／tileY`；`CoverageEntryKind {Bitmap, Empty, Full, Tiled}`、`lookupTiles`／`insertTiles`；每頁右下角保留 4×4 全 255 區（建立、成長、回收時寫入）。**解讀採用**：漸層的「滿」磚不取樣保留區（覆蓋率漸層著色器由 uv 推算著色位置，uv 恆定會把漸層壓成單色），改以一般 Gradient cover quad 繪製；純色滿磚照定案取樣保留區。均勻磚與表頭項掛在該形狀第一塊邊緣磚所在的頁、隨頁回收。debate 1440p 1x：fallbacks 1.03 → 0／幀、draw 148.6 → 146.6；該形狀是對齊像素格的 1600×104 面板，前後畫面逐位元組相同（7 塊磚全是邊緣磚）。`ui_coverage_fill` 對 SW 仍為 27／18（大形狀的邊都在整數像素上）。「磚數 > 1024 退回」經由填色無法觸及（4096 上限最多 256 塊），只在圖集層測試。未測：GL 套件；未編譯：Clang／GCC。

**T-661 實作記錄（#628，2026-11-11）**：SW 的筆畫＝裝置空間展平 → 虛線（陣列與相位乘 `sqrt|det|·dpr`）→ `strokeToFill`（寬度乘同一比例）→ NonZero 光柵化；GPU 在圖集未命中時重複完全相同的步驟（`rasterizeStrokeCoverage`、`xt::rasterizeCoverageContours`），唯一差別是位移取 1/4 px。鍵雜湊裝置空間的實際幾何參數（寬度×比例、縮放後的虛線與相位、展平容差）。1x GPU 對 SW 的超差像素：`scene_modifiers` 1164 → 0、`scene_dash` 880 → 0、`lottie_gradient_stroke` 824 → 0、`ui_svg_basic` 731 → 0、`ui_svg_modifiers` 658 → 14、`ui_game_hud` 713 → 133（餘下未查，屬後續路徑）、新 fixture `ui_coverage_stroke` 2385 → 82（旋轉虛線的 1/4 px 位移量化）。0.25 px 折線連續、無缺口。Sherefox debate 1440p 1x：draw 146.6 → 95.3／幀、coverage hits 22.3、圖集 3 MiB、穩態 misses 0、build ms 持平。未做：3D 投影下的筆畫。未編譯：Clang／GCC。

**10.11 定案 2 補充 2：預算退回的幀必須補畫（2026-11-11；T-660b 解讀 7 揭露的缺口；T-662 內處理）**：因每幀預算而退回 stencil（硬邊）的形狀，在靜態畫面上若該幀被判為可重用，就會一直停在硬邊。定案：某次 build 有「因預算退回」的形狀時，(1) `lastFrameReusable` 回 false；(2) frame builder 提供 `wantsAnotherFrame()`，renderer 往上傳遞——C ABI 的 surface 把它視為下一次 `xt_surface_update`／render 的全髒 damage（即使文件沒有變動也重畫一次），`xtframe-view` 與編輯器預覽同樣再畫一幀；直到沒有預算退回為止。因尺寸上限（> 4096）而退回的不算（重畫也不會變）。另：預算的計量由「位圖包圍盒面積」改為「光柵化器實際走訪的像素數（各掃描線有覆蓋的跨距總和）」的事後記帳——細長斜線的包圍盒很大但實際工作量很小；事前的 `budgetAllows` 檢查改為只看「本幀已用量是否已達預算」。

**T-662 實作記錄（#629，2026-11-11）**：A——`CoverageBitmap::coveredPixels`（掃描回呼內累加非零跨距，無額外掃描）、`budgetAllows()` 無參數（本幀已用 < 預算；首形狀特例移除）、`FrameBuilder::wantsAnotherFrame()`（只計預算退回）、`WgpuRenderer`／`GlRenderer` 轉發、C ABI `SurfaceGpu::repaint`（`xt_surface_render_gpu` 僅在 `!repaint` 且無 damage 時跳過）；`xtframe-view` 與編輯器預覽每 tick 都重建，無需改。B——輪廓字形先試覆蓋率圖集、再試直繪、最後 stencil；SW 的輪廓字形是線性覆蓋率不套 gamma（gamma 只在 < 24 px 的 hinted 位圖路徑），與圖集著色器一致。字形鍵＝（"GLYPHOL1"、shaper 指標、FontId、glyph id、字級、展平容差）。1x 對 SW：`ui_coverage_text`（新）5651 → 1257（餘下：1/4 px 位移量化在水平邊約 22 階；24～96 px 帶 GPU 用 hinted 圖集而 SW 填輪廓，T-263c 已記）、`ui_i18n_vertical_mixed` 175 → 0；capi 120 px 字的中間 alpha 像素 SW 1294／1x 1299／4x 778。Sherefox 1440p 1x：title draw 40.2 → 27.7、cutin 12.2 → 8.9；cutin 的縮放動畫期間大字每幀重新光柵化（build 0.18 → 0.39 ms 平均）。**待修（T-663 內順手）**：字形鍵含 shaper 指標——bundle 重載後新 shaper 可能落在同一位址而誤命中舊項；改用 shaper 的世代編號或在 shaper 更換時清空覆蓋率圖集。未編譯：Clang／GCC。

**10.11 定案 5 定稿：圓角與旋轉裁切（2026-11-11；T-663）**

裁切的抗鋸齒不為每個 draw 加新機制，而是先把「其實不需要裁切」的情形拿掉，剩下的才付成本。全部在 frame builder（GPU 路徑、`edgeFringe` 開時）判定，SW 與 painter 發出的命令不變。對每個會走 `pushClipStencil` 的裁切層（圓角矩形 overflow、圓角影像裁切、旋轉矩形、任意 clip path），在 Push 時向後掃到對應的 Pop，求出其間所有繪製命令的裝置空間包圍盒聯集 `C`（效果／圖層以其視窗為準；掃描結果按命令範圍記憶，同一 build 不重算）：

1. **不裁**：`C` 完全落在裁切形狀的「安全內區」內——圓角矩形取各邊內縮對應圓角半徑後的內接矩形與兩條十字帶的聯集（即 `C` 不碰四個角的方塊且不超出外框）；旋轉矩形取 `C` 的四個角都在矩形內。此時不發任何裁切（不動 stencil、不動 scissor）。
2. **scissor**：軸對齊的圓角矩形、`C` 超出外框但不碰四個角的方塊，且外框四邊落在裝置像素格上（同 T-659 的判定與容差）→ 以外框做 scissor（既有 `pushScissorRect`）。
3. **圓角影像直繪**：裁切層內只有一個 `DrawImage`／`DrawImageQuad`、裁切形狀是凸的（圓角矩形、橢圓）→ 不建裁切，以影像著色的凸形填色繪製（`fillConvex` 加貼圖座標，UV 由影像 quad 的仿射映射求得；沿用 fringe）。影像 quad 沒有蓋滿裁切形狀時，先與 quad 求交（兩個凸多邊形的交集仍為凸）。
4. **其餘**：改走既有的遮罩層路徑（`doPushMask`：內容畫進 transient，再以裁切形狀的覆蓋率作遮罩合成）——遮罩形狀由既有路徑繪製（凸形有 fringe、凹形走覆蓋率圖集），所以邊緣有抗鋸齒。巢狀裁切各自判定；內層若走遮罩層，外層的 stencil／scissor 照常作用於合成。
5. 4x（`edgeFringe` 關）完全不變。統計加 `clipSkipped`、`clipScissor`、`clipImageDirect`、`clipMaskLayers`。
6. 命中測試與佈局不受影響（只改繪製）。

**T-663 實作記錄（#632，2026-11-11）**：`xt::gpu::clipContentBounds`（`clip_bounds.{h,cpp}`；以 `xt::commandBounds` × dpr 外擴 2 px，圖層／遮罩／效果／投影取 `boundsHint`，空 hint 視為 Unknown）；frame builder `scanClip`（每個範圍每次 build 掃一次並記憶）、`clipAtOneX`、`clipImageDirect`／`fillConvexImage`、`pushClipMaskLayer`。安全內區推廣到任何凸的單輪廓形狀（包圍盒扣掉四個角的矩形，角的大小取形狀與各邊的切點）。保留 stencil 的情形：3D 投影下、Direct 模式中 Pop 落在之後的 run、`dryWalk` 期間、退化的旋轉矩形。1x 對 SW：`ui_rounded_clip_children` 256 → 0、`ui_background_image` 60 → 0、`ui_clip_path` 1064 → 427、新 fixture `ui_coverage_clip` 1570 → 787（餘下全在旋轉的 24 px 文字，與裁切無關）。Sherefox：board／investigate／result／debate 的立繪圓角裁切走情形 3（debate 每幀 draw 95.3 → 92.3）；只有 notes 多出 pass（3.6 → 5.7／幀）——兩個 `.g-text`（`overflow: hidden`＋`skewX`）其實文字沒超出，但字形包圍盒以「原點外擴一個字級」估計而偏大，落入情形 4。**發現**：`ui_coverage_clip` 單獨跑 787、整套跑 883——輸出依賴先前 fixture 的狀態，疑為 T-662 字形鍵含 shaper 指標的誤命中（T-675 修）。未編譯：Clang／GCC。

**T-664 實作記錄（#634，2026-11-11；一項裁決）**：盤點結果——1x 下已無任何邊框走 stencil（均勻環與 outline 走凸環 fringe、單邊走 collapsed ring、L 形與每邊不同色的圓角走覆蓋率圖集、直角不同色為四個凸梯形、dashed／dotted 為互不相碰的凸片），以測試鎖住。`ringSidesUneven`：同色環有任一邊 < 1 裝置像素且各邊寬度相差 > 0.25 px 時改走覆蓋率圖集（0.5／1／4／1 px 的環對 SW 692 → 0；旋轉 9° 431 → 149）；等寬薄環不變。`ui_game_hud` 的 133 個殘差像素：125 個是 3D 投影（`rotateY`）下的圓角裁切（T-663 規定投影下維持 stencil）、6 個是投影下的 fringe 圓角、2 個是光暈邊緣——都不屬 2D 路徑。`glyphRunInkBounds`（以字型 ascent／descent 收緊垂直範圍，水平以末字輪廓右緣加半個字級；24 組 run 的 SW 非零像素盒皆在回報盒內）。**未達**：Sherefox notes 的 `clipMaskLayers` 仍 0.53／幀——T-663 的掃描先在裝置空間取包圍盒、再映回裁切的本地座標取一次包圍盒，`skewX(-6deg)` 使兩側各多約 3.6 px。**裁決**：定案 5 的 `C` 改為「在裁切形狀的本地座標系中，各命令本地包圍盒（經命令自身對裁切的相對變換）的聯集」，不經裝置空間的軸對齊包圍盒；隨下一張碰裁切的單一併實作（T-676）。另記：等寬的次像素薄環（0.5／0.75 px）對 SW 仍有 296／400 個超差像素（2 px 的 fringe 足跡 vs SW 的實際覆蓋）——可比照不等寬者改走覆蓋率圖集，T-676 一併。未編譯：Clang／GCC。

**10.12 定案 2 的規格補充：`DrawBoxShadow` 的公式與判定（2026-11-11；T-668）**

*判定*（painter，`effect: shadow()` 的 drop shadow；全部成立才轉換，否則維持 PushEffect）：(1) 節點的效果鏈只有這一個 drop shadow（或多個 drop shadow——各自轉換；鏈中有其他種類的效果則不轉換）；(2) 節點自身不透明度為 1，且不是需要圖層的節點（混合模式、遮罩、3D 等）；(3) 節點有 alpha = 1 的純色或各色標 alpha 皆為 1 的漸層背景，背景繪製範圍是邊框盒（`background-clip: border-box`），圓角每個角的水平與垂直半徑相等（圓形角；四個角可各不相同）；(4) 節點是 `overflow` 非 visible，或其所有子孫（含文字、偽元素）的可繪包圍盒都落在邊框盒內——用保守的包圍盒，拿不準就不轉換；(5) 邊框若有，位於邊框盒內（本來就是）且不改變輪廓；`outline` 存在時不轉換（輪廓超出邊框盒）；(6) 陰影參數有限且模糊 ≥ 0。成立時輪廓＝邊框盒的圓角矩形 `R`（半徑 `r_tl, r_tr, r_br, r_bl`）。

*命令*：`DrawBoxShadow { rect, radii[4], offset, blur, spread, color }`，在節點自身內容之前發出；與現有 drop shadow 相同的座標空間與變換。`R'` = `R` 向外擴 `spread`（半徑各加 `spread`，下限 0）再平移 `offset`。

*覆蓋率*：令 `σ = blur / 2`（與現有高斯的定義一致）。`σ` 小於「現有路徑視為無模糊」的門檻時，覆蓋率 = `R'` 的解析覆蓋（以凸形填色＋fringe 繪製，或 SW 的光柵化器）。否則，對像素中心 `p`：

`S(p) = ∫ G(y − p.y; σ) · ½ · [ erf((x₁(y) − p.x) / (σ√2)) − erf((x₀(y) − p.x) / (σ√2)) ] dy`

其中 `[x₀(y), x₁(y)]` 是 `R'` 在高度 `y` 的水平範圍（圓角處由圓方程式求得，超出 `R'` 的 `y` 範圍為空）、`G` 是單位面積高斯。`y` 的積分以**固定 16 點的 Gauss–Legendre 求積**在區間 `[max(R'.top, p.y − 3σ), min(R'.bottom, p.y + 3σ)]` 上計算（區間為空則 `S = 0`）；`erf` 用 Abramowitz–Stegun 7.1.26 的有理近似（最大誤差 1.5e-7），GPU 與 SW 用同一組常數。輸出色 = `color`（預乘）× `S`。繪製範圍是 `R'` 外擴 `3σ` 的矩形（一個 quad；SW 逐像素計算）。這個公式是高斯與圓角矩形的真卷積（僅 y 向為數值積分），與現有「離屏＋降採樣高斯」的近似不同，因此轉換後的陰影像素會改變：SW golden 與 GPU golden 都要重產，重產的每一張由任務代理列出前後差異統計，主模型抽看確認是變得更接近真高斯而非出錯。

*兩個後端同源*：SW（`xt_sw`）與 `xt_gpu`（wgpu、GL 的片段著色器）都實作同一公式；GPU 對 SW 的 golden 容差不變。CE 的 Host 後端不在本倉（該倉另行跟進；命令是新增的，舊後端遇到未知命令的行為須在任務中確認並回報）。

*不轉換的情形維持原樣*，像素不變。

**1x 畫面回報的查證（2026-11-11；遊戲 session 以 code.3 並排比對 1x／4x，回報 title 大字階梯、debate 動畫立繪塊狀）**：兩者都不是 1x 的缺陷。title 的 140 px 標題在 1x 有 184 個不同的邊緣灰階、對 SW 平均誤差 0.63／超差 471 px；4x 只有 5 級、0.83／2071 px。debate 立繪在同一動畫幀上 1x 與 4x 相符（平均差 0.01～0.03），兩張截圖落在待機動畫的不同相位；可見的顆粒是 3.4× 縮小、單層貼圖的取樣鋸齒（三條路徑皆同）→ T-665 提前。**量測方法的三個教訓**：(1) 以第 4 幀量測會漏掉有進場淡入的內容（T-662 的 title 數字沒量到標題本身）——量測一律取進場動畫結束後的幀；(2) `xtframe-view` 一律加入系統字型，而 XTAtrium 不設 `system_fonts`，兩者字形不同（遊戲內粗體為合成粗體）——view 加 `--no-system-fonts`；(3) 並排比對必須是同一 tick 的同一幀，且以 SW 為基準算數字，不以 4x 的觀感為準。**仍存在的 1x 差異**：≤ 96 px 的旋轉／傾斜文字（T-653 的位圖重取樣路徑）比 SW 軟且略淡（SW 與 4x 是畫輪廓）——改走輪廓覆蓋率，T-676。

**T-677 實作記錄（#635，2026-11-11）**：「輸出依賴先前畫過的內容」的根因不在覆蓋率圖集，而在字形圖集：`GlyphAtlas::lookup` 的 `markDirty` 只上傳字形本身的矩形、不含它四周的 1 px 邊距；T-653 的旋轉／傾斜字形把 quad 與 UV 外擴半個 texel 到邊距上、3D 投影文字以線性取樣讀到邊距，兩者都假設裝置貼圖上的邊距為 0——但只有 CPU 頁保證。裝置頁在三種情況下不是 0：`setGlyphAtlas` 換了同尺寸的圖集而 frame builder 沿用裝置貼圖（`xt_golden` 每個 fixture 換一次；`Code/Backends` 的 renderer 在 shaper 更換時也會）、`GlyphAtlas::resetAll`（四頁全滿後重排）、GL 以 `nullptr` 建立的貼圖內容未定義。修法：髒矩形外擴 1 px（夾在頁內），只多上傳 0。影響：C ABI 宿主（renderer 與圖集同生共死、wgpu 新貼圖為 0）只在長時間執行填滿四頁字形圖集並 `resetAll` 之後才會出現（旋轉／傾斜／3D 文字邊緣帶到別的字形的殘墨）；v0.15.2 與 code.1～code.3 皆存在。被排除：`pathContentHash` 記憶（每次 build 清空）、字形覆蓋率鍵、分塊表頭與回收、頁成長後的 UV、漏傳、預算退回、效果快取跨文件。新增：`xt_golden --backend wgpu --order-pairs`、`--filter` 接受逗號與 `=` 全名、`Tools/Scripts/golden_order_check.sh`（整套 vs 逐一 vs 兩兩順序；版號單加跑）。已知未改：內容雜湊的查找只比鍵不比內容（64 位元碰撞理論上可能）。未編譯：Clang／GCC／Release；未測：Vulkan、實體 GL 驅動。

**T-668 實作記錄（#636，2026-11-11）**：`xt::judgeBoxShadow`（`box_shadow.{h,cpp}`，回傳第一個不成立的條件）；painter 照常先發 PushEffect，到該節點的 PopEffect 時以子樹實際發出的命令（`xt::commandBoundsInFrame`）判定，成立則把 PushEffect 就地換成 `DrawBoxShadow`（不轉換的節點輸出逐位元組不變）。SW 與 GPU（wgpu、GL 皆完成）同公式；SW 的列共用路徑與逐像素公式浮點相同。對 64×64 超採樣暴力卷積的最大誤差 0.5～1.16／255（舊的離屏路徑在同樣情形為 2.6～29.3）；門檻以下（`boxBlurWidth(σ) ≤ 1`，即 σ < √2 裝置像素）與 FillPath 逐位元組相同。重產的既有 golden 只有 `ui_fx_shadow`（對舊圖 3 px 超差、對參考誤差 2.62 → 0.67）。解讀採用：inset shadow 視為其他種類的效果；多個 drop shadow 由後往前發出；負的 spread 取 0；公式在節點本地空間求值（skew／非均勻縮放下模糊在螢幕上各向異性，與離屏路徑的螢幕空間各向同性不同）；3D 投影下不發此命令。**Sherefox 的轉換率偏低**：debate 32 個陰影轉換 4 個（未轉換：效果鏈含其他效果 4、背景半透明 alpha 0.90～0.92 共 6、outline 1（即 `litpulse` 節點）、內容超出 17——其中 8 個只是字形包圍盒估得太粗、9 個是真的幾何超出）；board 25 轉 7、investigate 7 轉 1；debate passes 18.78 → 17.75。**兩個相容問題**：BakedWriter 不認得 `DrawBoxShadow`（`xtframe-bake` 遇到可轉換陰影即失敗，除非 `--simplify`）；沒有實作此命令的宿主後端會什麼都不畫（陰影消失）。未編譯：Clang／GCC。

**10.12 定案 2 補充 2：相容開關與判定放寬（2026-11-11；T-680）**

*A. 相容*：`Context`（或 painter 的選項，擇既有選項結構）新增 `boxShadowCommands`，**預設開**。`xtframe-bake` 與任何以 BakedWriter 輸出 `.xtcl` 的路徑在繪製時關閉它（輸出與 T-668 之前相同，烘焙不再失敗）；`.xtcl` 格式不動。後端能力：`Renderer`／後端介面若有能力查詢點則加一項「支援 `DrawBoxShadow`」，由 Context 在綁定不支援的 renderer 時自動關閉；沒有這種查詢點就只做選項，並在 23 章的宿主整合記錄（實作記錄由主模型補）註明自訂後端須實作此命令或關閉選項。

*B. 放寬*（判定仍須讓解析式結果與離屏結果在 golden 容差內一致；每項各有 fixture）：
1. **不透明 outline**：`outline` 為實線、色彩 alpha = 1 時不再拒絕。輪廓＝邊框盒 `R` ∪ outline 環；outline 環的外緣 `O_out`（`R` 外擴 `offset + width`）與內緣 `O_in`（`R` 外擴 `offset`）都是圓角矩形。卷積是線性的：`offset ≤ 0`（環與盒相接或重疊）→ 陰影＝`S(O_out)`；`offset > 0`（有縫）→ 陰影＝`S(O_out) − S(O_in) + S(R)`。`DrawBoxShadow` 擴充為最多三個帶正負號的圓角矩形項（同一 quad、同一片段著色器內加總後夾在 0..1），SW 同式。虛線／點線 outline、半透明 outline 仍拒絕。
2. **文字包圍盒**：條件 4 對字形命令改用字型度量收緊（ascent／descent 與末字輪廓右緣，同 T-664 的 `glyphRunInkBounds` 的規則；把該函式下沉到 core 可用的位置供 painter 與 GPU 共用，不複製兩份）。
3. **高不透明度背景**：背景 alpha `a ≥ 0.9`（純色，或漸層各色標 alpha 皆 ≥ 0.9 且相等）時視為均勻 alpha 的方塊，陰影＝`a · S(·)`。這是近似（子內容的不透明部分本應讓陰影局部更深，誤差上界為 `(1 − a) ×` 陰影色 alpha，且多半被盒本身遮住）；`a < 0.9` 仍拒絕。fixture 必須包含「文字貼邊＋位移陰影」的最壞情形並回報與離屏路徑的最大差。
4. 仍拒絕：效果鏈含其他種類的效果、內容的幾何真的超出邊框盒、橢圓角、3D。

**T-676 實作記錄（#638，2026-11-11；兩項裁決）**：`xtframe-view --no-system-fonts`。旋轉／傾斜的 ≤ 96 px 文字在 1x 改走輪廓覆蓋率（`outlineGlyphCoverage`／`doTransformedRun`；覆蓋率被拒的個別字形退回 T-653 的圖集路徑並要求補畫）：`ui_text_transform_skew` 1972 → 823、`ui_coverage_clip` 787 → 369；Sherefox（`--no-system-fonts`、720p、第 400 幀）notes 2576 → 1、result 5157 → 2178、evbook 4910 → 1807。等寬次像素薄環走圖集：0.5 px 圓角 296 → 0、0.75 px 直角 400 → 0。裁切包圍盒改在裁切的本地座標求聯集（`clipContentBoundsInFrame`）。`golden_order_check.sh` 通過（179 fixture、56 對）。

*paper 的查證*：標題是 40 裝置像素、`font-weight: 700`（bundle 只有 Regular → 合成粗體）、無變換，落在 24～96 px 帶：GPU 用 hinted 字形圖集（`FT_LOAD_TARGET_LIGHT`、hint 後加粗、基線取 `round(y)`、gamma 1.2），SW 填未 hint 的輪廓（線性覆蓋率、精確原點）。主因是**基線取整**（271.52 → 272，位移 0.48 px），其次是 light hinting 改變橫筆位置（字腔不同），gamma 使邊緣約淡 3%；合成粗體兩邊強度相同，不是原因。把圖集上限暫時由 96 降到 24 的實驗：paper 8022 → 1166（最大 234 → 52）、title 2643 → 2361、result 5157 → 3837；覆蓋率圖集在 paper 由 7 項增為 91 項、一頁由 512² 長到 1024²、無回收。

**裁決 1（T-681）**：1x（`edgeFringe` 開）下，GPU 的文字與 SW 用同一個門檻——字級 ≥ 24 裝置像素（SW 改填輪廓的同一條件，以 SW 的判斷式為準）的 run 走輪廓覆蓋率；< 24 px 維持 hinted 字形圖集（SW 在此也用 hinted 位圖）。4x 不變。T-681 必須量 CJK 密集畫面（Sherefox vn 的對話、paper 的內文）的圖集項數、頁數與回收次數；若出現持續回收，回報由主模型決定圖集頁數上限或文字鍵的次像素量化。

**裁決 2（T-681）**：notes 的遮罩層仍在（`clipMaskLayers` 0.57／幀）不是座標系的問題，而是 `glyphRunInkBounds` 的估計規則太寬（左側加半個字級、垂直取 ascent＋descent）。改為逐字形的實際 ink 盒：以 shaper 取得每個字形輪廓的控制點包圍盒（或字形度量的 bearing／尺寸），經變換後聯集、外擴 1 裝置像素；無輪廓的字形（點陣字、emoji）以其位圖度量；拿不到時才退回今天的估計。此函式由 T-680 下沉到 core 後在該處修改。

**10.11 定案 7 定稿：縮小取樣的規格（2026-11-11；T-665；任務單 `Docs/tasks/T-665.md` 的規格段，加上兩項開工前裁決）**：LOD 由 CPU 以仿射映射的線性部分明確算出（`ρ` = 每個裝置像素在 x、y 方向跨過的 texel 數取大者，`lod = max(0, log2 ρ)`），兩個 GPU 後端與 SW 同式；`lod = 0` 逐位元組不變；`lod > 0` 為兩層雙線性的線性混合。取代原定案 7 的「GPU ≥ 1.5×、SW ≥ 2×」門檻。**裁決 1（層尺寸）**：採硬體 mip 尺寸 `max(1, w >> k)`，下一層為上一層 (2x, 2y)～(2x+1, 2y+1) 在預乘空間的 2×2 box；奇數邊的最後一行／列不取樣（非 2 的冪的影像在低層有次 texel 的錯位，為已知限制；是否值得做 2×3／3×3 足跡的降採樣待實測）。**裁決 2（CPU 鏈的歸屬）**：`AssetResolver` 新增虛擬介面（預設回「無」）；`FileAssetResolver` 把鏈存在解碼項旁（延遲產生、隨項釋放、計入閒置預算）；resolver 回「無」時由 SW renderer 自己建並持有，鍵與失效規則比照 GPU frame builder 對借用影像的做法（view 指標＋asset epoch／source；`invalidateAsset`、source 釋放、shutdown／trim 時丟棄），不得是沒有失效的裸指標快取。BC 壓縮且無現成 mip 鏈的影像 GPU 維持單層，SW 對該影像也維持單層（「SW 與 GPU 取樣相同的層」）。未共用的貼圖同樣延遲產生 mip；`DrawImageQuad` 僅在裝置 quad 為平行四邊形時有 lod。

**T-680 實作記錄（#639，2026-11-11；一項裁決）**：A——`Context::setBoxShadowCommands(bool)`（預設開，經 `Document::paintAs` 傳到 painter）；`xtframe-bake` 關閉它（四個場景的 `.xtcl` 與 T-668 之前的工具逐位元組相同），`xtframe-view --no-box-shadow`；BakedWriter 仍拒絕 `DrawBoxShadow`。本倉的 renderer（`SwRenderer`、經 `FrameBuilder` 的 wgpu／GL）皆已實作；宿主自訂的 `GpuDevice` 看不到命令，只會收到 `Shading::BoxShadow` 的繪製，須實作該著色或關閉選項（Context 不綁定 renderer，無法自動關閉）。B.1——`DrawBoxShadow` 加 `extraCount`＋`BoxShadowTerm extra[2]`（矩形、半徑、正負號）；outline `offset ≤ 0` 為單項（`O_out`，或環完全在盒內時為 `R`），`offset > 0` 為 `S(O_out) − S(O_in) + S(R)`，spread 使 `O_out` 與 `R` 外擴、`O_in` 內縮（縫合上時該對項消去）；節點自己的 outline 不計入條件 4；對暴力卷積最大誤差 0.84～1.19／255。B.2——`glyphRunInkBounds` 下沉到 core（`xt/damage.h`）。Sherefox 1440p 1x（第 300～600 幀平均）：debate 轉換 4 → 12／32，passes 13.85 → 4.99、效果快取未命中 1.00 → 0.01、`litpulse` 的離屏 pass 約 8.9 → 0／幀；board 7 → 11／25、investigate 1 → 5／7；board 與 investigate 的 draw 增加（41 → 57、23 → 38，陰影與內容分開畫）而 pass 不變。重產：`ui_box_shadow_kept`（其 outline 節點現在會轉換）。**B.3（背景 alpha ≥ 0.9）未併入**：最壞格與離屏路徑最大差 17（> 16 的 12 px），但同一格在不透明背景下與離屏路徑也差 17——差距多半來自離屏路徑自身的近似。**裁決**：停損條件的基準訂錯了；改以真值（實際內容 alpha——半透明背景與其上的不透明子內容——的精確高斯卷積，測試內暴力計算）為基準：解析式（含 B.3）對真值的最大誤差與 > 8 的像素數皆不高於離屏路徑對真值者，就併入 B.3；否則不併。由 T-681 執行（patch 在 `C:\Users\Bigspring\.claude\jobs\dccd3382\tmp\t680\b3_only.patch`）。未編譯：Clang／GCC。

**T-681 實作記錄（#640，2026-11-11）**：A——`xt::glyphRunUsesHintedBitmap(cmd, deviceXf)`（`glyphRunBitmapApplies && glyphRunBitmapSize < 24`；SW 與 GPU 共用，SW 輸出不變）；1x 下被它拒絕的軸對齊 run 逐字形走 `outlineGlyphCoverage`，被拒者退回字形圖集並補畫；縮放中的 run 與 3D 投影維持原路徑。Sherefox（720p、`--no-system-fonts`、第 400 幀）對 SW：paper 8022 → 1166（最大 234 → 52）、result 2178 → 858、evbook 1807 → 1527；wgpu golden 有 19 個 fixture 的數字改變，全部變好或持平（`ui_text_cjk` 1384 → 7、`ui_text_wrap` 907 → 124、`ui_text_decoration` 1929 → 616、`ui_ime_composing` 472 → 3；`ui_text_vertical` 561 → 615 為唯一略增者，最大差 144 → 51）。圖集壓力（1800 幀）：四個畫面皆無回收；vn 1440p 每行 CJK 對話約增 55 項、14 行後用掉 4 頁中的 3 頁（更長的對話會開始整頁回收舊行）。B——逐字形的輪廓控制點盒（hinted 位圖的 run 另聯集位圖盒，否則 16 px CJK 的 hinted 像素會落在盒外），外擴 1 裝置像素；notes 的 `clipMaskLayers` 0.50 → 0、畫面逐位元組不變；方塊陰影轉換 debate 12 → 13、board 11 → 13。C——B.3 以真值判定：五格的解析式誤差皆不高於離屏路徑（最大／> 8 的像素：a92text 8.79／16 對 15.47／674；textEdge 14.23／313 對 32.13／784；childEdge 25.98／395 對 32.62／928），保留；debate 轉換 19／32。重產 SW golden：`ui_box_shadow_relaxed`、`ui_game_hud`（5387 px、最大 1）。**待修（T-682）**：穩態 build ms 上升（evbook 1440p 0.14 → 0.43、notes 0.07 → 0.21）——每個字形的覆蓋率查找都先取輪廓並展平；違反定案 9「靜態畫面不得高於修改前」。未編譯：Clang／GCC；未測：Vulkan。

**T-665 實作記錄（#641，2026-11-11）**：`xt/image_mips.h`（LOD 公式、CPU 鏈；`s/4` 取最近、平手取偶，GPU 鏈才能在深層維持 1 階內）、`sw_mip_cache`（來源＝resolver＋IR 載入序號，只留最近 2 個來源；新增 `SwRenderer::invalidateAsset／invalidateAllAssets`）、wgpu `wgpu_mips.cpp`＋`texture_lod.wgsl`（請求當下以自己的 submit 產生，該幀即可用）、GL（延到下一次 submit 開頭產生）。對 16×16 面積積分參考的平均誤差（0～255）：0.5× 0.185 → 0.185（恰為 2:1 時單次雙線性本來就落在 2×2 box 上）、0.33× 38.51 → 8.00、0.2× 112.16 → 7.36。閃爍量（相鄰幀平均絕對差）：單元測試 57.2 → 11.1；Sherefox debate 720p 待機動畫的變動區 0.181 → 0.090。既有 golden 無一改變（既有 fixture 的影像都 ≤ 24 px 且 1:1 或放大）；新 fixture `ui_image_minify`。Sherefox：`image_bytes` debate 720p 33.6 → 44.8 MB（5 條鏈、CPU 22.9 ms 一次性）、vn 27.3 → 36.4；**SW 每幀時間上升至多 21%**（debate 720p 36.1 → 43.6 ms，幾乎都在 `sw:spanImg`）——SW 是基準與後備路徑，接受，記 PENDING（SIMD 化三線性）。已知限制：奇數邊最後一行／列不進下一層（129×129 末列紅線在第 1 層完全消失）；GL 不上傳 DDS 內存的 BC mip 層（GL 上 BC 影像單層、SW 三線性，兩者不同）；裝置不支援 BC 而退回 RGBA 時 GPU 有鏈、SW 視為單層；圖集頁的格在高層互相滲色（SW 與 GPU 相同）。**另記（非本單造成）**：1440p 下 GPU 對 SW 的超差像素很大且前後不變（debate 43,673、investigate 12,506、vn 9,039；720p 僅 172／8／0）——量測基底早於 T-681，待 T-681 之後重量再判斷。未編譯：Linux／macOS；未測：Vulkan、Metal、帶 mip 的 BC DDS。

**T-682 實作記錄（#642，2026-11-11；兩項裁決）**：`glyphCoverage` 先組鍵查圖集，命中不呼叫 shaper、不配置；未命中才 `fetchGlyphOutline`；`glyphOutlineNotes`（無輪廓／空輪廓的負向快取，4096 項滿即清）；每個 run 的變換與雜湊種子只算一次；`fillCoverage` 拆為 `coverageLookup`／`coverageRasterize`／`coverageDraw`。強迫每幀全重建時的 build ms（c4091d87／T-681 後／本單）：evbook 1440p 0.104／0.340／0.117、notes 1440p 0.075／0.211／0.074、paper 1440p 0.041／0.461／0.086、title 1440p 0.058／0.263／0.085、debate 1440p 0.228／0.866／0.468、debate 720p 0.206／0.387／0.375。在正常的 damage 追蹤下，除 debate 外各畫面在穩定後根本不重建（0 ms）。**裁決 1**：1440p 下 paper／title／vn／debate 仍略高於 T-681 之前（每個覆蓋率 quad 命中約 0.1 µs；以前是每個 run 一個字形圖集批次）——差距為次毫秒且只在全重建時出現，接受；定案 9 的「不得高於修改前」對此以「同一數量級、< 0.3 ms」解讀。debate 的進一步下降靠 10 §10.12 的 GPU 局部重繪。**裁決 2**：1440p 下 GPU 對 SW 的超差像素（T-681 之後：debate 17,372／最大 71、vn 4,220、investigate 1,912；720p 為 172／0／8）集中在 ≥ 24 裝置像素的 CJK 字形邊緣，來源是覆蓋率鍵的 1/4 px 位移量化（GPU 位置誤差 ≤ 1/8 px，SW 用精確位置）——屬位置量化而非缺邊，肉眼不可辨，接受；把預設改 1x（T-666）的驗收以「對 SW 的平均絕對誤差與最大通道差」並列，不以超差像素數單獨判定。vn 1440p 連續 60 行 CJK 對話：項數峰值 1548、第 23 行用滿 4 頁、之後共回收 3 次（每次約 460 項），回收的那次 build 0.37～0.54 ms、無可量測的重新光柵化尖峰、無預算退回——4 頁足夠。**待辦**：`glyphRunInkBounds`（core）在每次掃描文字周圍的裁切時都向 shaper 取輪廓（notes 每次 build 74 次）——按 run 記憶，記 PENDING。未編譯：Clang／GCC。

