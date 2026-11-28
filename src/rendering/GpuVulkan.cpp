#include "GpuRenderer.h"
#include "pack_spv.h"
#include "rendering_spv.h"
#include <QByteArray>
#include <QLoggingCategory>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <map>
#include <vulkan/vulkan.h>

Q_LOGGING_CATEGORY(lcGpu, "editroom.gpu")

namespace er {

namespace {

constexpr int kBindings = 7;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

const char* typeName(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

int rank(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}

} // namespace

struct GpuRenderer::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline compose = VK_NULL_HANDLE, pack = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    float timestampPeriod = 0;
    double lastMs = -1;
    Buffer bufs[kBindings];
    bool setDirty = true;
    Info info;
    // What the pixel buffer holds now: the keys of the pictures, so unchanged ones aren't copied again.
    std::vector<uint64_t> uploadedKeys;
    std::vector<int> uploadedOffsets;
    std::mutex lock;
    // Presenting (the viewer's swapchain): what the instance and device can do, and the swapchain on one surface.
    bool surfaceExt = false, swapchainExt = false;
    // DMA-BUF frames imported: each layer an image on the dma-buf's memory, kept for a few frames (a paused frame
    // isn't imported again).
    bool dmabufExt = false;
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
    struct Imported {
        VkImage image[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDeviceMemory memory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        std::shared_ptr<const DmaBuf> keep;
        uint64_t used = 0;
    };
    std::map<uint64_t, Imported> imported;
    uint64_t importClock = 0;
    std::map<std::pair<int, uint64_t>, bool> importable;   // (format, modifier) the device takes
    int importFailures = 0;
    long long importedCount = 0;

    void dropImported(Imported& im) {
        for (int l = 0; l < 2; ++l) {
            if (im.image[l]) vkDestroyImage(device, im.image[l], nullptr);
            if (im.memory[l]) vkFreeMemory(device, im.memory[l], nullptr);
        }
        im = Imported();
    }

    static VkFormat layerFormat(uint32_t fourcc) {
        switch (fourcc) {
        case 0x20203852: return VK_FORMAT_R8_UNORM;     // R8
        case 0x38385247: return VK_FORMAT_R8G8_UNORM;   // GR88
        case 0x20363152: return VK_FORMAT_R16_UNORM;    // R16
        case 0x32335247: return VK_FORMAT_R16G16_UNORM; // GR32
        default: return VK_FORMAT_UNDEFINED;
        }
    }

    bool canImport(VkFormat fmt, uint64_t modifier) {
        const auto k = std::make_pair(int(fmt), modifier);
        if (auto it = importable.find(k); it != importable.end()) return it->second;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        mi.drmFormatModifier = modifier;
        mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        ei.pNext = &mi;
        VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        fi.format = fmt;
        fi.type = VK_IMAGE_TYPE_2D;
        fi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        fi.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        fi.pNext = &ei;
        VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        fp.pNext = &ep;
        const bool ok = vkGetPhysicalDeviceImageFormatProperties2(physical, &fi, &fp) == VK_SUCCESS
            && (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
        importable[k] = ok;
        return ok;
    }

    bool importLayer(const DmaBuf::Layer& L, VkImage& img, VkDeviceMemory& mem) {
        const VkFormat fmt = layerFormat(L.fourcc);
        if (fmt == VK_FORMAT_UNDEFINED || !canImport(fmt, L.modifier)) return false;
        VkSubresourceLayout layout{};
        layout.offset = VkDeviceSize(L.offset);
        layout.rowPitch = VkDeviceSize(L.pitch);
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
        mod.drmFormatModifier = L.modifier;
        mod.drmFormatModifierPlaneCount = 1;
        mod.pPlaneLayouts = &layout;
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        ext.pNext = &mod;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.pNext = &ext;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = VkExtent3D{uint32_t(L.width), uint32_t(L.height), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ii, nullptr, &img) != VK_SUCCESS) { img = VK_NULL_HANDLE; return false; }
        const int fd = dup(L.fd);
        VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, img, &req);
        if (fd < 0 || getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp) != VK_SUCCESS) {
            if (fd >= 0) close(fd);
            vkDestroyImage(device, img, nullptr);
            img = VK_NULL_HANDLE;
            return false;
        }
        const int type = memoryType(req.memoryTypeBits & fdp.memoryTypeBits, 0);
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = img;
        VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        imp.fd = fd;
        imp.pNext = &ded;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = uint32_t(type);
        // On success the driver owns the fd.
        if (type < 0 || vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) {
            close(fd);
            vkDestroyImage(device, img, nullptr);
            img = VK_NULL_HANDLE;
            mem = VK_NULL_HANDLE;
            return false;
        }
        vkBindImageMemory(device, img, mem, 0);
        return true;
    }

    /// The frame's layers as images, imported now or before; none when the device can't take them.
    Imported* importFrame(const VideoFrame& f) {
        if (!dmabufExt || !f.dmabuf || f.dmabuf->layers.size() != 2 || importFailures > 8) return nullptr;
        if (auto it = imported.find(f.key); it != imported.end()) { it->second.used = ++importClock; return &it->second; }
        // A few at a time: the oldest goes (the previous frame's copy has finished: every frame waits for its fence).
        while (imported.size() >= 6) {
            auto oldest = imported.begin();
            for (auto it = imported.begin(); it != imported.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
            dropImported(oldest->second);
            imported.erase(oldest);
        }
        Imported im;
        for (int l = 0; l < 2; ++l)
            if (!importLayer(f.dmabuf->layers[size_t(l)], im.image[l], im.memory[l])) {
                dropImported(im);
                ++importFailures;
                return nullptr;
            }
        im.keep = f.dmabuf;
        im.used = ++importClock;
        ++importedCount;
        return &(imported[f.key] = im);
    }
    struct Present {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{0, 0};
        std::vector<VkImage> images;
        VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
        // The composite as an image, for the blit (which scales and swaps channels as the window needs).
        VkImage staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        int stagingW = 0, stagingH = 0;
        bool stale = true;
        int wantW = 0, wantH = 0;
        // The part of the composite shown (a zoomed viewer shows what's in view).
        double x0 = 0, y0 = 0, x1 = 1, y1 = 1;
        // Tests: the next presented image copied back (BGRA or RGBA as the window has it).
        bool capture = false;
        Buffer captured;
        std::vector<uint8_t> lastCapture;
        bool capturedBgr = false;
        int capturedW = 0, capturedH = 0;
    } present;

    bool ensureCapture(size_t bytes) {
        Buffer& b = present.captured;
        if (b.size >= bytes) return true;
        release(b);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        const int type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = uint32_t(type);
        if (type < 0 || vkAllocateMemory(device, &ai, nullptr, &b.memory) != VK_SUCCESS) { release(b); return false; }
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        b.size = bytes;
        return true;
    }

    void dropSwapchain() {
        if (present.swapchain) vkDestroySwapchainKHR(device, present.swapchain, nullptr);
        present.swapchain = VK_NULL_HANDLE;
        present.images.clear();
    }
    void dropStaging() {
        if (present.staging) vkDestroyImage(device, present.staging, nullptr);
        if (present.stagingMemory) vkFreeMemory(device, present.stagingMemory, nullptr);
        present.staging = VK_NULL_HANDLE;
        present.stagingMemory = VK_NULL_HANDLE;
        present.stagingW = present.stagingH = 0;
    }
    void dropPresent() {
        if (!device) return;
        vkDeviceWaitIdle(device);
        dropSwapchain();
        dropStaging();
        if (present.acquired) vkDestroySemaphore(device, present.acquired, nullptr);
        if (present.rendered) vkDestroySemaphore(device, present.rendered, nullptr);
        release(present.captured);
        present = Present();
    }

    /// The swapchain for the surface at this size (made again when the window changed).
    bool makeSwapchain(int w, int h) {
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, present.surface, &caps) != VK_SUCCESS) return false;
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu) extent = VkExtent2D{uint32_t(std::max(1, w)), uint32_t(std::max(1, h))};
        if (extent.width == 0 || extent.height == 0) return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, present.surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, present.surface, &n, formats.data());
        // Plain 8-bit UNORM, as the painted QImage is: the composite's values go to the screen as they are.
        VkSurfaceFormatKHR pick{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (VkFormat f : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
            for (const auto& sf : formats) if (sf.format == f && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && pick.format == VK_FORMAT_UNDEFINED) pick = sf;
        if (pick.format == VK_FORMAT_UNDEFINED) return false;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical, pick.format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return false;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = present.surface;
        ci.minImageCount = std::max(caps.minImageCount, 2u) + 1;
        if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = pick.format;
        ci.imageColorSpace = pick.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                          : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        // Mailbox when there is one: presenting never waits for the screen (on Wayland, FIFO can block for as long
        // as the window is hidden, and with it the render thread). FIFO otherwise, which every driver has.
        uint32_t pmn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, present.surface, &pmn, nullptr);
        std::vector<VkPresentModeKHR> modes(pmn);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, present.surface, &pmn, modes.data());
        ci.presentMode = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end() ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = present.swapchain;
        VkSwapchainKHR made = VK_NULL_HANDLE;
        vkDeviceWaitIdle(device);
        const VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &made);
        dropSwapchain();
        if (r != VK_SUCCESS) return false;
        present.swapchain = made;
        present.format = pick.format;
        present.extent = extent;
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device, made, &count, nullptr);
        present.images.resize(count);
        vkGetSwapchainImagesKHR(device, made, &count, present.images.data());
        if (!present.acquired) {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(device, &si, nullptr, &present.acquired);
            vkCreateSemaphore(device, &si, nullptr, &present.rendered);
        }
        present.stale = false;
        return true;
    }

    bool makeStaging(int w, int h) {
        if (present.staging && present.stagingW == w && present.stagingH == h) return true;
        dropStaging();
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = VkExtent3D{uint32_t(w), uint32_t(h), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ii, nullptr, &present.staging) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, present.staging, &req);
        int type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = memoryType(req.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = uint32_t(type);
        if (type < 0 || vkAllocateMemory(device, &ai, nullptr, &present.stagingMemory) != VK_SUCCESS) { dropStaging(); return false; }
        vkBindImageMemory(device, present.staging, present.stagingMemory, 0);
        present.stagingW = w;
        present.stagingH = h;
        return true;
    }

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            dropPresent();
            for (auto& [k, im] : imported) dropImported(im);
            imported.clear();
            for (auto& b : bufs) release(b);
            if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
            if (descPool) vkDestroyDescriptorPool(device, descPool, nullptr);
            if (compose) vkDestroyPipeline(device, compose, nullptr);
            if (pack) vkDestroyPipeline(device, pack, nullptr);
            if (pipeLayout) vkDestroyPipelineLayout(device, pipeLayout, nullptr);
            if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void release(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device, b.memory);
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);
        b = Buffer();
    }

    int memoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    /// Host-visible storage buffers: device-local too when the driver offers it (integrated GPUs and
    /// resizable BAR), so the shader reads them where they are.
    bool ensure(int binding, VkDeviceSize size) {
        size = std::max<VkDeviceSize>(size, 256);
        Buffer& b = bufs[binding];
        if (b.size >= size) return true;
        release(b);
        VkDeviceSize grown = std::max(size, b.size * 3 / 2);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = grown;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        int type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = uint32_t(type);
        if (vkAllocateMemory(device, &ai, nullptr, &b.memory) != VK_SUCCESS) { release(b); return false; }
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        b.size = grown;
        setDirty = true;
        if (binding == 3) { uploadedKeys.clear(); uploadedOffsets.clear(); }
        return true;
    }

    VkPipeline makePipeline(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = bytes;
        si.pCode = code;
        VkShaderModule module;
        if (vkCreateShaderModule(device, &si, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = module;
        pi.stage.pName = "main";
        pi.layout = pipeLayout;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
        vkDestroyShaderModule(device, module, nullptr);
        return p;
    }

    bool init(bool allowSoftware, QString* why) {
        auto fail = [&](const QString& w) { if (why) *why = w; return false; };
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Editroom LX";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        // Surfaces for the viewer's swapchain, where the platform has them (none headless; then frames are read back).
        uint32_t en = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> have(en);
        vkEnumerateInstanceExtensionProperties(nullptr, &en, have.data());
        auto has = [&](const char* name) { for (const auto& e : have) if (!std::strcmp(e.extensionName, name)) return true; return false; };
        std::vector<const char*> exts;
        if (has("VK_KHR_surface")) {
            exts.push_back("VK_KHR_surface");
            for (const char* e : {"VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface"}) if (has(e)) exts.push_back(e);
            surfaceExt = exts.size() > 1;
            if (!surfaceExt) exts.clear();
        }
        ci.enabledExtensionCount = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return fail(QStringLiteral("vkCreateInstance failed"));
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devices(n);
        vkEnumeratePhysicalDevices(instance, &n, devices.data());
        const QByteArray want = qgetenv("EDITROOM_VK_DEVICE");
        allowSoftware = allowSoftware || qgetenv("EDITROOM_VK_ALLOW_CPU") == "1";
        int best = -1;
        for (auto pd : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(pd, &props);
            if (props.apiVersion < VK_API_VERSION_1_1) continue;
            if (!want.isEmpty() && !QByteArray(props.deviceName).contains(want)) continue;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !allowSoftware && want.isEmpty()) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
            for (uint32_t q = 0; q < qn; ++q) {
                if (!(qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
                if (rank(props.deviceType) > best) {
                    best = rank(props.deviceType);
                    physical = pd;
                    queueFamily = q;
                    timestampPeriod = qs[q].timestampValidBits ? props.limits.timestampPeriod : 0;
                }
                break;
            }
        }
        if (!physical) return fail(QStringLiteral("no Vulkan device with compute"));
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        info.device = QString::fromUtf8(props.deviceName);
        info.software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        info.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        info.api = QStringLiteral("%1.%2.%3").arg(VK_API_VERSION_MAJOR(props.apiVersion)).arg(VK_API_VERSION_MINOR(props.apiVersion)).arg(VK_API_VERSION_PATCH(props.apiVersion));
        info.driver = QStringLiteral("%1 (%2)").arg(props.driverVersion).arg(QLatin1String(typeName(props.deviceType)));
        vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        uint32_t dn = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &dn, nullptr);
        std::vector<VkExtensionProperties> dev(dn);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &dn, dev.data());
        auto devHas = [&](const char* name) { for (const auto& e : dev) if (!std::strcmp(e.extensionName, name)) return true; return false; };
        std::vector<const char*> devExts;
        if (surfaceExt && devHas("VK_KHR_swapchain")) { swapchainExt = true; devExts.push_back("VK_KHR_swapchain"); }
        // Importing decoded frames as DMA-BUFs (tiled, with their DRM format modifier).
        const char* dmaExts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list"};
        dmabufExt = qEnvironmentVariable("EDITROOM_DMABUF") != QLatin1String("0");
        for (const char* e : dmaExts) if (!devHas(e)) dmabufExt = false;
        if (dmabufExt) for (const char* e : dmaExts) devExts.push_back(e);
        di.enabledExtensionCount = uint32_t(devExts.size());
        di.ppEnabledExtensionNames = devExts.data();
        if (vkCreateDevice(physical, &di, nullptr, &device) != VK_SUCCESS) return fail(QStringLiteral("vkCreateDevice failed"));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        if (dmabufExt) {
            getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
            if (!getMemoryFdProperties) dmabufExt = false;
        }

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cai, &cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device, &fi, nullptr, &fence);

        VkDescriptorSetLayoutBinding b[kBindings];
        for (int i = 0; i < kBindings; ++i) b[i] = {uint32_t(i), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = kBindings;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        vkCreatePipelineLayout(device, &pl, nullptr, &pipeLayout);
        compose = makePipeline(er_rendering_spv, sizeof(er_rendering_spv));
        pack = makePipeline(er_pack_spv, sizeof(er_pack_spv));
        if (!compose || !pack) return fail(QStringLiteral("compute pipelines failed"));

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        vkCreateDescriptorPool(device, &dpi, nullptr, &descPool);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        vkAllocateDescriptorSets(device, &dai, &set);
        if (timestampPeriod > 0) {
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 2;
            vkCreateQueryPool(device, &qpi, nullptr, &timestamps);
        }
        for (int i = 0; i < kBindings; ++i) if (!ensure(i, 256)) return fail(QStringLiteral("buffer allocation failed"));
        qCInfo(lcGpu) << "Vulkan device" << info.device << info.driver << "API" << info.api;
        return true;
    }

    void writeSet() {
        if (!setDirty) return;
        VkDescriptorBufferInfo bi[kBindings];
        VkWriteDescriptorSet w[kBindings];
        for (int i = 0; i < kBindings; ++i) {
            bi[i] = {bufs[i].buffer, 0, VK_WHOLE_SIZE};
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = set;
            w[i].dstBinding = uint32_t(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device, kBindings, w, 0, nullptr);
        setDirty = false;
    }
};

GpuRenderer::GpuRenderer() : d(std::make_unique<Impl>()) {}
GpuRenderer::~GpuRenderer() = default;

std::unique_ptr<GpuRenderer> GpuRenderer::create(bool allowSoftware, QString* why) {
    if (qgetenv("EDITROOM_GPU") == "cpu") {
        if (why) *why = QStringLiteral("EDITROOM_GPU=cpu");
        return nullptr;
    }
    std::unique_ptr<GpuRenderer> r(new GpuRenderer());
    if (!r->d->init(allowSoftware, why)) return nullptr;
    return r;
}

bool GpuRenderer::available(bool allowSoftware) {
    static int cached[2] = {-1, -1};
    int& c = cached[allowSoftware ? 1 : 0];
    if (c < 0) c = create(allowSoftware) ? 1 : 0;
    return c == 1;
}

const GpuRenderer::Info& GpuRenderer::info() const { return d->info; }
double GpuRenderer::lastGpuMs() const { return d->lastMs; }

static size_t align4(size_t v) { return (v + 3) & ~size_t(3); }

std::vector<uint8_t> GpuRenderer::run(const FrameJob& job, int mode) {
    std::lock_guard g(d->lock);
    Impl& I = *d;
    const int w = job.frame.outW, h = job.frame.outH;
    if (w <= 0 || h <= 0) return {};

    // Pictures into one buffer, at absolute offsets. A picture already there (same key, same place)
    // isn't copied again: stills, text and a paused frame cost nothing after the first upload.
    std::vector<int> base(job.pictures.size());
    size_t total = 0;
    for (size_t i = 0; i < job.pictures.size(); ++i) {
        base[i] = int(total);
        total += align4(job.pictures[i].valid() ? job.pictures[i].byteSize() : 0);
    }
    if (!I.ensure(3, total)) return {};
    // Frames still on the GPU (DMA-BUFs) are copied in there by the GPU, below; the rest from memory.
    std::vector<Impl::Imported*> gpuCopy(job.pictures.size(), nullptr);
    for (size_t i = 0; i < job.pictures.size(); ++i) {
        const auto& p = job.pictures[i];
        const bool same = i < I.uploadedKeys.size() && p.key && I.uploadedKeys[i] == p.key && I.uploadedOffsets[i] == base[i];
        if (same) continue;
        if (p.data) { std::memcpy(static_cast<uint8_t*>(I.bufs[3].mapped) + base[i], p.data->data(), p.data->size()); continue; }
        if (!p.dmabuf) continue;
        if ((gpuCopy[i] = I.importFrame(p))) continue;
        if (const auto* b = p.bytes()) std::memcpy(static_cast<uint8_t*>(I.bufs[3].mapped) + base[i], b->data(), std::min(b->size(), p.byteSize()));
    }
    I.uploadedKeys.resize(job.pictures.size());
    I.uploadedOffsets = base;
    for (size_t i = 0; i < job.pictures.size(); ++i) I.uploadedKeys[i] = job.pictures[i].key;

    std::vector<cm::ErSource> sources = job.sources;
    for (size_t i = 0; i < sources.size(); ++i) {
        sources[i].offset0 += base[i];
        sources[i].offset1 += base[i];
        sources[i].offset2 += base[i];
    }
    const size_t pixelsOut = size_t(w) * h;
    const size_t nv12Bytes = pixelsOut + size_t((w + 1) / 2) * ((h + 1) / 2) * 2;
    // P010 (mode 4, an HDR export): 16-bit samples, the Y plane then interleaved CbCr.
    const size_t p010Bytes = (pixelsOut + size_t((w + 1) / 2) * ((h + 1) / 2) * 2) * 2;
    const size_t outBytes = mode == 2 ? align4(nv12Bytes) : mode == 4 ? align4(p010Bytes) : pixelsOut * 4;
    const bool floats = mode == 1 || mode == 2 || mode == 4;
    if (!I.ensure(0, sizeof(cm::ErFrame)) || !I.ensure(1, job.layers.size() * sizeof(cm::ErLayer)) ||
        !I.ensure(2, sources.size() * sizeof(cm::ErSource)) || !I.ensure(4, job.cubes.size() * sizeof(float)) ||
        !I.ensure(5, outBytes) || !I.ensure(6, floats ? pixelsOut * 16 : 16))
        return {};
    std::memcpy(I.bufs[0].mapped, &job.frame, sizeof(cm::ErFrame));
    if (!job.layers.empty()) std::memcpy(I.bufs[1].mapped, job.layers.data(), job.layers.size() * sizeof(cm::ErLayer));
    if (!sources.empty()) std::memcpy(I.bufs[2].mapped, sources.data(), sources.size() * sizeof(cm::ErSource));
    if (!job.cubes.empty()) std::memcpy(I.bufs[4].mapped, job.cubes.data(), job.cubes.size() * sizeof(float));
    I.writeSet();

    // Presenting: an image of the window's swapchain to blit into.
    uint32_t imageIndex = 0;
    if (mode == 3) {
        auto& P = I.present;
        if (!P.surface) return {};
        if ((P.stale || !P.swapchain) && !I.makeSwapchain(P.wantW, P.wantH)) return {};
        if (!I.makeStaging(w, h)) return {};
        VkResult r = vkAcquireNextImageKHR(I.device, P.swapchain, UINT64_C(1000000000), P.acquired, VK_NULL_HANDLE, &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!I.makeSwapchain(P.wantW, P.wantH)) return {};
            r = vkAcquireNextImageKHR(I.device, P.swapchain, UINT64_C(1000000000), P.acquired, VK_NULL_HANDLE, &imageIndex);
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { P.stale = true; return {}; }
        if (r == VK_SUBOPTIMAL_KHR) P.stale = true;
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(I.cmd, 0);
    vkBeginCommandBuffer(I.cmd, &begin);
    if (I.timestamps) {
        vkCmdResetQueryPool(I.cmd, I.timestamps, 0, 2);
        vkCmdWriteTimestamp(I.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, I.timestamps, 0);
    }
    // DMA-BUF frames: each layer copied (untiled by the GPU) into the picture's place in the pixel buffer, in the
    // same packed layout the CPU copy would have.
    bool copied = false;
    for (size_t i = 0; i < job.pictures.size(); ++i) {
        Impl::Imported* im = gpuCopy[i];
        if (!im) continue;
        const auto& p = job.pictures[i];
        for (int l = 0; l < 2; ++l) {
            const auto& L = im->keep->layers[size_t(l)];
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            b.dstQueueFamilyIndex = I.queueFamily;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.image = im->image[l];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(I.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy c{};
            c.bufferOffset = VkDeviceSize(base[i] + p.offset[l]);
            c.bufferRowLength = uint32_t(p.stride[l] / L.bytesPerTexel);
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = VkExtent3D{uint32_t(L.width), uint32_t(L.height), 1};
            vkCmdCopyImageToBuffer(I.cmd, im->image[l], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, I.bufs[3].buffer, 1, &c);
        }
        copied = true;
    }
    if (copied) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(I.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    vkCmdBindDescriptorSets(I.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.pipeLayout, 0, 1, &I.set, 0, nullptr);
    const int composeMode = floats ? 1 : 0;   // floats for renderFloat and the NV12 and P010 packs
    vkCmdPushConstants(I.cmd, I.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &composeMode);
    vkCmdBindPipeline(I.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.compose);
    vkCmdDispatch(I.cmd, uint32_t((w + 15) / 16), uint32_t((h + 15) / 16), 1);
    if (mode == 2 || mode == 4) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(I.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(I.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, I.pack);
        const int packMode = mode == 4 ? 1 : 0;   // 0: NV12, 1: P010
        vkCmdPushConstants(I.cmd, I.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &packMode);
        const uint32_t words = mode == 4 ? uint32_t(p010Bytes / 4 + 1)
                                         : uint32_t((pixelsOut + 3) / 4 + ((size_t((w + 1) / 2) * ((h + 1) / 2) * 2) + 3) / 4);
        vkCmdDispatch(I.cmd, (words + 63) / 64, 1, 1);
    }
    if (mode == 3) {
        // The composite into the staging image, then blitted (scaled, channels as the window wants) into the
        // swapchain image, which is handed to the window.
        auto& P = I.present;
        VkMemoryBarrier toCopy{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(I.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
        auto layout = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = src;
            b.dstAccessMask = dst;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(I.cmd, s0, s1, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        const VkImage swap = P.images[imageIndex];
        layout(P.staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = VkExtent3D{uint32_t(w), uint32_t(h), 1};
        vkCmdCopyBufferToImage(I.cmd, I.bufs[5].buffer, P.staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        layout(P.staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        layout(swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // The part in view (all of it unless the viewer's zoomed in).
        const int sx0 = std::clamp(int(std::floor(P.x0 * w)), 0, w - 1), sy0 = std::clamp(int(std::floor(P.y0 * h)), 0, h - 1);
        const int sx1 = std::clamp(int(std::ceil(P.x1 * w)), sx0 + 1, w), sy1 = std::clamp(int(std::ceil(P.y1 * h)), sy0 + 1, h);
        blit.srcOffsets[0] = VkOffset3D{sx0, sy0, 0};
        blit.srcOffsets[1] = VkOffset3D{sx1, sy1, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = VkOffset3D{int32_t(P.extent.width), int32_t(P.extent.height), 1};
        vkCmdBlitImage(I.cmd, P.staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(I.physical, P.surface, &caps);
        if (P.capture && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && I.ensureCapture(size_t(P.extent.width) * P.extent.height * 4)) {
            layout(swap, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy back{};
            back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            back.imageExtent = VkExtent3D{P.extent.width, P.extent.height, 1};
            vkCmdCopyImageToBuffer(I.cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.captured.buffer, 1, &back);
            last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        layout(swap, last, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(I.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    }
    if (I.timestamps) vkCmdWriteTimestamp(I.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, I.timestamps, 1);
    vkEndCommandBuffer(I.cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &I.cmd;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (mode == 3) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &I.present.acquired;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &I.present.rendered;
    }
    vkResetFences(I.device, 1, &I.fence);
    if (vkQueueSubmit(I.queue, 1, &si, I.fence) != VK_SUCCESS) return {};
    // Until the frame is done: its buffers can't be touched before then. (A CPU Vulkan compiling the shader for the
    // first time can take well over 10 s; giving up early and reusing the command buffer crashed lavapipe.) Only a
    // lost device ends the wait.
    for (int waited = 0;; ++waited) {
        const VkResult r = vkWaitForFences(I.device, 1, &I.fence, VK_TRUE, UINT64_C(5000000000));
        if (r == VK_SUCCESS) break;
        if (r != VK_TIMEOUT) {
            qCWarning(lcGpu) << "GPU frame failed:" << int(r);
            return {};
        }
        if (waited == 1) qCWarning(lcGpu) << "GPU frame is taking a long time; still waiting";
    }
    if (I.timestamps) {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(I.device, I.timestamps, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            I.lastMs = double(ts[1] - ts[0]) * I.timestampPeriod / 1e6;
    }
    if (mode == 3) {
        auto& P = I.present;
        if (P.capture && P.captured.mapped) {
            P.lastCapture.assign(static_cast<uint8_t*>(P.captured.mapped), static_cast<uint8_t*>(P.captured.mapped) + size_t(P.extent.width) * P.extent.height * 4);
            P.capturedBgr = P.format == VK_FORMAT_B8G8R8A8_UNORM;
            P.capturedW = int(P.extent.width);
            P.capturedH = int(P.extent.height);
            P.capture = false;
        }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &I.present.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &I.present.swapchain;
        pi.pImageIndices = &imageIndex;
        const VkResult r = vkQueuePresentKHR(I.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) I.present.stale = true;
        // Out of date (the window just changed): this frame is dropped and the next goes to a new swapchain; it isn't
        // composed again to be read back.
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR) return {};
        return {1};
    }
    // The one readback.
    std::vector<uint8_t> out;
    if (mode == 1) {
        out.resize(pixelsOut * 16);
        std::memcpy(out.data(), I.bufs[6].mapped, out.size());
    } else if (mode == 2) {
        out.resize(nv12Bytes);
        std::memcpy(out.data(), I.bufs[5].mapped, nv12Bytes);
    } else if (mode == 4) {
        out.resize(p010Bytes);
        std::memcpy(out.data(), I.bufs[5].mapped, p010Bytes);
    } else {
        out.resize(pixelsOut * 4);
        std::memcpy(out.data(), I.bufs[5].mapped, out.size());
    }
    return out;
}

std::vector<uint8_t> GpuRenderer::render(const FrameJob& job) { return run(job, 0); }

std::vector<uint16_t> GpuRenderer::renderP010(const FrameJob& job) {
    const auto bytes = run(job, 4);
    std::vector<uint16_t> out(bytes.size() / 2);
    if (!bytes.empty()) std::memcpy(out.data(), bytes.data(), out.size() * 2);
    return out;
}

void* GpuRenderer::vkInstance() const { return d->surfaceExt && d->swapchainExt ? reinterpret_cast<void*>(d->instance) : nullptr; }

bool GpuRenderer::setSurface(void* surface, int width, int height) {
    std::lock_guard g(d->lock);
    Impl& I = *d;
    if (!surface) { I.dropPresent(); return true; }
    VkSurfaceKHR s = reinterpret_cast<VkSurfaceKHR>(surface);
    if (s != I.present.surface) {
        I.dropPresent();
        VkBool32 ok = VK_FALSE;
        if (!I.swapchainExt || vkGetPhysicalDeviceSurfaceSupportKHR(I.physical, I.queueFamily, s, &ok) != VK_SUCCESS || !ok) return false;
        I.present.surface = s;
    }
    if (width != I.present.wantW || height != I.present.wantH) I.present.stale = true;
    I.present.wantW = width;
    I.present.wantH = height;
    return true;
}

bool GpuRenderer::present(const FrameJob& job) { return !run(job, 3).empty(); }

void GpuRenderer::setPresentRegion(double x0, double y0, double x1, double y1) {
    std::lock_guard g(d->lock);
    d->present.x0 = std::clamp(x0, 0.0, 1.0);
    d->present.y0 = std::clamp(y0, 0.0, 1.0);
    d->present.x1 = std::clamp(x1, d->present.x0, 1.0);
    d->present.y1 = std::clamp(y1, d->present.y0, 1.0);
}

long long GpuRenderer::dmabufImports() const { std::lock_guard g(d->lock); return d->importedCount; }

void GpuRenderer::capturePresented() { std::lock_guard g(d->lock); d->present.capture = true; d->present.lastCapture.clear(); }

QImage GpuRenderer::presentedImage() const {
    std::lock_guard g(d->lock);
    const auto& P = d->present;
    if (P.lastCapture.empty()) return {};
    QImage img(P.lastCapture.data(), P.capturedW, P.capturedH, P.capturedW * 4, P.capturedBgr ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    return img.convertToFormat(QImage::Format_RGBX8888);
}
std::vector<uint8_t> GpuRenderer::renderNV12(const FrameJob& job) { return run(job, 2); }
std::vector<float> GpuRenderer::renderFloat(const FrameJob& job) {
    const auto bytes = run(job, 1);
    std::vector<float> f(bytes.size() / 4);
    if (!bytes.empty()) std::memcpy(f.data(), bytes.data(), bytes.size());
    return f;
}

} // namespace er
