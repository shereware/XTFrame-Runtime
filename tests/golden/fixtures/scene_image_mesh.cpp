#include <cstdint>
#include <vector>

#include "../golden_common.h"
#include "xt/sw/sw_png.h"

/// T-245 (docs/design/32-rigging-and-deformation.md §32.2.4): a 32x32 test image (a colour
/// gradient with a white grid every 8 texels) drawn as a 3x3-pin, 8-triangle image mesh whose
/// pins are moved off their rest grid, so each triangle is a different affine warp. Positions and
/// uvs are written straight into the scene's mesh pools (the uvs stay on the rest grid), which no
/// `<xt-mesh>` source can express before skinning (T-244). The image is embedded (PNG bytes), so
/// the fixture needs an AssetResolver: it is compiled into xt_golden only (golden_main.cpp passes
/// one), not into xt_golden_fixtures, whose other users render without one.

using namespace xt;
using namespace xt::golden;

namespace {

std::vector<uint8_t> makeTestImagePng() {
    constexpr int kSize = 32;
    std::vector<uint8_t> rgba(static_cast<size_t>(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            uint8_t* p = rgba.data() + (static_cast<size_t>(y) * kSize + static_cast<size_t>(x)) * 4;
            const bool grid = (x % 8) == 0 || (y % 8) == 0;
            p[0] = grid ? 255 : static_cast<uint8_t>(x * 8);
            p[1] = grid ? 255 : static_cast<uint8_t>(y * 8);
            p[2] = grid ? 255 : static_cast<uint8_t>(255 - x * 4 - y * 4);
            p[3] = 255;
        }
    }
    std::vector<uint8_t> png;
    sw::encodePng(rgba.data(), kSize, kSize, kSize * 4, png);
    return png;
}

}  // namespace

XT_GOLDEN_FIXTURE(scene_image_mesh, 256, 256, 1.0f, 0.0f, (Color{0.1f, 0.1f, 0.1f, 1}), buildSceneImageMesh) {
    compile::AssetBuilder image;
    image.name = writer.intern("mesh_test.png");
    image.kind = 0;
    image.embedded = makeTestImagePng();
    const AssetId asset = writer.addAsset(image);

    compile::SceneBuilder scene;
    scene.width = 256;
    scene.height = 256;

    // 3x3 pins; rest grid 0, 100, 200 on both axes (uv 0, 0.5, 1), moved off it below.
    const Vec2 positions[9] = {
        {0, 10},   {110, -10}, {200, 0},   //
        {-15, 95}, {130, 120}, {215, 90},  //
        {10, 200}, {85, 215},  {190, 195},
    };
    const Vec2 uvs[9] = {
        {0, 0},    {0.5f, 0},    {1, 0},     //
        {0, 0.5f}, {0.5f, 0.5f}, {1, 0.5f},  //
        {0, 1},    {0.5f, 1},    {1, 1},
    };
    const uint16_t tris[24] = {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4, 3, 4, 7, 3, 7, 6, 4, 5, 8, 4, 8, 7};
    scene.meshVec2Pool.assign(positions, positions + 9);
    scene.meshVec2Pool.insert(scene.meshVec2Pool.end(), uvs, uvs + 9);
    scene.meshIndexPool.assign(tris, tris + 24);
    IrMeshData mesh{};
    mesh.asset = asset;
    mesh.posOffset = 0;
    mesh.vertexCount = 9;
    mesh.indexOffset = 0;
    mesh.indexCount = 24;
    mesh.opacity = 1;
    mesh.smooth = 1;
    mesh.skinIndex = kIrNoSkin;
    scene.meshes.push_back(mesh);

    const uint32_t root = addNode(scene, NodeKind::Group, kRootParent);
    NodeOpts opts;
    opts.translate = Vec2{28, 24};
    addNode(scene, NodeKind::Mesh, root, 0, opts);
    writer.addScene(scene);
}
