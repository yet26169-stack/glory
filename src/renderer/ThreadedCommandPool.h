#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <cstdint>

namespace glory {

// Each worker thread needs its own VkCommandPool (Vulkan spec requirement).
// This struct holds a pool + secondary command buffers for each frame-in-flight.
static constexpr uint32_t MAX_FRAMES = 2;

struct ThreadCommandResources {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, MAX_FRAMES> secondaryBuffers{};

    void init(VkDevice device, uint32_t graphicsQueueFamily) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = graphicsQueueFamily;
        vkCreateCommandPool(device, &ci, nullptr, &pool);

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocInfo.commandBufferCount = MAX_FRAMES;
        vkAllocateCommandBuffers(device, &allocInfo, secondaryBuffers.data());
    }

    void destroy(VkDevice device) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }

    void reset(VkDevice device, uint32_t frameIndex) {
        vkResetCommandBuffer(secondaryBuffers[frameIndex], 0);
    }

    VkCommandBuffer begin(uint32_t frameIndex, VkRenderPass renderPass, VkFramebuffer framebuffer) {
        VkCommandBuffer cmd = secondaryBuffers[frameIndex];

        VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        inherit.renderPass = renderPass;
        inherit.framebuffer = framebuffer;

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                        | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        beginInfo.pInheritanceInfo = &inherit;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    }

    void end(uint32_t frameIndex) {
        vkEndCommandBuffer(secondaryBuffers[frameIndex]);
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

    ThreadCommandResources& getResources(uint32_t threadIndex) {
        return m_resources[threadIndex];
    }

    uint32_t threadCount() const { return static_cast<uint32_t>(m_resources.size()); }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<ThreadCommandResources> m_resources;
};

} // namespace glory
