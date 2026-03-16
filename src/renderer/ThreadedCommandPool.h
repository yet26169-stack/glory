#pragma once
#include "renderer/RenderFormats.h"
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <cstdint>
#include <cassert>

namespace glory {

static constexpr uint32_t MAX_FRAMES = 2;
static constexpr uint32_t CBS_PER_FRAME = 8; // up to 8 secondary CBs per thread per frame

struct ThreadCommandResources {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<std::array<VkCommandBuffer, CBS_PER_FRAME>, MAX_FRAMES> secondaryBuffers{};
    std::array<uint32_t, MAX_FRAMES> nextCB{};

    void init(VkDevice device, uint32_t graphicsQueueFamily) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = graphicsQueueFamily;
        vkCreateCommandPool(device, &ci, nullptr, &pool);

        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            allocInfo.commandBufferCount = CBS_PER_FRAME;
            vkAllocateCommandBuffers(device, &allocInfo, secondaryBuffers[f].data());
        }
    }

    void destroy(VkDevice device) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }

    void resetFrame(uint32_t frameIndex) {
        for (uint32_t i = 0; i < nextCB[frameIndex]; ++i) {
            vkResetCommandBuffer(secondaryBuffers[frameIndex][i], 0);
        }
        nextCB[frameIndex] = 0;
    }

    // Begin the next available secondary command buffer for this frame using
    // VkCommandBufferInheritanceRenderingInfo (Vulkan 1.3 dynamic rendering).
    VkCommandBuffer begin(uint32_t frameIndex, const RenderFormats& formats) {
        uint32_t idx = nextCB[frameIndex]++;
        assert(idx < CBS_PER_FRAME);
        VkCommandBuffer cmd = secondaryBuffers[frameIndex][idx];

        VkCommandBufferInheritanceRenderingInfo inheritRendering{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
        inheritRendering.colorAttachmentCount    = formats.colorCount;
        inheritRendering.pColorAttachmentFormats = formats.colorFormats;
        inheritRendering.depthAttachmentFormat   = formats.depthFormat;
        inheritRendering.stencilAttachmentFormat = formats.stencilFormat;
        inheritRendering.rasterizationSamples    = VK_SAMPLE_COUNT_1_BIT;

        VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        inherit.pNext = &inheritRendering;

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                        | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        beginInfo.pInheritanceInfo = &inherit;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    }

    static void end(VkCommandBuffer cmd) {
        vkEndCommandBuffer(cmd);
    }
};

// Manages per-thread command pools for all worker threads
class ThreadedCommandPoolManager {
public:
    void init(VkDevice device, uint32_t graphicsQueueFamily, uint32_t threadCount) {
        m_device = device;
        m_resources.resize(threadCount);
        for (auto& res : m_resources) {
            res.init(device, graphicsQueueFamily);
        }
    }

    void destroy() {
        for (auto& res : m_resources) {
            res.destroy(m_device);
        }
        m_resources.clear();
    }

    void resetFrame(uint32_t frameIndex) {
        for (auto& res : m_resources) {
            res.resetFrame(frameIndex);
        }
    }

    ThreadCommandResources& getResources(uint32_t threadIndex) {
        return m_resources[threadIndex];
    }

    uint32_t threadCount() const { return static_cast<uint32_t>(m_resources.size()); }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<ThreadCommandResources> m_resources;
};

} // namespace glory
