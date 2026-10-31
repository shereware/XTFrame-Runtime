/// The AVX2 build of every kernel. This translation unit is compiled with /arch:AVX2 (see
/// Code/Runtime/sw/CMakeLists.txt), so the identical `xt::simd::f32x4` source in sw_simd_kernels.h becomes
/// VEX-encoded and free to use 256-bit registers where the compiler widens a loop -- and nothing
/// else: it is built without FMA and without floating-point contraction, so its output is
/// byte-identical to the SSE2 table's (Docs/design/23-host-integration.md §23.5, T-529). `XT_SW_KERNEL_NS`
/// puts these instantiations in their own namespace so the linker cannot fold them into
/// sw_simd_dispatch.cpp's SSE2 ones. Only reachable after `swCpuHasAvx2()`.
///
/// Not compiled for non-x86 targets, where sw_simd_dispatch.cpp never calls it; it still has to
/// define the symbol, so the whole body is guarded.

#define XT_SW_KERNEL_NS simdk_avx2
#include "sw_simd_kernels.h"

namespace xt::sw {

const SwSimdOps& swSimdAvx2Ops() {
    static const SwSimdOps ops = simdk_avx2::makeOps<simd::f32x4>("avx2");
    return ops;
}

}  // namespace xt::sw
