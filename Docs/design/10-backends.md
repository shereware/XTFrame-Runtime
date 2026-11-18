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

**code.5 的同幀比對（2026-11-11；遊戲 session，`xtatrium shot`、1280×720、RTX 4070 Ti；`E:\coding4\sherefox-aa0.15.2-code.5
eport.json`）**——對 SW 的「> 16 像素數／最大通道差／平均絕對誤差」，1x｜4x：title 0／1／0.006｜0／10／0.032；paper（進場中）2189／37／0.257｜14013／195／1.009；paper_settled 8350／51／0.283｜10896／176／0.574；vn 0／7／0.217｜10／43／0.217；investigate 2944／53／0.329｜5725／102／0.438；board 0／16／0.070｜663／91／0.087；debate 289／41／0.192｜949／142／0.226；result 22／21／0.053｜1920／81／0.096。每個畫面 1x 的三項指標皆不高於 4x。paper_settled 的 8350 全在定格後 50～60 px 的標題字邊緣（24～96 帶的輪廓覆蓋率，1/4 px 位置量化；字形位置與 SW 一致、肉眼不可辨）——同 T-682 裁決 2。立繪的縮小取樣顆粒感消失（比以前略軟，遊戲 session 認為可接受）。據此執行定案 8（T-666）。

**T-666 實作記錄（#644，2026-11-12）**：`xt_gpu_target_desc.sample_count` 的 0（含較短的舊 desc）＝1；錯誤訊息改為 `(allowed: 0 = 1, 1, 4)`；`xtframe-view --msaa` 與 `BackendOptions::sampleCount` 預設 4 → 1。原本以 0 取得 4x 的測試經共用 helper 改為明確傳 4（`capi gpu` 14 個案例、`capi trim`、backends smoke 與 view 截圖測試），4x 覆蓋不減；新增 `capi gpu 25`（0 與 1 的 `transient_bytes` 與 readback 相同、小於 4）。本來就是 1 的：`WgpuInit`／`WgpuHeadlessOptions`、`WgpuDevice`、GL renderer、gpu-probe、Android。**未隨之改變**：編輯器的 GL／wgpu 預覽元件各自寫死 `kSamples = 4`（`gl_preview_widget.cpp:56`、`wgpu_preview_widget.cpp:62`）——預覽與出貨預設不同像素，T-684 改為 1；`xt::shell::WindowConfig::msaa` 預設仍 4（view 一律覆寫）；`CHANGELOG.md`／`README.md` 對 0.15.2 的敘述留到版號單。未編譯：Clang／GCC、Linux／macOS。

**T-669 的結果與裁決（2026-11-12；不合併）**：實作完成（`xt::judgeOpacityFlatten`／`applyOpacityFlatten`，painter 在 PopLayer 以實際發出的命令判定；分支 `task/T-669` 707cd159 保留）。量測：Sherefox 11 個 UI 只有 3 個「純因不透明度而建」的圖層（debate 的 `konju`、`shisshu`，result 的 `res-stamp`），三個都因子項的繪製範圍互相重疊而不可攤平——0／3，passes、draw、build ms 前後相同；其餘 8 個 UI 沒有這類圖層。另發現規格未涵蓋的情形：同一個文字 run 內相鄰字形的抗鋸齒像素會重疊，逐字形乘 alpha 與「先合成到圖層再乘 alpha」差到 2 階（新 fixture 上 2 px）。**裁決**：定案 3 的前提（debate 的兩個 0.22 圖層可攤平）不成立；對目標遊戲沒有效益，卻多一條 painter 路徑與一個選項——不合併，定案 3 撤回。日後若有實際 UI 出現大量「互不重疊的半透明圖示列」再取用該分支（屆時含文字的群組應排除）。debate 穩態已是 5 個 pass，剩餘的 pass 來自真正需要圖層的內容。

## 10.13 打包時的 BC7 影像（2026-11-12 定案；使用者：「好」；調查 `Docs/reviews/2026-11-12-bc7-at-pack-time.md`；T-686、T-687）

**現況**：端到端已存在——`xtframe-pack --compress bc7` 把點陣影像寫成 DX10 DDS（`BC7_UNORM`、straight alpha）、IR 引用改名、resolver 以內容辨識 DDS 並保留存放的 mip 層（T-665）、wgpu 與 GL 直接上傳區塊、SW 以 bc7decomp 解第 0 層、無 BC 的裝置退回 RGBA8；編碼器與解碼器皆已 vendored。宿主（XTAtrium）只是以子行程呼叫 `xtframe-pack`。**但今天對 Sherefox 開啟它會讓 CPU 記憶體變多而不是變少**，缺口如下，本節逐一定案。

**量測（Sherefox，48 張、59.0 Mpx）**：編碼 86 s（含 mip 123 s，單執行緒，約 0.65 Mpx/s）；磁碟 34.3 MB → 78.7 MB（含 mip；zlib 約 59.6 MB）；品質：背景 RGB 43～49 dB、立繪 RGB 45～50 dB／alpha 42～46.5 dB（誤差 > 32 的像素 0.02～0.06%，都在 alpha 邊緣）、44 px 圖示與 640 px 小圖的 alpha 僅 28～35 dB（至多 2.3% 像素 > 32）。GPU 影像記憶體：第 0 層 225 MiB → 56 MiB（全部可用 BC 時），含 mip 約 75 MiB。

**定案**：

1. **GPU 路徑不解成 RGBA（T-686）**：resolver 對 DDS 只保留壓縮區塊；RGBA 只在有人呼叫 `imageView`（SW、命中測試等需要像素者）時才解，並照既有的閒置預算管理。壓縮區塊在上傳後的去留比照 T-657（隱藏時釋放到閒置表；可見時保留，供裝置重建）。
2. **尺寸不是 4 的倍數（T-686＋T-687）**：打包器把影像補邊到 4 的倍數（複製邊緣 texel），DDS 標頭的寬高是補邊後的值；**邏輯尺寸**寫在 `DDS_HEADER.dwReserved1[0..2]`＝`'XTLS'`（0x534C5458）、邏輯寬、邏輯高。resolver 讀到此標記時，`imageSizeFromHeader` 與佈局用邏輯尺寸，繪製時 UV 乘上（邏輯／補邊）；沒有標記的 DDS 維持今天的行為（wgpu 對非 4 倍數者退回 RGBA）。mip 鏈由補邊後的影像產生。
3. **mip 鏈（T-687 產生、T-686 使用）**：只要啟用壓縮就寫完整鏈（不再綁 `publish.atlas.mipmaps`）；尺寸為 `max(1, w >> k)`；每層＝上一層 2×2 的 **alpha 加權平均**（色彩以 alpha 為權重、alpha 取算術平均；等價於 T-665 在預乘空間的 box 後再還原為 straight），平手取偶。GL 也上傳存放的 BC mip 層；**SW 對帶鏈的 DDS 使用存放的各層**（逐層解碼，不自建鏈），使 SW、wgpu、GL 取樣相同的資料。
4. **alpha**：維持 straight（既有的 `texture_straight` 著色與 SW 的 straight 取樣算術一致）；打包器在編碼前對 alpha = 0 的 texel **補色**（由最近的不透明鄰居擴散，至少涵蓋 4 px，並延伸到 mip 各層的產生），消除雙線性取樣在透明邊緣帶入的雜色。不採「檔內預乘」（要動編碼器、標頭、`CompressedImage`、frame builder 與 SW，效益與補色重疊）。
5. **設定（T-687）**：`.xtui` 的 `publish.images.compress`：`none`（預設，行為同今天）｜`bc7`｜`auto`；CLI `--compress` 保留並可覆寫。`auto`＝兩邊皆 ≥ 256 px 的影像用 BC7，其餘不壓縮（小圖省不了多少記憶體，品質卻較差）；單一資產仍可用既有的 `assets.yaml compress:` 覆寫。圖集頁照頁的尺寸套同一規則。
6. **打包速度（T-687）**：編碼以多執行緒進行（工具內可用 `std::thread`；runtime 不受影響）；轉檔結果以「來源內容雜湊＋編碼設定＋編碼器版本」為鍵快取在 bundle 旁的快取目錄（預設 `<bundle>/../.xtframe-cache/bc/`，可用 `--cache-dir` 指定、`--no-cache` 關閉），未變更的影像不重編。
7. **測試**：BC7 fixture 的 GPU 對 SW golden（含帶 alpha、非 4 倍數、縮小繪製用到 mip 的情形）；「打包 → 載入 → 繪製」的端到端測試。
8. **平台**：D3D12、Vulkan 桌面、macOS 的 Metal、GL 4.2 有 BC7；iOS、Android／GLES 沒有——退回由 BC7 解出的 RGBA8（有損且不省記憶體），ASTC／ETC2 不在本節。宿主自建的 wgpu 裝置須要求 `TextureCompressionBC`（C ABI 目標自己的裝置已要求）。
9. **不做**：容器層壓縮（`.xtb` 的 zip 仍為 store 模式；另案）；BC1／BC3 的新設定入口（既有的覆寫仍可用）。

**2026-11-14 修訂（22 §22.17，T-714）**：第 5 點的「預設 `none`」只剩函式庫與開發期自動打包適用；三個發佈入口（編輯器 Publish、`project.publish`、`xtframe-pack`）在專案沒有寫 `publish.images.compress` 時採 `auto`。編輯器的 Publish 對話框提供這項設定與圖集、字型的設定。

**T-684 實作記錄（#645，2026-11-12）**：編輯器 `GlPreviewWidget`（單取樣 FBO，`GlRenderer::resize()` 讀到 `GL_SAMPLES` = 0 而開啟 fringe；無解析 FBO、`glReadPixels` 直接讀渲染目標）與 `WgpuPreviewWidget`（直接畫進預覽紋理）改為 1x；`--software-preview` 不變。逐字形輪廓盒快取：每執行緒的排序向量，鍵（shaper 世代、FontId、glyph id），存未變換的 em 單位控制點盒或「無輪廓」，上限 4096、滿即清；`xt::GlyphInkMemo`：一次 build／paint 內按 run 記憶（另比對變換、字型、字級、`text-rendering`、字形範圍、effectRadius、dpr、世代），`FrameBuilder::buildOnce` 與 `Painter::paint` 開頭以 epoch 重設。Sherefox notes 1440p 每次 build 的 shaper 輪廓呼叫 74 → 0；15 張截圖逐位元組不變。`xt::shell::WindowConfig::msaa` 預設 4 → 0（`Examples/desktop_app`、`lottie_player` 因此取得非多重取樣視窗）。editor_qt 只跑了預覽相關的六個檔（39＋18 案例）。未編譯：Clang／GCC；未跑：editor_qt 全套。

**T-686 實作記錄（#647，2026-11-12）**：`FileAssetResolver::decodeInto` 對 BC1／BC3／BC7 的 DDS 只保留區塊（含存放的各層）；`pixelsOf()` 在第一次需要時才解第 0 層（包成單層 DDS 交給既有的解碼函式，`xt_core` 仍無第三方）、裁成邏輯尺寸。會觸發 RGBA 解碼的只剩：`imageView()`／`decodeImage()` 的呼叫者（SW；frame builder 在 `createTexture` 失敗時的退回——裝置無 BC，或 wgpu 拒絕非 4 倍數的尺寸）與 `external_media.cpp:355` 的序列預載（未改，文件未涵蓋）。邏輯尺寸：`parseDdsHeader` 讀 `dwReserved1[0..2]`，有效條件 0 < 邏輯 ≤ 存放且差 < 4；用於 `imageSizeFromHeader`、`CompressedImage`、內容鍵、SW 的裁切、frame builder 的 `uvScale`。GL：`requestMipChain` 逐層 `glCompressedTexImage2D`。SW：帶鏈的 DDS 逐層解碼存放的層、以 straight 取樣後預乘（與 GPU 的 straight BC 路徑同算術），不自建鏈。golden `ui_image_bc7`：wgpu 與 GL 對 SW 皆 0 px 超差、最大差 2。Sherefox（以現有打包器 `--compress bc7`＋`publish.atlas.mipmaps`；宿主流程、1080p、1x）：首顯首幀 vn 97.2 → 18.3 ms（RTX）／98.9 → 19.8（UHD）、debate 81.2 → 33.3／101.2 → 49.9、title 30.2 → 11.0；`image_bytes`（第 3 輪末）61.8 → 19.5 MiB；private RTX 603 → 537、UHD 606 → 462 MB；DXGI local 279 → 212／389 → 252 MB；resolver 的資料量（由檔案計算）225 → 75 MiB。44 張 BC7 影像有 12 張尺寸非 4 的倍數，T-687 補邊前 wgpu 仍對它們退回 RGBA。SW 每幀時間 vn 99.6 → 103.9 ms（第 0 幀 163.5 → 93.3）。附記：capi 測試套件在 Debug 需約 8 分鐘，超過 `dev_check` 的 600 s 上限而被標為 TIMEOUT（單獨跑 73／73 通過）——版號單前調整上限或拆套件。未編譯：Linux／macOS；未測：Vulkan、Metal、GLES。

**T-687 實作記錄（#648，2026-11-12）**：`.xtui` 的 `"publish": {"images": {"compress": "none"｜"bc7"｜"auto"}}`（預設 `none`；其他值為載入錯誤；往返保留）；CLI `--compress bc7|bc3|bc1|auto|none` 覆寫、`--no-cache`、`--cache-dir`；`PackOptions::compress` 預設空字串＝取專案設定（編輯器發佈等呼叫者也跟著設定走）。宿主以子行程呼叫 `xtframe-pack` 者不需傳旗標，在 `.xtui` 設定即可。補邊＋'XTLS'、完整 mip 鏈（整塊 alpha 為 0 的 2×2 取四個 texel 的平均）、透明補色（8 鄰域逐圈擴散、每層由補色後的上一層產生並再補色）套用於打包器寫出的所有 DDS（含 BC1／BC3 覆寫）；`auto` 下 `@2x／@3x` 跟隨 1x 的決定。快取鍵＝解碼後 RGBA 的 FNV-1a 64＋尺寸＋格式＋編碼器版本字串。`none` 的輸出與修改前逐位元組相同（僅 `builtAt` 與 `.xtir` 的編譯時間不同，同一工具兩次執行亦然）；多執行緒＝單執行緒＝快取，逐位元組相同。Sherefox（`auto`、13600K、20 執行緒）：首次編碼 10.8 s、全快取 2.7 s、改一張圖 2.6 s（修改前單執行緒 86 s／含 mip 123 s）；磁碟 47.7 → 92.2 MB；33 張走 BC7（11 張補邊）、11 張小圖不壓。PSNR：背景 RGB 43.2～49.1 dB、立繪 RGB 41.4～44.9／alpha 42.1～46.4；最差的是 640 px 的印章類（RGB 27.8～34.6、alpha 27.8～31.0）——屬 ≥ 256 px 而被 `auto` 納入，可用 `assets.yaml compress: none` 個別覆寫。執行時（宿主流程、1080p、1x；`none` → `auto`）：`image_bytes` 61.8 → 19.5 MiB；private RTX 621 → 473、UHD 653 → 434 MB；DXGI local 321 → 187／452 → 244 MB；首顯首幀 vn 124.8 → 16.3 ms（RTX）／108.2 → 19.3（UHD）、debate 99.8 → 42.4／102.5 → 54.1、paper 39.2 → 11.9／60.2 → 14.3。端到端（打包 → 載入 → wgpu 對 SW）三種設定皆 0 px 超差。已知：不透明的平坦區塊經 bc7enc mode 6 解出 alpha 254；`xtframe-view` 自動打包 UI Project 時會照專案設定編碼（首次較慢，之後走快取）——維持，預覽與出貨一致。未編譯：Linux／macOS（新增 `Threads::Threads`）；未測：Vulkan、Metal、GL 端到端。

**T-692 記錄（#649，2026-11-12；輕量閘門沒跑到的失敗與一個缺陷）**：(1) `xt_perf` T-643「effects >= 11」得 9——方塊陰影轉換所致（`effects_static.xt` 的 `#shadow`、`#spread` 被轉換）；該案例改以 `setBoxShadowCommands(false)` 建 Context（保留 ≥ 11 個真正的效果層，涵蓋關閉轉換的宿主仍會走的離屏路徑）。(2) **缺陷**：`Painter::finishBoxShadowCandidate` 每次轉換都預設建構一個區域 `std::vector`，MSVC Debug 的迭代器除錯會為它配置 16 B——每個被轉換的陰影每幀一次，違反 14 §14.4；改為成員 `m_boxShadowCmdScratch`，輸出不變；新增案例斷言含轉換陰影的畫面 10 幀 update＋render 與 100 幀 SW 皆零配置。(3) `sym_TouchButton_cooling` 的 4046 px 差異**不是**方塊陰影（該按鈕沒有陰影），而是 T-665：冷卻的 `#Shade` 以 4×4 的點陣圖鋪底、垂直縮小 1.67×（lod 0.74），現在走三線性；回溯確認 #641 之前通過、之後失敗；重產該期望圖（色調平均不變、網點對比由標準差 17.4 降為 11.4）。(4) `Tests/import/test_dc_effects.cpp` F14 的期望改為 `DrawBoxShadow`。之後 CTest 151 項（不含編輯器與網路類）全過。`dev_check.sh` 的說明加註：改動效果、陰影、影像取樣或 painter 輸出時另跑 `perf` 與 `symbols`。未跑：編輯器與網路類套件、Release 的 `xt_perf`；未編譯：Clang／GCC。

## 10.14 裝置能力查詢：沒跟上的 `GpuDevice` 不會畫錯（2026-11-12 定案；23 章「宿主自訂 `GpuDevice` 後端的升級須知」第 2、3 點的解法；T-691）

**問題**：10.11／10.12 讓 frame builder 發出兩種舊裝置看不懂的 draw——帶 `coverage` 貼圖的 quad、`Shading::BoxShadow`。自訂 `GpuDevice` 的宿主升級後編得過，但前者被塗成實心、後者漏畫。

**原則**：新的 draw 形式一律由裝置明說「我會」才發；預設值是「不會」。沒跟上的裝置拿到的 frame 與它上一次能正確畫出的形式相同，品質較低但不出錯。

1. `GpuDevice` 追加兩個帶預設實作的虛擬函式（附加在介面尾端，不動既有順序）：
   - `virtual bool supportsCoverageDraws() const { return false; }`——裝置會把 `DrawCmd::coverage` 當 R8 覆蓋率貼圖與著色結果相乘。
   - `virtual bool supportsBoxShadowShading() const { return false; }`——裝置實作 `Shading::BoxShadow`（含 `boxShadowMulti`）。
   `xt_wgpu`、`xt_gl` 兩者皆回 true（它們本來就實作）。`FrameBuilder::build` 每次 build 問一次。
2. **覆蓋率**：`supportsCoverageDraws()` 為 false 時，frame builder 不發任何帶 `coverage` 的 draw，也不建立覆蓋率 atlas 貼圖——凹形填色、筆畫、輪廓文字、裁切、邊框一律走各自在 10.11 之前的路徑（凸形直畫＋fringe、stencil-then-cover、字形 atlas／hinted bitmap）。單取樣目標上這些形狀的邊緣回到沒有抗鋸齒，其餘正確。
3. **方塊陰影**（2026-11-12 改判；原案「frame builder 把 `DrawBoxShadow` 展開回舊命令序列、與關閉轉換逐位元組相同」撤回——T-691 實作前查明：關閉轉換時 painter 走的是一般效果路徑 `PushEffect`＋整個子樹＋`PopEffect`，`DrawBoxShadow` 不帶子樹範圍、`EffectCmd` 的變換與邊界、原始效果參數與多重陰影的歸屬，無法還原；而且要不要轉換必須在 paint 時決定，frame builder 為時已晚）：
   - 決定權在 paint 端：`Context::setBoxShadowCommands(device.supportsBoxShadowShading())`。本倉所有把 `Context` 接到 `GpuDevice` 的地方（C ABI 的 GPU 目標、shell、`Code/Backends` 的參考 renderer、`xtframe-view`、編輯器預覽）在建立或更換裝置時照此設定；SW 路徑不受影響（SW 自己會畫 `DrawBoxShadow`）。裝置回 false 的宿主因此自動得到舊的效果路徑，畫面正確。
   - 保底：frame builder 在 `supportsBoxShadowShading()` 為 false 時收到模糊的 `DrawBoxShadow`，不發 `Shading::BoxShadow`、該陰影不畫，計數並在 `FrameBuilder::lastError` 報一次（訊息指名 `Context::setBoxShadowCommands(false)`）。這只發生在宿主自己接 `Context` 又沒照上一點設定時；銳利陰影（模糊 0）以路徑畫，不受影響。
4. `supports…` 為 true 的裝置行為、像素、每幀配置數與今天完全相同。
5. 覆蓋率能力為 false 不是錯誤，不寫 `lastError`。frame builder 的統計加兩個計數：本次 build 因能力不足而改走舊路徑的覆蓋率形狀數（被 fringe 環路徑直接接走、無從分辨者不計）、因能力不足而未畫的方塊陰影數。
6. 23 章升級須知第 2、3 點隨實作更新：宿主什麼都不做即正確；要品質就實作並回 true。

**T-691 實作記錄（#652，2026-11-12）**：兩個能力函式在 `GpuDevice` 尾端；`FrameBuilder::lastBuildStats()` 回 `BuildStats{coverageFallbacks, boxShadowSkipped}`；paint 端的接法是 `gpu::applyDeviceCapabilities(device, context)`（只關不開）。本倉唯一「`Context` 遇到非內建裝置」之處是編輯器的宿主預覽（`host_preview_widget.cpp`，每幀套用）；C ABI、shell、`Code/Backends`、`xtframe-view` 一律經內建的 wgpu／GL 裝置（皆回 true），未加呼叫。驗證：能力皆 false 的轉發裝置跑 181 個 fixture（1x）沒有收到覆蓋率 draw 或 BoxShadow；五個陰影 fixture 與關閉轉換逐位元組相同；凹形填色／筆畫／裁切與「fringe 關」逐位元組相同。未測：真實的宿主裝置、Vulkan、Metal。

## 10.15 3D 空間內的仿射平面不算投影（2026-11-13 定案；使用者回報 challenge-menu 右下角色腳下的圓圈在 1x 斷線；調查見 BACKLOG T-700；T-700）

**事實**：`FrameBuilder` 的 `projecting()` 對 `PushProjection`…`PopProjection` 之間的**所有**內容為真，包含空間根自己平面上的一般 2D 內容。投影中 10.11 的 1x 路徑全部停用（覆蓋率填色與筆畫、影像 fringe、字形覆蓋率與 atlas、裁切邊緣、解析式方塊陰影），退到 stencil-then-cover 或輪廓路徑——1x 下沒有抗鋸齒。只要根元素寫了 `perspective`，整個 UI 就落入這個狀態（challenge-menu 除了 `#hud` 以外全部）。

**裁決**：投影的品質退讓只該發生在真的有透視變形的平面上。

1. 對目前平面（`viewProj · rootXf · world3` 限制在該平面 z = 0 上的 3×3 映射）判定是否**仿射**：齊次項 w 在平面上為常數（w 對 x、y 的係數相對於常數項的比值，在該 pass 的視窗範圍內造成的偏差小於 1/64 device px）。常見的兩種都成立：CSS `perspective` 根自己的平面（映射為恆等）、只有 `translateZ`／縮放／`rotateZ` 的圖層（整個平面離攝影機等距）。
2. 仿射的平面：把映射摺進 2D 的 device 變換（除以常數 w），其內容視為**不在投影中**——走與一般 2D 內容完全相同的路徑（10.11 全部、10.12 的方塊陰影轉換在 frame builder 這一側的處理、nearest 取樣捷徑）。`rotateX`／`rotateY` 或攝影機斜看造成 w 變化的平面維持現行的投影路徑。
3. 判定函式放在 `xt/project3d.h`（SW、GPU、simplifier 共用的那一份），GPU frame builder 採用；SW 與 simplifier 若對仿射平面也有品質較低的專用路徑，同樣採用，否則不動。
4. 像素：仿射平面上的內容與「把同樣的仿射變換寫成 2D `transform`」的畫面相同（這是驗收條件）。既有 3D golden 中屬於仿射平面的部分會改變（文字由輪廓路徑變回字形 atlas／hinted bitmap、1x 邊緣得到抗鋸齒）；逐一檢視後更新期望圖。真正透視的平面像素不變。
5. 深度排序、背面剔除、命中測試不受影響（它們在 painter／core，不看這個判定）。
6. 不在本節：真正透視平面在 1x 下的抗鋸齒（投影後輪廓進覆蓋率 atlas）——另案，待 UI Volume 的設計定了再排。

**T-700 實作記錄（#659，2026-11-13）**：`planeHomography`／`affinePlane`（`project3d.h`；w 對 x、y 無係數時直接判為仿射，否則以視窗四角回推平面上的 W、要求螢幕誤差上界 < 1/64 device px）。frame builder、SW、simplifier 三處都採用（SW 與 simplifier 原本在投影下也有較低品質的路徑）。仿射平面上的命令以 `fold · xf` 繪製，fold 為恆等時用原命令（根平面逐位元組相同）。既有 golden 無一變動；語料庫只有 challenge-menu 的 `#profile-block`（translateZ 圖層）改變：文字改走字形路徑、縮小的影像經 mip 取樣、環與弧走覆蓋率——1x 下 `#holo-arc` 由斷續變為連續。**與第 4 點預期不同**：4x 下變動不只文字，影像 mip 與展平容差也跟著 2D 路徑改變（符合「與 2D 相同」的驗收條件）。**未涵蓋**：painter 在任何 3D 空間內都不做方塊陰影轉換（`box_shadow.cpp` 的 `in3D`），所以仿射平面上的陰影仍走離屏效果——是否放寬另案（PENDING）。GL 後端未動。

## 10.16 削減 runtime 每幀交出去的工作量（2026-11-14 草案；War Table 基準與 `Docs/reviews/2026-11-14-war-table-runtime-facts.md`）

**事實**：重建命令清單只要約 1 ms；每幀的成本是把同樣的東西再交給 GPU 一次——828 個 pass，其中陰影鏈 45%、圓角裁切的遮罩層 26%。38 個每幀未命中的效果裡，22 個只是整個元素在移動或旋轉。181 個動畫有 177 個只動 transform／opacity。跨元素共用效果結果在實際內容中幾乎不存在。

以下依「確定性高、範圍小」排序；每項獨立成單、前後量測。**合成層（原計畫 P2-1）延後**：W1～W3 做完預期 pass 由 828 降到 300 上下，屆時重新量測，再決定合成層要解決的剩餘部分是什麼。

### W1 圓角裁切在 1x 以解析式覆蓋率處理（2026-11-14 定稿；可行性調查見 BACKLOG T-718；T-718 builder＋wgpu、T-719 GL）

**事實（調查）**：1x 下非軸對齊矩形的裁切最後落到遮罩層（白色形狀一張、內容一張、MaskApply 一張、再合成回去，每個約 3.7 個 pass）；4x 下是 stencil、0 個遮罩層。War Table 靜止時的 68 個遮罩層**全部**是四角圓角矩形的 `PushClipPath`（overflow＋border-radius 與圓角背景），全在仿射平面上；巢狀深度 1／2／3 各 37／19／12 個。真正透視的平面今天不走遮罩層而是硬邊 stencil（不在本項）。另一個事實：遮罩層把巢狀群組（圖層、效果）的**合成結果**也裁掉，而 SW 與 4x 只裁葉節點的繪製、不裁群組的合成——1x 今天與 SW／4x 並不一致。

**裁決**：
1. frame builder 在 1x 的裁切判定（`clipAtOneX`）中、「單一影像直接畫」之後、開遮罩層之前：路徑能辨識為**圓形圓角的矩形**（`appendRoundedRectPath` 產生的形狀；四角半徑可各自不同、可為 0）、變換可逆、不在投影中、裝置回報支援、目前作用中的解析式裁切少於 **3** 層時，改為**解析式裁切層**：不開離屏，scissor 仍收窄到其外接框。
2. 作用中的解析式裁切（最多 3 層）蓋在其內每一個**葉節點繪製**上（solid、gradient、texture 三種、glyph、coverage 兩種、box shadow 兩種、particle 兩種）：片段著色乘上各層覆蓋率的乘積。覆蓋率＝clamp(0.5 − d, 0, 1)，d 為片段到圓角矩形邊界的有號距離（device px；以局部座標的圓角矩形 SDF 經變換的 2×2 換算）。
3. **巢狀群組**（圖層、效果、遮罩）在裁切內時：裁切帶進它們的離屏目標、套在裡面的葉節點繪製上（頂點本來就是 surface px，同一組參數直接可用）；群組的合成、效果、StencilOnly 繪製**不套**。這與 SW、4x 的語意相同（陰影可以溢出圓角裁切），**1x 的畫面因此在「效果溢出圓角裁切」之處改變**——向 SW 與 4x 看齊。
4. **圓角的邊緣**用真正的圓弧距離，不重現遮罩層所用的「攤平成折線的 kappa 曲線」。直線邊與遮罩層相同（≤ 1／255）；圓角處與折線版相差至多約 0.2 px（比現況更圓）。驗收以 SW 為基準、沿用既有 golden 的容許值；受影響的 1x golden 逐一檢視後更新。
5. 效果快取：作用中的解析式裁切參數計入 segment 的雜湊（比照 stencil 層）。
6. 維持現行路徑的情形：4x（stencil）；裝置不支援（遮罩層，像素不變）；橢圓半徑的 `inset(... round a / b)`、`circle`／`ellipse`、多邊形、凹形；第 4 層巢狀圓角裁切；透視平面；旋轉的純矩形（之後可用半徑 0 併入，本項不做）。
7. 裝置介面：`GpuDevice::supportsAnalyticClip()`（尾端新增，預設 false）；`DrawCmd` 尾端新增 `clipParamIndex`（0＝無）；參數區塊 `AnalyticClipParams`（層數；每層：device→局部的 2×3、矩形、四個半徑）。自訂裝置的宿主不實作就維持遮罩層；23 章升級須知第 8 點。
8. 統計：`BuildStats` 新增解析式裁切層數與因超過 3 層／形狀不符而退回的次數。

**T-718 實作記錄（#673，2026-11-14）**：照八點實作（frame builder＋wgpu）。War Table 1080p rest（wgpu 獨顯、釘效能核、量測時另有建置在跑）：遮罩層 62.8 → 0、pass 806 → 575、draw 1359 → 1187、CPU render 中位數 14.7 → 10.3 ms、行程記憶體峰值 1471 → 1237 MB；storm 845 → 607 pass、focus 821 → 595。出入：參數區塊的陣列成員名為 `clips`（`slots` 是 Qt 的巨集）；`matchRoundedRectPath` 收 `span<const float>`；四角半徑全 0 的路徑留在遮罩層；wgpu 多一個綁定。期望圖檔無變動；`xt_golden --backend wgpu` 186／186，其中 4 張的圓角弧線改變（解析式對 16×16 超取樣的真圓角最大差 11／255，遮罩層為 59／255；與 SW 的差距因此變大——SW 畫的也是折線）。`PushMask` 的遮罩內容視為葉節點、同樣套裁切。未做：GL（T-719）、`xtframe-gpu-probe` 的假裝置仍回 false。

**第 1 點修訂（2026-11-14，T-720）**：原文「scissor 仍收窄到其外接框」是錯的——它讓群組的合成結果（溢出圓角的陰影、光暈）被外接框切成硬邊的矩形（War Table 手牌區旋轉的卡牌），與第 3 點「群組的合成不套」矛盾，也與 SW 不同。改為：**解析式裁切層不收窄 scissor**；葉節點由覆蓋率負責（框外覆蓋率為 0），群組的合成完全不受該層影響。4x 的 stencil 層也有同樣的外接框硬切（既有行為，另案，PENDING）。

**T-720 實作記錄（#675，2026-11-14）**：解析式層推入／彈出都不動 scissor。新增 `analyticBounds`（作用中各解析式裁切外接框的交集）：沒有 boundsHint 的群組離屏目標以它為範圍（不會變成整個 surface）；有 hint 的群組與所有合成不被它切。葉節點完全落在外接框外時直接跳過（`leafCulled()`——覆蓋率必為 0，結果不變；主模型接受）。已知限制：無 hint 的群組裡再有會溢出的巢狀效果，仍被外接框切。War Table rest：pass 574.8、draw 1187 不變；離屏面積 14.55 → 14.77 Mpx／幀；手牌區對 SW 差 > 16 的像素 8.44% → 0.32%。`xtframe-gpu-probe` 統計用假裝置回 true。待查：storm 的 wgpu 效果編碼時間 0.30 → 0.96 ms（原因未追）。

**T-719 實作記錄（#676，2026-11-14）**：GL 裝置實作同一能力（`XT_GL_ANALYTIC_CLIP_COMMON`，算式與 `prelude.wgsl` 逐項相同；GL 只有一份 `#version 330 core` 的著色器）。War Table 1080p rest（GL、釘效能核）：pass 828 → 586、draw 1401 → 1222、每幀新建 transient 35.9 → 23.2、含等 GPU 平均／p99 15.1／53.7 → 10.7／30.4 ms、view render 中位／p95 5.4／44.7 → 4.7／31.2 ms；storm render 中位 13.9 → 10.0 ms。GL 對 wgpu 的截圖回到最大差 1。119 個靜態 fixture：4x 全部逐位元組不變，1x 只有含圓角裁切的 4 個改變。期望圖無變動。未測：GLES／WebGL、Intel 內顯的 GL；GL 沒有「裝置不支援時走遮罩層」的對照測試。

**已知後果**：效果快取在 rest 每幀命中 69.0 → 65.7（第 5 點：外層圓角裁切的參數含絕對位置，裁切在動時裡面的效果要重畫）——併入 W2 一起解（鍵改用局部座標時，解析式裁切參數也換成效果局部座標）。

### W2 效果快取與位置無關（2026-11-14 定案；使用者：「按你說的做吧」——接受移動中元素的次像素取樣差異；T-721 平移、旋轉與縮放另單）

**事實**：`segmentHash` 把絕對位置算進鍵（各命令的絕對變換與範圍、效果的 bounds、父視窗、scissor、stencil 裁切的頂點、解析式裁切的參數）。War Table 靜止時 38 個未命中裡 22 個只是元素自己在移動（20 個平移、2 個旋轉）；T-718 之後又多 3.3 個（外層解析式裁切的絕對位置）。存入條件是「同一個鍵連續兩次建置出現」，所以會動的效果永遠存不進去。

**裁決（第一階段：平移；T-721）**：
1. 每個可快取的效果 segment 有一個**錨點**：效果 bounds 的左上角（device px，浮點）。**位置無關鍵**＝現行鍵的所有內容，但其中的位置量全部改為相對於錨點：各命令變換的平移分量、命令範圍、bounds、stencil 裁切頂點、解析式裁切的 device→局部變換。dpr、目標的取樣數與 fringe、變換的線性部分（縮放／旋轉／斜切）、效果參數、內容照舊計入。另存**相位**＝錨點的小數部分。
2. 位置無關只在這些條件都成立時使用，否則退回現行的絕對鍵（行為與今天相同）：segment 不在真正透視的投影中；效果的視窗沒有被 scissor、父視窗或目標邊界切到（存入與重用時皆然）；沒有 backdrop 類的輸入（結果取決於底下的內容）。
3. **命中**：位置無關鍵相同。
   - 相位相同（位移是整數像素）：把快取的結果貼到新的位置，與重新算一次逐像素相同。
   - 相位不同：同一筆結果以**雙線性取樣**貼到新的浮點位置。這是有意的取捨（移動中的內容本來就關閉像素對齊）。為此，快取的貼圖四周要有至少 1 px 的透明邊。
4. **停下來就校正**：某個位置無關鍵連續兩次建置的錨點相同（元素不動了）而快取那一筆的相位與現在不同時，該幀以現在的相位重算一次並取代那一筆。所以靜止的畫面永遠與「不用快取重新算」逐像素相同。
5. **存入條件**改以位置無關鍵判定「連續兩次建置出現」。預算與筆數上限不變。
6. 巢狀的效果各自以自己的錨點計算；外層命中時內層照舊不走訪。
7. 統計：`BuildStats` 新增「以不同相位重用的次數」與「停下來校正的次數」；`--perf` 一併輸出。
8. 裝置介面不變（快取的結果本來就是以貼圖 quad 畫回去；小數位置需要線性過濾——裝置若只支援 nearest 的快取貼圖，條件 3 的第二種情形不啟用，報告說明）。

**T-721 實作記錄（#677，2026-11-15）**：每命令雜湊搬到 `core/src/command_hash.cpp`，新增 `hashCommandRelative(list, index, CommandHashOrigin)`（預設 origin 的值與原本相同，damage 不變）；frame builder 先查絕對鍵（靜止的 segment 不算相對雜湊，建出的 frame 與修改前相同），不中才算位置無關鍵。相對雜湊的成本：War Table rest 每幀 25.8 次、共 0.008 ms。實作時補上的規則（主模型接受）：
- **捨入**：鍵中的相對位置捨入到 1／16 device px、效果變換的線性部分 1／256、仿射平面摺入的線性部分 2^-14——精確浮點在移動時不會相等（3D 空間內的線性部分是有限差分，有約 1e-4 的抖動）。靜止時的精確性由每筆另存的「精確相對雜湊」把關：靜止的 segment 只沿用整數位移且精確雜湊相同的結果，否則校正。
- 父視窗與 scissor 不轉成相對量，而是從相對鍵中拿掉（第 2 點已保證它們切不到視窗）。
- 1 px 透明邊只給以小數位移移動中的 segment；靜止存入的結果維持原視窗，開始小數漂移時重畫一次帶邊的版本，停下時校正。
- 條件比第 2 點略嚴：補齊 32 px 粒度後的視窗須整個落在 scissor ∩ 父視窗 ∩ surface 內；segment 內含 `PushProjection`／`PushTransform3D`、或帶 custom shader 的不重用。
- 用了移動中結果的那一幀 `lastFrameReusable` 為否（宿主跳幀後仍會校正）。
War Table 1080p（wgpu 獨顯、釘效能核、安靜時量）：rest 命中 65.7 → 88.8、未命中 42.0 → 18.8、pass 575 → 436、draw 1187 → 1071、CPU render 中位 10.05 → 8.17 ms（p95 12.6 → 10.5）；storm 607 → 467 pass、12.1 → 10.4 ms；focus 595 → 457 pass、11.4 → 9.0 ms；wgpu 內顯 rest 15.0 → 12.1 ms、GL rest 5.1 → 4.5 ms。畫面（rest，修改前後）：平均差 0.17、差 > 16 的像素 0.085%，集中在浮動的單位圖；小數位移的測試案例最大差 58／255、只在邊緣，停下後逐位元組相同。未做：`xtframe-view --perf` 尚未印出 `effectCachePhaseHits`／`effectCacheSettles`（要動 `Code/Runtime/wgpu` 的統計轉拷；併入下一張動 wgpu 的單）；未測 Direct 模式、dpr ≠ 1、GL 的小數貼回像素。已知：捨入邊界上的鍵偶爾翻轉，移動中的元素會多重畫幾幀。

**第二階段（另單，待第一階段量測後）**：旋轉與縮放在變的元素——鍵再去掉線性部分，重用時以新的變換貼上；縮放比存入時放大超過 1.125 倍或縮小到 0.5 倍以下時重算；停下來校正的規則相同。

預期（War Table rest，待量測）：未命中 38＋3.3 → 約 18（剩 12 個內含動畫子元素、4 個陰影參數在動、2 個旋轉）；每個命中省約 6～9 個 pass。

不在本項：效果內含動畫子元素（內容真的在變）、陰影參數在動。

### W3 perspective 根的平面上也做方塊陰影轉換
10.15 已裁定仿射平面的內容「視為不在投影中」，但 painter 判斷方塊陰影轉換時仍以「mapping stack 非空」拒絕。改為：元素所在的平面為仿射（10.15 第 1 點的判定，同一個函式）且元素自己沒有 3D 變換時，`in3D` 為否。SW 在投影內收到 `DrawBoxShadow` 的行為要一併確認。

**T-716 實作記錄（#679，2026-11-15）**：`project3d.h` 新增 `PlaneTest`／`planeTest()`／`stackPlaneAffine()`，frame builder、SW、simplifier、painter 共用同一份判定。painter 另記每層 mapping stack 所在的 3D 圖層並要求 stack 的每個前綴都仿射（巢狀的第二個 perspective 根；只看最外層會與 frame builder 判定不一致，而後端在投影平面上收到 `DrawBoxShadow` 是直接不畫）。world-layer 輸出時 painter 只接受 w 為常數的平面。War Table rest：`DrawBoxShadow` 0 → 13、效果層 123 → 110、pass 435.7 → 370.2、draw 1071 → 1047、綁定 863 → 842、上傳 614 → 610 KB、未命中 18.8 → 11.4；CPU render 中位：wgpu 獨顯 8.7 → 6.9 ms、內顯 12.7 → 10.6 ms、GL 3.4 → 2.8 ms（p95 27.3 → 5.0）；內顯含等 GPU 約 45 → 40 ms；GL 每幀新建 transient 18.9 → 1.5、含等 GPU p99 約 30 → 7 ms（量測期間另有 benchmark 在跑）。既有期望圖無變動；perspective 根下與無 perspective 的畫面逐位元組相同（SW、wgpu、GL）。畫面：解析式對離屏高斯的差最大 27（wgpu，大模糊半徑的光暈）／38（SW），屬 10.12 的既有取捨。**只轉換 13 個的原因**：123 個效果中 65 個不過 10.12 條件 3（背景不是整個邊框盒上的不透明背景）、23 個效果鏈不是純外陰影、20 個在真透視的傾斜地圖上、5 個 opacity < 1。發現未改：`Painter::m_currentLayer` 在巢狀 3D 空間結束後不還原（PENDING）。

**T-724（#681，2026-11-15）：SW 的解析式方塊陰影**。T-716 之後 SW 每幀逐像素重算轉換出來的陰影（War Table 約 100 ms／幀），比原本的離屏模糊貴（SW 沒有效果快取——快取只在 GPU 的 frame builder）。修正：(1) 每像素的兩個直邊 erf 值只算一次、軸對齊者按欄重用（逐位元相同）；(2) `sw::SwShadowCache`：存浮點覆蓋率（與不用快取的值完全相同），鍵＝形狀與附加項、sigma、完整的 device 變換、涵蓋的 device 矩形（顏色在覆蓋率之後套用，不入鍵）；同鍵連續兩幀出現才存；預算 16 MiB、LRU、120 幀未用即丟；`releaseScratch()` 與解構時全部釋放；命中不配置。公開 `SwRenderer::setBoxShadowCacheBudget`／`boxShadowCacheBytes`。War Table sw rest 331 → 237 ms（p95 242）、storm 334 → 246、focus 183 → 144、首幀 172 → 131 ms；12 筆、1.46 MiB；與修改前的 SW 輸出逐位元組相同。未加 perf CSV 計數。

### W4 已到終點且 hold 的時間軸成員算結束
暫停在終點的成員：不再每幀求值與寫值、不再標為「正在動」（其子樹恢復像素對齊）；被 seek、重播、反向或釋放時恢復。輸出值不變。**畫面會變**：被 hold 的容器底下的內容恢復像素對齊（這是修正）。

**T-717 實作記錄（#674，2026-11-14）**：只限時間軸群組的成員（暫停在終點的一般 CSS 動畫不變）。成員暫停且位於終點（fill 含 Forwards 的結尾，或 fill 含 Backwards 的開頭）時，進入的那一幀照常寫入，之後跳過求值、寫入與 `noteMotion`；seek／pause／resume／setSpeed／updateParams／retarget 清除該狀態。fill 值在 restyle 後的保持：節點新增兩個內部位元——`kAnimValueStale`（任何 `setProperty` 或樣式 commit 設定 → held 成員當幀或下一幀重寫，時序與修改前相同）與 `kAnimWriteQueued`（同一次求值內較早的寫入；held 成員只在**同屬性**被別的動畫寫入時才醒來，依 transform 分量比對）。新計數 `FrameStats::animEvaluated`（`--perf` 與 CSV 最後一欄）。War Table（釘效能核、有背景負載）：`#bench-all` 後每幀求值 441 → 181、動畫步驟 0.163 → 0.077 ms；靜止時每幀少 16 個（180 → 164；靜止的 197 裡還有在 animation-delay 中的 CSS 動畫，所以不是原先預期的 181）。期望圖變動一張：`sym_BottomSheet_expanded.png`（展開後被 hold 的 sheet 恢復像素對齊，標題字形 205 px 改變）。未編譯：GCC／Clang。

### W5 互動只影響該影響的
1. hover／active／focus 狀態改變時，只有「存在可能命中該狀態的選擇器」的元素才標樣式髒；子樹只在有後代選擇器依賴該狀態時才標。
2. 樣式綁定寫入時不標祖先；值沒變時不寫、不重啟 tween。

**W5 補充裁決（2026-11-15；T-723 中途回報之後）**：
- 第 2 點查證：祖先確實被重新解析（`kDirtyStyle` 只有一個位元，`markDirty` 沿祖先設定，`resolve` 對帶該位元的節點全部重新解析）。修正＝只標元素自己（`markStyleDirtySelf`）；War Table 的 port 更新重算 96 → 30 個、style 1.48 → 0.48 ms。
- 新舊兩種失效模式的等價測試在 3 個 UI 上不相等，根因是 05 §5.6a 所述的既有缺陷（多餘的重新解析會重啟 transition、真正的改變可能被吞掉）。裁決：先修 5.6a（它本身是使用者看得到的缺陷），等價測試在修正後必須相等。
- 第 1 點「只有自己重新解析」需要的下傳規則（引擎原本沒有）：元素重新解析後，若其計算樣式在**可繼承的屬性、自訂屬性、font-size**上有改變，就把直接子節點標為樣式髒（走訪是由上而下，子節點在同一輪被重新解析，再依同一規則往下）；另外，樣式表中只要有任何規則對非繼承屬性使用 `inherit` 關鍵字（編譯時記一個旗標），就退回「標整個子樹」。text-decoration 的顏色與粗細比照可繼承屬性。

**T-723 實作記錄（#680，2026-11-15）**：(D) 05 §5.6a——`StyleEngine::resolve` 以目標值（進行中 transition 的終值，否則元素的值）比較；範圍是元素的 base 屬性，scene-target 與效果參數的 transition 路徑仍用舊比較（未見失敗，待補）。(A) 綁定寫入：值沒變不寫、不標、不重啟 tween；值變只標元素自己（`NodeArena::markStyleDirtySelf`），可繼承屬性與 text-decoration 顏色／粗細才標子樹。(F) 下傳：元素重新解析後，可繼承屬性、font-size、`lang`、ui-scale／media-override 與有效縮放、text-decoration 有變時標直接子節點。`inherit` 關鍵字的退回規則與自訂屬性沒有對象（編譯器與 IR 不編 `inherit`；自訂屬性是文件層的 token，已有 `markTokenUsersDirty`）。狀態相依表 `core/src/style_state_deps.{h,cpp}`：每個測試 ElementState 位元的 compound 一筆（測試的位元、是否主體、只留 tag／id／class 的 compound——其餘條件視為恆真，只會多標不會漏標）；`setElementStateBit` 依它標「不標／只標自己／標子樹」，不再標祖先；選擇器引擎沒有 `+`／`~`。War Table：1,197 條規則 → 23 筆（20 個 `:hover`、3 個 `:active`；21 主體、2 祖先）。等價測試（`Tests/golden/golden_style_invalidation.cpp`）：160 個 UI、29,609 步，新舊模式全部元素的計算樣式逐位元相同。War Table（釘效能核）：指標掃過每幀重算 平均 35.9／最大 523 → 1.1／19，update p95 4.93 → 0.55 ms、最大 8.7 → 0.9 ms；port 更新重算 96 → 30、style 1.48 → 0.50 ms、update 2.09 → 1.09 ms。10 張截圖前後相同。期望圖變動 3 張（皆因 5.6a）：`sym_Slider_drag`、`sym_SliderTouch_drag`（把手現在是 focus／按下的顏色——舊圖是被不斷重啟的 transition 卡住的舊色）、`sym_Button_focus`（恢復像素對齊）。未涵蓋：`ElementRef::setState`、`bind:state`／`bind:checked`／`bind:disabled`、自訂狀態、`:focus-player` 仍標全範圍。未跑 editor／flow／bundle／capi 套件；未編譯 GCC／Clang。

### W6 damage 的兩個修正
1. 規則 B6（效果內有變動就整個效果範圍算髒）維持——效果的輸出確實可能整塊改變——但合併後才判「整幀」的門檻改以聯集面積計，而不是面積總和。
2. 投影內 3D 圖層移動時，以該圖層投影後的範圍產生 damage（現況是空的，潛在殘影）。

### 不做／延後
跨元素共用效果結果（事實不支持）；背景模糊的快取（其下的內容每幀在變）；合成層（見上）。

## 10.17 wgpu 裝置的提交成本（2026-11-15；`Docs/reviews/2026-11-15-wgpu-vs-gl-cpu.md`）

**事實**：GL 的驅動自己有一條工作執行緒（每幀 3.3～3.5 ms，render 計時看不到）；wgpu 的工作全在呼叫端執行緒，且由 pass 數決定（每個 pass 6～8.5 µs，wgpu-core 每 pass 開兩份 D3D12 command list）。draw 與重複的狀態呼叫幾乎不花時間。

**T-726 實作記錄（#684，2026-11-15）**，修訂 §10.10.2 的對應敘述：
- **標籤**：自建 instance 時加 `WGPUInstanceFlag_DiscardHalLabels`、render pass 不帶 label；`XT_WGPU_LABELS=1` 恢復（供 PIX／RenderDoc）。宿主自建的 instance 不碰。
- **bind group 快取**：每個可被引用的 view 帶一個行程內唯一、永不重用的 id；快取以 id 為鍵並維護 id → 鍵的索引，銷毀一個 view 只移除引用它的 bind group（原本是整批清空）。跨裝置共用的快取以最近 4096 筆的銷毀日誌同步。transient 池的預算與淘汰規則不變。
- **上傳**：頂點與索引緩衝由三組輪替改為**單一組、與前一幀比對**（§10.10.2 表中「三組緩衝輪替」與 T-438 記錄就此作廢；uniform 本來就是單一組）。依據：`wgpuQueueWriteBuffer` 的資料先進 staging，複製指令插在下一次 submit 的最前面，不會改到在途幀讀的內容；被換掉的舊 buffer 活到在途幀做完。合併間隙 `XT_WGPU_UPLOAD_MERGE_GAP`（預設 1 KB；64 B 時寫入更少但 CPU 加倍）。
- War Table 原版、wgpu 獨顯（四個建置交錯輪跑的中位數，釘效能核）：view CPU render 平均 7.07 → 6.24 ms、p95 9.01 → 7.04 ms；probe 含等 GPU 平均／p99 9.90／12.25 → 8.15／10.15 ms；內顯 view 平均 10.53 → 9.63 ms。每幀建立的 bind group 103.8 → 13.1（優化版 32.5 → 11.4）；每幀上傳 610 → 376 KB（優化版 493 → 310 KB）。上傳項只省位元組，CPU 持平。截圖逐位元組相同。未測：Vulkan／Metal、宿主自建裝置。

**待設計（未動工）**：(1) 減少 pass——小的效果目標併進共用圖集、互不相依的階段合併（估 wgpu −1.3～1.8 ms，並降記憶體）；(2) `Finish`／`Submit` 移到工作執行緒（render 執行緒最多 −4.5 ms；要改 50 §50.1「庫內不開執行緒」）；(3) 葉節點共用一支著色器；(4) Windows 上 Vulkan 後端的評估。

## 10.18 減少 pass（2026-11-15 草案；可行性調查 `E:\coding3\xt-bench\investigation-20261115-pass-merging\`）

**事實（War Table，單幀實測）**：pass 數 rest 359（原版）／267（優化版）、storm 421／276、focus 379／275。
- **玻璃面板佔了大半**：10 個背景模糊面板，每個的效果堆疊是「BackdropBlur → 外陰影 → 內陰影」，各花一次 surface 複製加 20 個串行的 pass（金字塔 4、取樣 1、Backdrop 1、兩個陰影各 7）——共 200 個，佔優化版的 75%。`fx_backdrop` 之後的每一步都依賴 surface；面板排成左右兩欄，下面的面板讀到上面面板的合成結果，所以鏈是真的串行（6 個世代）。
- frame builder 是單次向前走訪、沒有規劃階段；每個效果步驟立刻輸出自己的 pass，合成回父目標時重開父 pass——surface 因此被切成 17～21 個 pass。
- 同一條效果鏈上的步驟不可能併（WebGPU 不允許在同一個 pass 取樣自己的 attachment）；能併的只有「不同效果、同一深度」的步驟。
- 靜止時效果快取 83 命中／12 未命中，命中不花 pass。
- focus 狀態有 97／83 個 transient pass 大於 1024²（例：頂部分頁的陰影鏈在 focus 時跑在 1920×1080 的視窗、靜止時是 160×64），原因未查明。

**依序進行（每步獨立量測；畫面要求寫在各步）**：

### A. 三個逐位元相同的局部修正（T-728）
1. **AlphaExtract 併進下一步**：陰影鏈的第一步「取 alpha」併進緊接的 Downsample／GaussScalar（讀 `.a` 或 `1 − .a`；Dilate 已有同樣的讀法）。alpha 本來就是 8 位元，結果逐位元相同。每個陰影少 1 個 pass（rest −30／−22）。參數結構尾端加一個通道欄位；自訂裝置以能力查詢把關（預設維持現行）。
2. **transient 之間的複製之後不開空的 render pass**（wgpu 裝置）：focus −8。
3. **空的背景模糊捷徑**：BackdropBlur 要讀的區域在父目標清除之後還沒有任何繪製時，金字塔全為零、`fx_backdrop` 的結果恰等於來源——略過複製、金字塔、取樣與 Backdrop：focus −21 個 pass（都是全螢幕的）。

### B. 玻璃面板的陰影與背景脫鉤（T-729；先驗證前提）
前提（要先以程式與像素驗證）：`fx_backdrop` 輸出的 alpha 等於來源的 alpha（背景只填在來源形狀之內）。若成立，接在 BackdropBlur 之後的外陰影／內陰影只取決於**來源的 alpha**，與 surface 的內容無關。
- 作法：效果堆疊為「BackdropBlur 之後接只讀 alpha 的陰影類效果」時，陰影鏈的輸入改取來源層（BackdropBlur 之前）的 alpha；陰影的結果因此不依賴 surface，可以**獨立進效果快取**（鍵＝來源內容與陰影參數，沿用 W2 的位置無關鍵）。面板靜止時，每個面板 14 個陰影 pass 變成 0。
- 合成順序與最終像素必須與現行相同（陰影在 Backdrop 結果之下／之上的順序不變）；前提不成立或只在部分情形成立時，只對成立的情形啟用，其餘維持現行。
- 預期（待量測）：rest 最多 −140 個 pass（兩個場景皆然），並直接減少內顯每幀的模糊工作量。

### C. 排程與圖集（A、B 量測之後再決定是否進行）
- C1 frame 排程（只重排）：建完 frame 後計算真正的相依關係，把不依賴 surface 的離屏工作提前、相鄰的 surface pass 合併（surface 17～21 → 約 7 個 pass）；重排後畫面逐位元相同；要有峰值 transient 面積的上限。
- C2 pass 群組＋裝置圖集：同一（世代、深度）且互不讀寫的離屏步驟畫進同一張頁面的不同格（每格自己的 viewport／NDC／scissor；每支取樣上一步的效果著色器加輸入矩形與取樣範圍夾限；快取存入與複製改為子矩形）。模擬：rest 359 → 約 149、267 → 約 133；畫面最大差 1。自訂 `.xtfx` 效果與 Direct 模式不參與。
- C 的收益會被 B 吃掉一部分（B 之後剩下的串行鏈更短），所以先做 B 再重新模擬。

### D. 需要裁決、暫不進行
- 玻璃鏈上的近似合併（金字塔降階＋Kawase 等；8 位元中間值會差約 ±1，SW 也要跟著改）。
- Layer 模式的背景模糊改成像 Direct 模式那樣共用一份金字塔（會改變像素）。

**T-728 實作記錄（#685，2026-11-15；§10.18 A）**：(1) `GaussParams`／`DownsampleParams` 尾端新增 `channel`（0 現行、1 讀 `.a`、2 讀 `1 − .a`）；`GpuDevice::supportsAlphaChannelReads()`（預設 false；wgpu、GL 回 true）；spread 為 0 的外陰影／內陰影與 glow／inner glow 的 AlphaExtract 併入下一步（有 spread 的鏈不變）。`.a` 在結構上逐位元相同；`1 − .a` 只在本機 NVIDIA 的 D3D12 與 GL 上驗證逐位元相同（浮點與雙線性過濾在別的硬體上理論上可能在進位平手處差 1）。(2) wgpu 在 transient 複製之後不開空的 render pass。(3) `FrameEmitter::untouchedSinceClear(parent, region)`：父目標是本幀清除過的 transient、區域完全在其視窗內、區域內沒有任何複製或著色命令時，BackdropBlur 不建金字塔、直接透傳來源（非 Direct 模式）。War Table（每幀平均、wgpu、釘效能核、前後交錯三輪中位）：原版 pass rest 370 → 337、storm 409 → 376、focus 389 → 346；優化版 272 → 245、285 → 258、296 → 258。wgpu 獨顯 CPU render 中位：原版 rest 6.57 → 5.99 ms、優化版 4.90 → 4.61 ms；行程記憶體峰值（原版 rest）1036 → 950 MB。截圖 rest／focus 逐位元組相同。focus 的降幅小於調查的單幀預期（−43／−38 對 −56／−51）。

**§10.18 B 的前提不成立（T-728 查證）**：`fx_backdrop` 的輸出 alpha＝`a + b·a·(1 − a)`（a＝來源 alpha、b＝模糊後背景的 alpha）。a 為 0 或 1、或背景為空時等於 a；玻璃面板的半透明填色與抗鋸齒邊緣處則大於 a 且依賴背景。所以接在 BackdropBlur 之後的陰影現在確實依賴 surface。要脫鉤只能改語意——**陰影類效果接在 BackdropBlur 之後時，改取來源（BackdropBlur 之前）的 alpha**。這與 CSS 一致（`backdrop-filter` 不影響元素自己的 `filter: drop-shadow`／`box-shadow`），但畫面會變：半透明面板的陰影變淡（例：填色 alpha 0.5 且背景不透明時，陰影依據的 alpha 由 0.75 變為 0.5）。**待使用者裁決**；裁決前 B、C 不動。

### §10.18 B 定案（2026-11-15；使用者：「改改改，這都小事不用問」）：背景模糊不參與元素自己的陰影

**新語意**（SW 與 GPU 相同；與 CSS 一致——`backdrop-filter` 不影響元素自己的陰影）：效果堆疊以 BackdropBlur 開頭、其後**只有**陰影類效果（外陰影、內陰影、Glow、InnerGlow）時：

1. 設 S＝來源層（元素自己的內容）。先在 S 上依序套用那些陰影類效果，得到 R′——與「同一個元素沒有 BackdropBlur」時的結果完全相同，與 surface 無關。
2. 設 B＝現行的模糊後背景（金字塔取樣、套飽和度／亮度後的那一層）。最終結果＝**R′ 疊在「以 S 的 alpha 為遮罩的 B」之上**：`out.rgb = R′.rgb + B.rgb · S.a · (1 − R′.a)`、`out.a = R′.a + B.a · S.a · (1 − R′.a)`（預乘）。背景只出現在元素自己的形狀內；陰影不再含有背景的 alpha。
3. 堆疊中 BackdropBlur 之後有其他種類的效果（ColorAdjust、遮罩、自訂效果等）、或 BackdropBlur 不在第一個：維持現行的逐步套用，不變。
4. 沒有陰影類效果的純 BackdropBlur：R′＝S，公式退化為現行算式，逐位元不變。

**畫面變化**：半透明填色的玻璃面板，其陰影依據的 alpha 由 `a + b·a·(1 − a)` 變為 `a`——陰影變淡（a＝0.5、背景不透明時 0.75 → 0.5）；抗鋸齒邊緣處的陰影略細。受影響的期望圖逐一更新並列出。

**分兩張單**：
- **T-729**：語意改動（SW、frame builder、wgpu、GL）。pass 數大致不變，但陰影鏈從此不依賴 surface。裝置需要「帶遮罩的 Backdrop 合成」：`GpuDevice::supportsBackdropMask()`（預設 false——未實作的自訂裝置維持舊語意，23 章升級須知註明畫面會與參考後端不同）。
- **T-730**（T-729 之後）：把 S 與 R′ 納入效果快取（沿用 W2 的位置無關鍵與「連續兩次建置才存」的規則；一個元素兩筆：來源層、陰影結果）。靜止的面板每幀只剩背景的複製、金字塔、取樣與最後的合成。預期每個面板 20 → 約 6～7 個 pass。

**T-729 實作記錄（#686，2026-11-15；§10.18 B 第一步）**：判定函式 `backdropThenShadowsOnly`（`core/src/effect_math.h`；SW 與 GPU 共用）。SW：`applyBackdropBlurUnder(top = R′, mask = S)`，原本的 BackdropBlur 以 top = mask = src 走同一段運算（浮點相同）。frame builder：`runShadowsUnderBackdrop()`——先在來源層上跑陰影鏈（不讀 surface），再取模糊背景，最後一個 `EffectProgram::BackdropMasked` pass（`BackdropParams{mask}` 指向來源層）；pass 數不變。裝置：`GpuDevice::supportsBackdropMask()`（預設 false＝舊語意）；wgpu `fx_backdrop_masked.wgsl`、GL `kFxBackdropMaskedFrag`；原本的 Backdrop 程式未動。Direct 模式的共用金字塔照新語意。期望圖無變動（既有 golden 的 backdrop 都沒有接陰影）。War Table：計數前後完全相同（原版 rest 337 pass、優化版 245）；截圖 rest 平均差 0.17、最大 18，都在玻璃面板的陰影與邊緣；GL 對 wgpu 最大差仍為 1。18 章的對應修訂見該章末尾。本節上方 B 草案中「合成順序與最終像素必須與現行相同」「前提若成立」兩句已由「B 定案」取代。

**T-730 實作記錄（#687，2026-11-15；§10.18 B 第二步）**：符合 `backdropThenShadowsOnly` 的 segment 在支援快取且 `supportsBackdropMask()` 的裝置上快取一對結果（一個 `CacheEntry`，`texture`＝R′、`maskTexture`＝S；算 2 筆、兩倍位元組，放不下就都不存）。鍵沿用絕對鍵／W2 的位置無關鍵（「沒有 backdrop 輸入」的條件對這一對不適用），加 pair 標記，且 PushEffect 的雜湊把 BackdropBlur 的參數歸零（`frame_cache_pair.{h,cpp}`）。命中時不走訪 segment；背景的複製、金字塔、取樣照常，R′ 與 S 各貼進一張暫存目標（`BackdropMasked` 的輸入是 target，快取結果是 texture——每個元素因此比預期多 2 個 pass），再做 `BackdropMasked` 與合成：每個面板約 20 → 7～9 個 pass。新統計 `backdropPairHits／Misses／Stores`、`effectCacheBytes／Entries`，並把 T-721 的 `effectCachePhaseHits／Settles` 接到 `--perf`。War Table rest（釘效能核、前後交錯三輪中位）：pass 原版 337 → 266、優化版 245 → 140；draw 1014 → 720、934 → 544；上傳 373 → 294 KB、307 → 232 KB；wgpu 獨顯 CPU render 中位 6.89 → 5.10、5.29 → 3.05 ms；內顯含等 GPU 平均／p99 35.2／38.4 → 29.7／35.6、28.9／32.8 → 16.9／18.9 ms；GL 含等 GPU 4.82 → 3.96、3.55 → 2.39 ms。快取佔用 13.5 MB／84 筆（原版）、21.7 MB／75 筆（優化版）。rest／focus 截圖逐位元組相同。原版 10 個面板中 4 個每幀未命中（面板內含動畫子元素，bounds 每幀在變）；優化版約 9 個命中。未測：Vulkan／Metal、Direct 模式的宿主場景、dpr ≠ 1。可再省的：讓 `BackdropMasked` 直接吃快取貼圖（每個面板再 −2 pass，需裝置端配合）。

## 10.19 量測用的 GPU 計時與等待分離（2026-11-17；使用者：「確保量的是確實量到 CPU、GPU 執行時間，等待時間要分開來」；T-734）

**問題**：到目前為止「GPU 時間」都是 CPU 送出後等佇列做完的牆鐘時間（`xtframe-gpu-probe` 的 render + queue wait），混了同步開銷、GPU 降頻與排程；CPU 的各階段也只量牆鐘，阻塞在驅動或在途幀上限裡的時間算進了「CPU」。

**定案**：
1. **GPU 執行時間以 GPU 時間戳量**（WebGPU `timestamp-query`）。`xt_wgpu` 提供**預設關閉**的診斷開關：headless 初始化可要求該功能（adapter 不支援就不要求、照實寫進 notes）；renderer 開啟計時後，每幀在第一個 command encoder 開頭與最後一個結尾寫時間戳（裝置支援 `TimestampQueryInsideEncoders` 時；否則退回每個 pass 的 `timestampWrites`），另記每個 pass 的起訖。結果以非阻塞的 readback 環在幀完成後取回，對外是「第 N 幀：GPU 跨度（第一個時間戳到最後一個）、pass 合計、pass 數、用了哪一種寫法」。關閉時不要求功能、不多任何指令，畫面與成本與今天相同。
2. **CPU 執行時間以執行緒 CPU 時間量**（Windows `QueryThreadCycleTime` 以啟動時校準的係數換成毫秒；其他平台 `CLOCK_THREAD_CPUTIME_ID`），每個階段同時記牆鐘；**等待＝牆鐘 − 執行緒 CPU**。另以行程 CPU 時間（所有執行緒）對照主執行緒，看出驅動或 runtime 的其他執行緒做了多少。
3. **CPU 等 GPU 單獨列**：在途幀上限的等待（`beginFrame`）與送出後等佇列（只在逐幀同步的量法）各成一列，不併入 render。
4. 這是量測工具的能力，不是出貨行為；GL 後端不在本次範圍（GL 驅動另有工作執行緒，見 §10.17，行程 CPU 時間可以涵蓋它，但 GPU 時間戳要另做）。

**解讀限制**（報告必須照寫）：逐幀同步時 GPU 在幀與幀之間閒置，獨顯可能降頻，時間戳量到的是「該頻率下的執行時間」；不同步（`--no-finish`）的跑法較接近遊戲實際情況，兩種都要量。時間戳只涵蓋該幀 command buffer 內的工作，`wgpuQueueWriteBuffer` 的上傳複製插在下一次 submit 最前面（§10.17），以 encoder 層級時間戳時算在下一幀。

