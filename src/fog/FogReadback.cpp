#include "fog/FogReadback.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>

namespace glory {

void FogReadback::init(VkDevice device, VkPhysicalDevice /*physDevice*/,
                        uint32_t mapW, uint32_t mapH, uint32_t framesInFlight) {
    m_device = device;
    m_w = mapW;
    m_h = mapH;

    // Create VMA allocator just for staging buffers (or reuse from renderer)
    // NOTE: In production wire up to the Renderer's existing allocator instance.
    VmaAllocatorCreateInfo vmaCI{};
    vmaCI.device         = device;
    vmaCI.physicalDevice = VK_NULL_HANDLE; // will be set by caller in production
    // vmaCreateAllocator(&vmaCI, &m_vma); // skipped — reuse renderer VMA

    m_frames.resize(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                   VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(device, &fenceCI, nullptr, &m_frames[i].fence), "Create readback fence");
    }
    spdlog::info("FogReadback: initialised ({}x{}, {} frames)", mapW, mapH, framesInFlight);
}

void FogReadback::requestReadback(VkCommandBuffer cmd, VkImage fogImage,
                                   uint32_t frameIdx) {
    if (frameIdx >= m_frames.size()) return;
    auto& frame = m_frames[frameIdx];

    // In production: check fence, reset, issue vkCmdCopyImageToBuffer,
    // then set frame.pending = true. Placeholder for now.
    (void)cmd; (void)fogImage;
    frame.pending = false; // not yet implemented
}

const uint8_t* FogReadback::getReadback(uint32_t frameIdx) const {
    if (frameIdx >= m_frames.size()) return nullptr;
    const auto& frame = m_frames[frameIdx];
    if (!frame.pending || frame.mapped == nullptr) return nullptr;
    // Check if fence is signalled (readback complete)
    if (vkGetFenceStatus(m_device, frame.fence) != VK_SUCCESS) return nullptr;
    return static_cast<const uint8_t*>(frame.mapped);
}

void FogReadback::destroy() {
    for (auto& f : m_frames) {
        if (f.fence != VK_NULL_HANDLE) vkDestroyFence(m_device, f.fence, nullptr);
        // NOTE: destroy staging buffer via VMA when wired up
    }
    m_frames.clear();
}

} // namespace glory
