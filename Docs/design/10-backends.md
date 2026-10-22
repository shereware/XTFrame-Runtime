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
