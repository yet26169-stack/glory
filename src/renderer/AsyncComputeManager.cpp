#include "AsyncComputeManager.h"
#include "Device.h"
#include <spdlog/spdlog.h>
#include <stdexcept>

#ifndef VK_CHECK
#define VK_CHECK(x, msg) do { if ((x) != VK_SUCCESS) throw std::runtime_error(msg); } while(0)
#endif

namespace glory {

void AsyncComputeManager::init(const Device& device) {
    m_device           = &device;
    m_computeQueue     = device.getComputeQueue();
    m_queueFamilyIndex = device.getQueueFamilies().computeFamily.value();

    // Create command pool for the compute queue family
    VkCommandPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                              VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = m_queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(device.getDevice(), &poolCI, nullptr, &m_commandPool),
             "Failed to create async compute command pool");

    // Allocate per-frame command buffers
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool        = m_commandPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(device.getDevice(), &allocInfo, m_cmdBuffers.data()),
             "Failed to allocate async compute command buffers");

    // Create timeline semaphore
    VkSemaphoreTypeCreateInfo timelineCI{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCI.initialValue  = 0;

    VkSemaphoreCreateInfo semCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semCI.pNext = &timelineCI;
    VK_CHECK(vkCreateSemaphore(device.getDevice(), &semCI, nullptr, &m_timeline),
             "Failed to create async compute timeline semaphore");

    spdlog::info("AsyncComputeManager initialized (queue family {})", m_queueFamilyIndex);
}

void AsyncComputeManager::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(dev, m_timeline, nullptr);
        m_timeline = VK_NULL_HANDLE;
    }
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    m_device = nullptr;
}

VkCommandBuffer AsyncComputeManager::begin(uint32_t frameIndex) {
    VkCommandBuffer cmd = m_cmdBuffers[frameIndex];

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "Failed to begin async compute CB");

    return cmd;
}

uint64_t AsyncComputeManager::submit(uint32_t frameIndex) {
    VkCommandBuffer cmd = m_cmdBuffers[frameIndex];
    VK_CHECK(vkEndCommandBuffer(cmd), "Failed to end async compute CB");

    uint64_t signalValue = ++m_timelineCounter;
    m_frameValues[frameIndex] = signalValue;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = m_timeline;
    signalInfo.value     = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount   = 1;
    submitInfo.pCommandBufferInfos      = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos    = &signalInfo;

    VK_CHECK(m_device->submitCompute(1, &submitInfo),
             "Async compute submit failed");

    return signalValue;
}

void AsyncComputeManager::waitForCompute(uint32_t frameIndex) {
    uint64_t waitValue = m_frameValues[frameIndex];
    if (waitValue == 0) return;

    VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores    = &m_timeline;
    waitInfo.pValues        = &waitValue;
    vkWaitSemaphores(m_device->getDevice(), &waitInfo, UINT64_MAX);
}

} // namespace glory
