# 19 · 3D 空間（After Effects 式 2.5D）

本章定義 XTFrame 的 3D 能力邊界與實作方式。模型取自 After Effects：**元素與場景節點是放在 3D 空間中的平面**，由攝影機投影到視口；沒有網格、沒有材質、沒有燈光。內容本身仍是 2D（路徑、圖片、文字、效果），3D 只改變它們被擺放與投影的方式。

本章對既有章節的修訂條款集中在 19.10；實作任務以本章為準，修訂條款同時併入對應章節。

## 19.1 範圍

| 能力 | 0.2 | 0.3 | 不做 |
|---|---|---|---|
| 節點 3D 變換：`translateZ`、`anchorZ`、`rotationX/Y`（`rotation` 即 Z）、`orientation` | ✓ | | |
| 攝影機：透視/正交、位置、目標點、焦距（zoom） | ✓ | | |
| CSS：`transform` 的 3D 函式、`perspective`、`perspective-origin`、`backface-visibility`、`<camera>` 標籤 | ✓ | | |
| 攝影機動畫：Lottie 攝影機圖層、API 時間軸 | ✓ | | |
| 攝影機動畫：CSS `@keyframes` 作用於 `<camera>` | | ✓ | |
| 逐圖層深度排序（畫家演算法） | ✓ | | |
| 透視正確的繪製（路徑、圖片、文字、裁切） | ✓ | | |
| 命中測試：指標射線與圖層平面求交 | ✓ | | |
| Lottie：`ddd:1` 圖層、攝影機圖層（`ty:13`） | ✓ | | |
| 烘焙與引擎最小子集：3D 展平成 2D 命令 | ✓ | | |
| 圖層交錯（逐像素深度、z-buffer、烘焙帶深度） | | ✓ | |
| 燈光、材質、投射陰影、景深 | | | ✓（效果模擬） |
| 3D 網格、模型匯入 | | | ✓ |

## 19.2 座標系與矩陣

- 右手座標：x 向右、y 向下（與 2D 一致）、**+z 朝向觀者**（CSS 慣例：`translateZ(+d)` 讓元素靠近觀者、看起來變大）。攝影機位於 z > 0 處朝 −z 看。
- After Effects / Lottie 的 +z 遠離觀者：匯入時 `z ← −z`（位置、錨點、攝影機位置與目標點都取負）。
- 角度：度，`rotationX/Y/Z` 依序繞局部軸；`rotation`（既有 PropId 7）就是 Z。
- 新型別（`xt/types.h`，實作於 `Code/Runtime/core/src/mat4.cpp`）：

```cpp
struct Vec3 { float x, y, z; };
struct Mat4 { float m[16]; };          // 列主序 m[row*4+col]；點為欄向量：p' = M * p
Mat4 mat4Identity(); Mat4 mat4Mul(const Mat4& a, const Mat4& b);   // a * b
Mat4 mat4FromMat2x3(const Mat2x3&);    // 2D 仿射嵌入 z = 0 平面
Mat4 mat4Translate(Vec3); Mat4 mat4Scale(Vec3);
Mat4 mat4RotateX(float deg); Mat4 mat4RotateY(float deg); Mat4 mat4RotateZ(float deg);
Mat4 mat4LookAt(Vec3 eye, Vec3 target, Vec3 up);        // 視圖矩陣（右手，看向 −z）
Mat4 mat4Perspective(float fovYDeg, float aspect, float near, float far);   // OpenGL 慣例 NDC z ∈ [−1, 1]
Mat4 mat4Ortho(float l, float r, float b, float t, float near, float far);
bool mat4Invert(const Mat4&, Mat4& out);
Vec3 mat4Transform(const Mat4&, Vec3);                   // 不除 w
Vec3 mat4Project(const Mat4&, Vec3, float* wOut);        // 齊次除法後的 (x, y, zNdc)；w ≤ 1e-6 時回傳 w = 0 由呼叫端裁掉
struct Mat3 { float m[9]; };                             // 單應矩陣（19.7 SW 用）
bool homographyFromQuad(const Vec2 src[4], const Vec2 dst[4], Mat3& out);
```

**局部變換（3D 節點）**，右乘欄向量：

```
local3 = T(translate.xyz)
       * Rz(rotation) * Ry(rotationY) * Rx(rotationX)      // 逐軸旋轉（AE Rotation X/Y/Z）
       * Rz(orientation.z) * Ry(orientation.y) * Rx(orientation.x)   // 方向（AE Orientation），預設 0
       * Sk(skew) * S(scale.x, scale.y, 1)
       * T(-anchor.xyz)
```

2D 節點（`is3D == 0`）維持 03.3 的 `local`（Mat2x3）計算，成本不變。

## 19.3 節點資料（修訂 03.3）

`NodeCommon` 新增欄位，**固定放在 `worldOpacity` 之後、依此順序**（18 章的效果欄位再接在其後）：

```cpp
// --- 19 章：3D ---
float   translateZ;     // PropId 16
float   anchorZ;        // PropId 17
float   rotationX;      // PropId 18，度
float   rotationY;      // PropId 19，度
Vec3    orientation;    // PropId 22，度
uint8_t is3D;           // 0 = 純 2D（預設）；1 = 3D 圖層
uint8_t backfaceHidden; // 1 = 背面不繪製也不命中
uint8_t _pad3d[2];
Mat4    world3;         // 只在 is3D == 1 時有效：相對於所屬 3D 空間根的變換（19.5）
```

- `is3D` 的來源：**場景節點**由 Lottie `ddd:1` 靜態決定（IR 欄位）。**Element** 由執行時推導：StyleEngine 每次解析後 `is3D = (base[16] ≠ 0 或 base[17] ≠ 0 或 base[18] ≠ 0 或 base[19] ≠ 0 或 base[22] ≠ (0,0,0))`；Animator 對 Element 的 PropId 16–22 存在作用中的 Track/transition/override 時把 `is3D` 設 1，全部結束後由下一次樣式解析恢復。所以只在 `:hover` 或 `@keyframes` 出現的 3D 旋轉也生效。`backfaceHidden` 對 Element 由層疊的 `backface-visibility` 每次解析寫入；場景節點恆 0。`Document::setNode3D(NodeId, bool)` 只對場景節點有效（Element 回 `kInvalidArgument`）。
- **3D 空間根**（簡稱「根」）：擁有攝影機的節點（19.4）。根本身是 2D 節點（其 `world` 為一般 2×3）。
- **3D 圖層**：根的子樹內（不含巢狀根的子樹）每個 `is3D == 1` 的節點是一個 3D 圖層。圖層的內容 = 自身 + 其 2D 子孫，直到遇到下一個 3D 圖層（該 3D 子孫是另一個獨立圖層）。
- 2D 節點在根的子樹內只有一個意義：它的 2D `local` 是「相對於所在平面」的變換。根的直接 2D 子孫位於根平面（z = 0）；3D 圖層的 2D 子孫位於該圖層平面。2D 節點底下的 3D 子孫仍是 3D 圖層（其 `world3` 由該 2D 祖先鏈的 2D 變換嵌入 z = 0 後與自身 `local3` 相乘，見 19.5）。

新增 PropId（修訂 03.9：通用區擴為 0–31）：

| PropId | 名稱 | 型別 | 適用 | 說明 |
|---|---|---|---|---|
| 16 | TranslateZ | Float | 全部 | |
| 17 | AnchorZ | Float | 全部 | |
| 18 | RotationX | Float | 全部 | 度 |
| 19 | RotationY | Float | 全部 | 度 |
| 20 | Translate3 | Vec3 | 全部 | 合成 (TranslateX, TranslateY, TranslateZ)，供空間貝茲；Element 的 x/y 與 PropId 3/4 同儲存（06.7 的 `cssTranslate`） |
| 21 | Anchor3 | Vec3 | 全部 | 合成 (AnchorX, AnchorY, AnchorZ)；Element 的 x/y 與 PropId 1/2 同儲存 |
| 22 | Orientation | Vec3 | 全部 | 度，AE Orientation |
| 23 | — | | | 保留 |
| 24 | CameraZoom | Float | Camera | 焦距 px（19.4） |
| 25 | CameraTarget | Vec3 | Camera | 目標點（根座標） |
| 26 | CameraNear | Float | Camera | |
| 27 | CameraFar | Float | Camera | |
| 28–31 | — | | | 保留 |

`propertyApplies(kind, prop)`：16–22 對所有可繪製節點種類成立，24–27 只對 `Camera` 成立。`ValueType` 新增 `Vec3`（03.10）：線性插值；空間貝茲（04）對 Vec3 以三維切線插值：`Keyframe`/`IrKeyframe` 的 `tanOut/tanIn` 由 `Vec2` 改為 `Vec3`（IR major 2 一併改，2D 軌道的 z 分量為 0），Lottie 的 `ti/to` 若含第三分量取用（取負），否則 z 切線為 0。

## 19.4 攝影機

新增節點種類 `NodeKind::Camera = 7`（03.2）。攝影機節點只能是 Group 或 Element 的直接子節點，不繪製、不命中、不參與佈局；其**父節點成為 3D 空間根**。同一父節點下多台攝影機時取文件順序第一台且 `visible == 1` 者，其餘忽略並於載入時警告一次。攝影機節點的 `anchor/scale/skew` 忽略；`rotation/rotationX/rotationY` 只在 `useTarget == 0` 時使用。

```cpp
struct CameraData {           // NodeKind::Camera 專用資料（NodeArena::camera(id)）
    uint8_t  kind;            // 0 = Perspective, 1 = Orthographic
    uint8_t  useTarget;       // 1 = 朝向 target；0 = 由節點的 rotationX/Y/rotation 決定朝向
    uint8_t  _pad[2];
    float    zoom;            // PropId 24。焦距 px；視口高 H 時 fovY = 2·atan((H/2)/zoom)；等同 AE 的 Camera Zoom
    Vec3     target;          // PropId 25，根座標
    float    near, far;       // PropId 26/27，預設 1、100000
};
```

**投影計算**（根的參考盒 `box`：Element 為 border-box、場景為 `SceneChunk` 設計尺寸、文件根為視口；`W/H` 為其寬高，座標原點為盒左上）：

1. 攝影機位置 `eye = translate.xyz`（根座標，+z 朝觀者）。
2. 視圖：座標系是 x 右、y **下**、z 朝觀者，所以不能直接用 y 向上的 `gluLookAt`（會鏡射一軸）。攝影機框架：`b = normalize(eye − target)`（攝影機 z 軸，指向觀者）、`s = normalize(cross((0, 1, 0), b))`（右）、`d = cross(b, s)`（下）；`|cross((0,1,0), b)| < 1e-6`（正對上下看）時 `s = (1, 0, 0)`。`view` 的列為 `[s, −dot(s, eye)]`、`[d, −dot(d, eye)]`、`[b, −dot(b, eye)]`、`[0 0 0 1]`。`useTarget == 0` 時 `view = inverse(T(eye) * Rz(rotation) * Ry(rotationY) * Rx(rotationX))`（旋轉為單位矩陣時框架即 (x, y, z)，與上式一致）。
3. 投影：Perspective → `proj = mat4Perspective(fovY, W/H, near, far)`（矩陣對 y 對稱，視圖空間 y 向下不影響）；Orthographic → `proj = mat4Ortho(-W/2, W/2, -H/2, H/2, near, far)`。
4. NDC → 根的 2D 座標（**不翻轉 y**，因為視圖空間已是 y 向下）：`x2d = (ndcX + 1) * W/2`、`y2d = (ndcY + 1) * H/2`。
5. `viewProj = ndcToBox * proj * view`。

驗證式：攝影機 `(W/2, H/2, W)` 看 `(W/2, H/2, 0)`、`zoom = W` 時，z = 0 平面上的點 `(x, y, 0)` 投影回 `(x, y)`（tan(fovY/2) = (H/2)/W）。

所以透視投影的視錐永遠對稱於根盒中心；攝影機不在盒中心上方時是「平移的攝影機」而不是斜視錐，與 AE 一致。

**預設攝影機**（根沒有攝影機節點但子樹有 3D 圖層）：

- Element 根（由 CSS `perspective` 建立，19.10 對 05 的修訂）：**不走 LookAt**，直接用 CSS 的定義：`viewProj = T(ox, oy, 0) * P(d) * T(-ox, -oy, 0)`，其中 `(ox, oy)` 為 `perspective-origin` 換算的 px，`P(d)` 為 `m[3][2] = -1/d`、其餘為單位的矩陣（投影：`x' = ox + (x - ox) / (1 - z/d)`，`y'` 同理）。`perspective: none`（預設）→ 正交：`viewProj = I`（3D 旋轉退化為平面縮放，與 CSS 一致）。`near/far` 不裁切（`w ≤ 1e-6` 的點由 19.7 規則裁掉）。
- 場景根（Lottie `ddd:1` 而無攝影機圖層）：AE 預設攝影機：`eye = (W/2, H/2, +zoom)`、`target = (W/2, H/2, 0)`、`zoom = W`（AE 預設 50 mm 鏡頭於合成寬 W 的等效焦距）、`near = 1`、`far = 100000`，走上面的 1–5 步。

**文件中的攝影機**（修訂 05.2 標籤表）：`<camera>` 標籤，屬性 `type="perspective|orthographic"`（預設 perspective）、`position="x y z"`、`target="x y z"`、`zoom`、`near`、`far`。表示法：編譯器輸出一個 `IrDocElement`（tag `camera`，保留 id/class/屬性）並在 DOCT 新增 `cameras[]`（`IrCameraData`）由 `IrDocElement.cameraIndex` 指向（非攝影機為 `0xFFFFFFFF`）；載入時建立 Element 節點（不佈局、不繪製、不命中，`display` 視為 none，但保留在樹中供 `byId/query`）並建立其子 `Camera` 節點；3D 空間根 = `<camera>` 的父元素。`ElementRef::sceneNode()` 對 `<camera>` 回傳該 Camera 節點。省略的屬性在**每次佈局後**依父 border-box 重算預設值（`position = (W/2, H/2, W)`、`target = (W/2, H/2, 0)`、`zoom = W`），有屬性者為固定基底值；Track/transition/override 對攝影機 PropId 的層疊以該基底值為底（04 的既有規則）。`<camera>` 沒有 CSS 樣式與 `@keyframes`（0.3）；執行時可用 `Document::play(TimelineDesc)` 對攝影機節點下 Track。父元素若同時有 `perspective` 屬性，`<camera>` 優先，`perspective` 被忽略並警告。

## 19.5 世界變換與 Painter（修訂 01.5 步驟 4、09.3）

`updateWorldTransforms` 在進入根時開始一段「3D 遍歷」：

- 根：`world`（2×3）照舊。記 `rootId`。
- 根子樹內的 2D 節點：`world = parent2D * local`，其中 `parent2D` 是「從所在平面到父的 2D 累積」：若父是根或 3D 圖層 → `parent2D = I`；否則為父的 `world`。也就是：**根子樹內 2D 節點的 `world` 是相對所在平面的變換，不是絕對變換**（03.3 的 `world` 語意在根子樹內改為「平面相對」；命中與繪製都靠 19.7 的映射堆疊，不用絕對 2×3）。
- 3D 圖層：`world3 = P(parent) * local3`，其中 `P(parent)`：父是根 → `I`；父是 3D 圖層 → 父 `world3`；父是 2D 節點 → `Q * mat4FromMat2x3(parent.world)`，`Q` 為該 2D 節點所在平面的 `world3`（根平面時為 `I`）。
- 巢狀根（根子樹內又有攝影機的節點）：對其子樹遞迴同樣規則；它本身在外層是 2D 節點或 3D 圖層照舊。

Painter 對每個根：

1. 發出 `PushProjection { viewProj, rootXf = root.world（在外層語意下的 2×3）, boxRect = box, depthTest = 0, boundsHint }`。`boundsHint`（T-105 起）：Painter 把空間根子樹的 3D 包圍盒八個角經 `viewProj` 投影後取螢幕 AABB，再外擴子樹效果的 `effectExpand`；投影失敗（w ≤ 0）時退為整個視口。髒矩形（23.2）與 SW 局部重繪用它，之前 PushProjection 一律算整個視口。
2. 依 19.6 決定圖層順序。
3. 對每個 3D 圖層：`PushTransform3D { world3 }` → 圖層內容（自身與 2D 子孫；其命令的 `xf` 為平面相對 2×3，即 19.5 的 `world`）→ `PopTransform3D`。**不巢狀**：一個圖層內遇到 3D 子孫不會發第二層 `PushTransform3D`，該子孫是排序清單中的另一個圖層。
4. 根平面上的 2D 內容（根自身與不屬於任何 3D 圖層的 2D 子孫）以 `PushTransform3D { I }` 包住輸出。
5. `PopProjection`。

巢狀根：在其父平面的內容輸出到一半時發出第二層 `PushProjection`（`rootXf` 為該巢狀根的平面相對 `world`），後端以堆疊處理（19.7）。

命令中的 `Mat2x3 xf` 語意不變：永遠是「到當前平面」的 2D 變換。**後端不必改任何既有命令結構。**

**螢幕座標查詢**：既有依賴絕對 `world` 的 API 改用新工具 `Vec2 Document::nodeToScreen(NodeId, Vec2 localPoint)`（`Code/Runtime/core/include/xt/project3d.h` + `Code/Runtime/core/src/project3d.cpp`，公開標頭，SW、GL、簡化器共用的 CPU 投影：`projectPoint`、`projectPath`、`sMax` 估算）：從節點往上找到所在 3D 圖層（或根平面），以平面相對 `world` 變到平面，再經圖層 `world3`、根 `viewProj`、`rootXf` 與外層映射投影。使用者：`Context::textInputRect`（IME 視窗定位）、`ElementRef::boundingBox`、命中測試的反向。不受影響的：`Event.localPos`（命中已在平面上求出局部座標）、游標與選取繪製（`painter_caret` 的命令屬於圖層內容，隨平面投影）、`scroll` 位移（平面內的 2D 位移）。

## 19.6 深度排序與背面

排序在根內進行，只有 3D 圖層參與：

1. 走訪根的**直接子節點**（文件順序，Element 先依 z-index 排序）。把子節點分段：連續的「子樹含至少一個 3D 圖層」的兄弟為一段（排序段），其他兄弟各自為一段（2D 段）。
2. 2D 段：整個子樹在根平面依既有 2D 順序輸出（步驟 4）。
3. 排序段：收集段內所有兄弟子樹中的**全部 3D 圖層**（含深層），計算各圖層錨點的攝影機空間深度 `depth = (view * world3 * (anchor.xyz, 1)).z`（CSS 預設攝影機時 `view = I`，depth = 錨點 z），**由遠到近**（depth 小者先）畫；`std::sort` 於 Painter 暫存 vector，鍵為 `(depth, 文件序)`；段內的 2D 內容（不屬於任何 3D 圖層者）在該段的圖層之前於根平面輸出。

這對應 AE 的規則：2D 圖層切斷 3D 排序群，群內全域排序。0.2 不做逐像素交錯：兩個相交平面依錨點深度整層先後繪製。

**背面**：`backfaceHidden == 1` 時，取投影映射 `F(p) = project(viewProj * world3 * (p, 0, 1))` 在錨點處的 2×2 雅可比矩陣 `J`（以中央差分於 ±1 px 估算），`det(J) < 0` → 背面 → 跳過繪製與命中。`backfaceHidden` 來源：CSS `backface-visibility: hidden`（Element）、Lottie 無（恆 0）、API `setNodeProperty`。

**0.3 預留**：`ProjectionCmd.depthTest = 1` 時 GL 走深度緩衝、SW 走 z-buffer，烘焙格式帶每命令深度。

## 19.7 命令清單（修訂 09.1）

新增命令（編號接在既有 0–11 之後）：

```cpp
enum class CmdKind : uint8_t { /* 0–11 既有 */
    PushProjection  = 12,  // payload: projections
    PopProjection   = 13,
    PushTransform3D = 14,  // payload: transforms3
    PopTransform3D  = 15,
    DrawImageQuad   = 16,  // payload: imageQuads（19.8 簡化輸出用；Painter 不直接產生）
};
struct ProjectionCmd  { Mat4 viewProj; Mat2x3 rootXf; Rect boxRect; uint8_t depthTest; uint8_t _pad[3]; };
struct Transform3DCmd { Mat4 world3; };
struct ImageQuadCmd   { AssetId asset; Vec2 p[4]; Rect srcUv; float opacity; uint8_t smooth; uint8_t _pad[3]; };
// CommandList 新增：std::vector<ProjectionCmd> projections; std::vector<Transform3DCmd> transforms3; std::vector<ImageQuadCmd> imageQuads;
```

**映射堆疊**：後端維護一個「平面到螢幕」映射堆疊。初始（無投影）：`screen = xf * p`。`PushProjection` 推入 `{outer = 目前映射, viewProj, rootXf}`；`PushTransform3D` 設定目前層的 `M`（同一投影內不巢狀，`Push` 覆蓋、`Pop` 清除）。命令中一點 `p`（xf 座標）：

```
q   = xf * p                                   // 平面座標
r   = project(viewProj * M * (q.x, q.y, 0, 1)) // 根盒 2D 座標；w ≤ 1e-6 → 該幾何丟棄（整條路徑/整張圖片）
screen = outer(rootXf * r)                     // outer 為 PushProjection 時的映射（可能又是一層投影）
```

`PopProjection` 還原 `outer`。`dumpCommandList` 格式：`PushProjection viewProj=[16 floats] rootXf=[6] box=(x,y,w,h) depth=0`、`PushTransform3D [16 floats]`、`DrawImageQuad asset=N p=(x,y)(x,y)(x,y)(x,y) uv=(…) opacity=… smooth=…`、`PopProjection`、`PopTransform3D`。

後端語意：

- **GL**（10.3）：不改成 4×4 著色器管線。投影範圍內的路徑、字形外框與裁切用與 SW 相同的 CPU 投影（`Code/Runtime/core/include/xt/project3d.h`，同一份程式）轉成 2D 幾何後走既有 stencil-then-cover；圖片（`DrawImage`、`DrawImageQuad`）與字形圖集四邊形以**齊次頂點**繪製：頂點帶 `(x·w, y·w, w)` 與 uv，片段著色器靠硬體的透視除法取得透視正確的貼圖；巢狀投影因此不需要特別處理。Layer/Mask 的離屏緩衝在螢幕空間，不受影響。
- **SW**（10.2）：路徑先在平面空間扁平化，容差 = 既定容差 / `sMax`，`sMax` = 路徑包圍盒四角投影後與投影前的邊長比最大值（下限 1）；再逐點投影。圖片：`dst` 四角投影成四邊形，`homographyFromQuad(dst 四角, 投影後四角)` 得單應矩陣，反向映射每像素做透視除法後取樣。字形：外框走路徑流程。`ClipRect` 在投影下轉四邊形路徑裁切。
- 引擎最小子集（09.5）：由 `CommandListSimplifier` 展平（19.8），宿主不需理解投影。

`BakedWriter/BakedPlayer`（12.5）序列化新 payload 向量（`projections`、`transforms3`、`imageQuads`），`.xtcl` minor 版本 +1（讀舊檔仍可）。

## 19.8 簡化與烘焙

`SimplifyOptions` 新增 `bool flatten3D = true`；`simplify()` 的回傳型別改為 `SimplifyStats { uint32_t flattenedCommands, droppedGlyphRuns, droppedEffects; }`（18 章使用 `droppedEffects`）。規則順序：**先 `flatten3D`，再套用既有規則**（strokes、gradients、layers、masks、clipPaths、glyphsToPaths）。

`flatten3D` 對投影範圍內的命令：

- `FillPath/StrokePath`：以 SW 規則（19.7）扁平化並逐點投影，輸出只含 `Line` 的路徑，`xf = I`（描邊先於本步驟轉為填充：`strokes` 選項在 3D 下強制視為 true，因為投影後描邊寬度不再一致）。
- `DrawGlyphs`：需要 `shaper` 轉外框後同路徑處理；`shaper == nullptr` → 移除該命令並計入 `droppedGlyphRuns`。
- `DrawImage`：投影後四角若仍為平行四邊形（正交攝影機或 `viewProj = I`）→ 保留 `DrawImage` 並合成 `xf`；否則輸出 `DrawImageQuad`。
- `PushClipRect/PushClipPath`：投影為 `PushClipPath`（其後 `clipPaths` 選項再決定是否降為 AABB）。
- 移除 `PushProjection/PopProjection/PushTransform3D/PopTransform3D`。

`xtframe-bake` 預設 `flatten3D`，所以 `.xtcl` 播放端與引擎最小子集只需多支援 `DrawImageQuad`（四個頂點帶 UV 的貼圖三角形對，`IRenderAuxGeom::DrawBuffer` 直接對應）。

## 19.9 命中測試（修訂 08.2）

進入根時：把指標點 `s`（根盒 2D 座標，先經 `rootXf` 反變換）反投影為射線：`inv = inverse(viewProj)`，`p0 = inv * (s, -1, 1)`、`p1 = inv * (s, +1, 1)`（各自齊次除法）；CSS 預設攝影機時 `p0 = (s, +∞)` 改用 `eye = (ox, oy, d)` 與 `p1 = (s, 0)`。對每個 3D 圖層：平面為 `world3` 空間的 z = 0；把射線變到圖層空間（`inverse(world3)`），與 z = 0 求交得 `q`；`q` 落在圖層平面上後走既有 2D 命中（圖層內 2D 子孫用平面相對 `world`）。深度 = 交點的攝影機空間 z；排序段內取最近者；背面隱藏的圖層不命中；2D 段在根平面命中（等同既有流程）。`boundingBox` 回投影後四角的 AABB（再經外層映射）。

## 19.10 對既有章節的修訂條款

| 章節 | 修訂 |
|---|---|
| 02 IR | `major` 升為 2。`IrSceneNode` 由 64 bytes 改為 96 bytes：在 `_pad[2]` 位置起新增 `translateZ, anchorZ, rotationX, rotationY`（f32）、`orientation`（3×f32）、`is3D, backfaceHidden`（u8）、`_pad[2]`、`effectOffset, effectCount`（u32，18 章；合計恰為 96 bytes，無尾端填充）；`kind == Camera` 時 `kindIndex` 指向新的 `IrCameraData` 陣列（SCEN 區塊新增 `cameraCount`、`cameraOffset`，結構同 `CameraData`，32 bytes）。`IrTrack.type` 可為 `Vec3`（`IrKeyframe.value` 放 3 個 f32）；`IrKeyframe.tanOut/tanIn` 改為 3×f32（各 +4 bytes）。`IrDocElement` 新增 `uint32_t cameraIndex`（DOCT 新增 `cameras[]`）；`is3D/backfaceHidden` 對 Element 由執行時推導（19.3），不進 IR。 |
| 03 | 3.2 新增 `Camera = 7`；3.3 依 19.3 的欄位與順序；3.9 通用區改為 0–31 並新增 16–27；3.10 新增 `ValueType::Vec3`；`propertyApplies` 依 19.3。 |
| 04 | Vec3 的插值與空間貝茲；`Keyframe.tanOut/tanIn` 改為 Vec3；`Orientation` 以歐拉角線性插值（不做四元數，與 AE 一致）；Animator 對 Element 的 16–22 Track 設定 `is3D`（19.3）。 |
| 05 | 5.2 標籤表新增 `<camera>`（19.4）。5.5 `transform` 新增函式：`translateZ(len)`、`translate3d(x,y,z)`、`rotateX(angle)`、`rotateY(angle)`；`rotate3d`、`scale3d`、`matrix3d`、`perspective()` 函式 → 編譯錯誤「不支援」。順序規則改為：translate*, rotateX, rotateY, rotate, skew, scale，各至多一次。新增屬性：`perspective`（Length，`none` 編碼為 `LengthUnit::Auto`，PropId 106，可動畫）、`perspective-origin`（PropId 107/108，Length，語法同 `transform-origin`，預設 `50% 50%`）、`backface-visibility`（StyleProp 188，Enum `visible \| hidden`，不可動畫）。`transform-style` 不支援（恆 preserve-3d）。`kElementPropCount` → 109。 |
| 06 | 3D 變換不影響佈局；`ElementData` 新增佈局後換算的 `perspectivePx`（`Auto` → 0 表示 none）與 `perspectiveOriginPx`（Vec2）；`perspectivePx > 0` 的元素即 3D 空間根（無 `<camera>` 時）。 |
| 08 | 依 19.9；`Context::textInputRect` 改用 `nodeToScreen`。 |
| 09 | 依 19.7；9.5 最小子集加入 `DrawImageQuad`。 |
| 10 | 依 19.7 後端語意；10.1 新增 `Mat4/Mat3` 工具。 |
| 11 | `ddd:1` 圖層：`p/a` 第三分量（取負）→ TranslateZ/AnchorZ；`rx/ry/rz` → RotationX/RotationY/Rotation；`or` → Orientation（三者可同時存在，依 19.2 公式相乘）；`ty:13` 攝影機圖層 → Camera 節點（`pe` → zoom、`p` → 位置（z 取負）、`a` → target（z 取負）、`useTarget = 1`；AE 單節點攝影機（無 `a`）→ `useTarget = 0` 用 `rx/ry/rz`）；無攝影機時用 19.4 預設。原「警告忽略」條款刪除。 |
| 12 | 編譯器據 05 產生 `<camera>` 元素與 DOCT `cameras[]`、Lottie 場景節點的 `is3D`；`.xtcl` minor +1。 |
| 13 | `ElementRef::boundingBox` 語意更新；新增 `Document::setNode3D(NodeId, bool)`（場景節點）、`NodeId Document::cameraOf(NodeId root)`（無 → `kInvalidNode`）、`Vec2 Document::nodeToScreen(NodeId, Vec2)`、`Document::setNodeProperty(NodeId, PropId, Value)`（13.3 提及但 0.1 未做，本章要求）、`float Document::animationTime(uint32_t instance)`（17 章流圖用）。 |
| 15 | 黃金圖 `ui_3d_card_flip`（`rotateY(60deg)` 中間幀）、`ui_3d_text_perspective`、`ui_3d_sort_two_layers`、`ui_3d_backface_hidden`、`ui_3d_camera_tag`；`lottie_3d_layer`、`lottie_3d_camera_anim`。GL/SW 互比沿用 15.3 的 16/255、3%。 |
| 16 | M7 任務 T-42～T-49（16.2.2）。 |

## 19.11 參考

- After Effects 3D 圖層模型（2D 圖層切斷 3D 排序群、Camera Zoom 定義、Orientation 與 Rotation 分離且相乘）：概念參考，無原始碼。
- CSS Transforms Module Level 2：`perspective`/`perspective-origin` 的投影矩陣定義（19.4 預設攝影機直接採用）。
- lottie-web `elements/HCameraElement.js`：Lottie 攝影機到 CSS 3D 的映射（`pe` → perspective、目標點 → lookAt、z 取負）；只取映射規則。
- Glaxnimate `src/core/io/lottie/lottie_exporter.hpp` 把 `ddd` 硬寫為 0：反面教材，我們在 IR 一開始就帶 3D 欄位。
- CryEngine `CryCommon/CryMath/Cry_Matrix44.h`、`Cry_Matrix34.h`、`Cry_Camera.h`：`Mat4` 採同樣的列主序 `mRC` 命名與欄向量 `M * p` 慣例、平移在每列最後一欄（`m03/m13/m23`），`TransformPoint/TransformVector` 的分工對應 `mat4Transform/mat4TransformVector`，`CCamera::Project` 的 NDC → 像素 y 翻轉對應 19.4 步驟 4；刻意不同：我們用度、座標為 x 右 y 下 +z 朝觀者（CryEngine 為 Z 上 Y 前），轉換攝影機需重映射軸而非轉置；CryEngine 攝影機渲染路徑用列向量 `v * M`，接收其 `Matrix44` 時要先確認慣例。

## 19.9 投影下的漸層（T-160）

投影下路徑在平面空間扁平化再逐點投影，但漸層必須在**平面空間**求值：SW 以路徑平面 AABB 四角與投影後四角求「螢幕→平面」單應矩陣（`homographyFromQuad`），`PaintEval` 逐像素算 t；GPU 的漸層頂點帶平面 u,v 與齊次 w 做透視正確內插。`flatten3D`（19.8）的簡化器同樣要注意（未改）。
