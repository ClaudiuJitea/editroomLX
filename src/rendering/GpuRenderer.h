// The GPU compositor (Vulkan compute). One FrameJob is one command buffer: upload what changed, the
// composite (rendering.comp), optionally the NV12 pack (pack.comp), and one readback.
//
// Device choice (IMPLEMENTATION.md §5.4): a discrete GPU, then an integrated one. Mesa's lavapipe (a CPU
// Vulkan) only when asked (`allowSoftware`, used by the tests, or EDITROOM_VK_ALLOW_CPU=1), since the
// OpenMP path is faster on the same CPU. EDITROOM_GPU=cpu turns the GPU off; EDITROOM_VK_DEVICE=<text>
// picks the device whose name contains it.
#pragma once
#include "RenderPlan.h"
#include <QImage>
#include <QString>
#include <memory>

namespace er {

class GpuRenderer {
public:
    struct Info {
        QString device;
        QString driver;
        QString api;
        bool software = false;
        bool discrete = false;
    };

    /// Nullptr when there's no usable Vulkan device (the reason in `why`).
    static std::unique_ptr<GpuRenderer> create(bool allowSoftware = false, QString* why = nullptr);
    /// Whether a device can be had at all, cached (for tests: QSKIP when false).
    static bool available(bool allowSoftware = true);
    ~GpuRenderer();

    const Info& info() const;

    std::vector<uint8_t> render(const FrameJob& job);
    std::vector<float> renderFloat(const FrameJob& job);
    std::vector<uint8_t> renderNV12(const FrameJob& job);
    /// An HDR project's export frame as P010 (BT.2020 limited range), packed on the GPU (pack.comp): read back at
    /// 3 bytes a pixel, not the composite's 16.
    std::vector<uint16_t> renderP010(const FrameJob& job);

    /// The VkInstance, for making a window's surface on it (QVulkanInstance::setVkInstance); none when presenting
    /// isn't possible (no surface or swapchain support).
    void* vkInstance() const;
    /// The window surface (a VkSurfaceKHR) frames are presented to, at its size in device pixels; none to stop
    /// (call that before the surface goes). False when this device can't present to it.
    bool setSurface(void* surface, int width, int height);
    /// Composes the frame and presents it in the window: nothing read back. False when it couldn't (then render).
    bool present(const FrameJob& job);
    /// The part of the frame the window shows (0…1 across and down): all of it, or what's in view of a zoomed viewer.
    void setPresentRegion(double x0, double y0, double x1, double y1);

    /// DMA-BUF frames imported so far (decoded frames that never went through memory).
    long long dmabufImports() const;
    /// Tests: the next presented image is copied back; then presentedImage() has it (as the window shows it).
    void capturePresented();
    QImage presentedImage() const;

    /// Milliseconds the GPU spent on the last frame (timestamp queries), or -1.
    double lastGpuMs() const;

private:
    GpuRenderer();
    struct Impl;
    std::unique_ptr<Impl> d;
    std::vector<uint8_t> run(const FrameJob& job, int mode);
};

} // namespace er
