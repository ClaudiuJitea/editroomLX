// The CPU compositor: Kernel.inc run per pixel, rows in parallel with OpenMP. The same results as the
// Vulkan path (tests/gpu compares them), for machines without Vulkan and for headless tests.
#pragma once
#include "RenderPlan.h"

namespace er {

namespace CpuRenderer {
/// The frame as RGBA8 (opaque), `outW × outH × 4` bytes.
std::vector<uint8_t> render(const FrameJob& job);
/// The frame as float RGBA (premultiplied), for comparing with the GPU before rounding.
std::vector<float> renderFloat(const FrameJob& job);
/// NV12, BT.709 limited range (the encoder's input): Y plane then interleaved CbCr.
std::vector<uint8_t> renderNV12(const FrameJob& job);
} // namespace CpuRenderer

} // namespace er
