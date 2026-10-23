#pragma once

#include "xt/wgpu/wgpu_renderer.h"

namespace xt::wgpu::test {

/// The executable's headless device (test_wgpu_main.cpp); the cases only run when it exists.
const WgpuInit& sharedInit();

}  // namespace xt::wgpu::test
