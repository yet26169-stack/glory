#include "renderer/QueueManager.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>

namespace glory {

void QueueManager::init(VkDevice device,
                         uint32_t graphicsFamily, uint32_t computeFamily,
                         uint32_t transferFamily, uint32_t /*framesInFlight*/) {
    m_device = device;
    vkGetDeviceQueue(device, graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(device, computeFamily,  0, &m_computeQueue);
    vkGetDeviceQueue(device, transferFamily, 0, &m_transferQueue);

    // Create timeline semaphore for async compute → graphics synchronisation
    VkSemaphoreTypeCreateInfo typeCI{};
    typeCI.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeCI.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeCI.initialValue  = 0;

    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semCI.pNext = &typeCI;
    VK_CHECK(vkCreateSemaphore(device, &semCI, nullptr, &m_computeTimeline), "Create compute timeline semaphore");

    spdlog::info("QueueManager: initialised (graphics={}, compute={}, transfer={})",
                 graphicsFamily, computeFamily, transferFamily);
}

void QueueManager::submitCompute(VkCommandBuffer cmd, uint64_t signalValue) {
    VkTimelineSemaphoreSubmitInfo tsInfo{};
    tsInfo.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsInfo.signalSemaphoreValueCount = 1;
    tsInfo.pSignalSemaphoreValues    = &signalValue;

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext                = &tsInfo;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_computeTimeline;
    VK_CHECK(vkQueueSubmit(m_computeQueue, 1, &si, VK_NULL_HANDLE), "Compute queue submit");
}

void QueueManager::submitGraphics(VkCommandBuffer cmd, VkSemaphore waitSemaphore,
                                   uint64_t waitValue, VkFence signalFence) {
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;

    VkTimelineSemaphoreSubmitInfo tsInfo{};
    tsInfo.sType                    = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsInfo.waitSemaphoreValueCount  = 1;
    tsInfo.pWaitSemaphoreValues     = &waitValue;

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext                = &tsInfo;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &waitSemaphore;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    VK_CHECK(vkQueueSubmit(m_graphicsQueue, 1, &si, signalFence), "Graphics queue submit");
}

void QueueManager::destroy() {
    if (m_computeTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_device, m_computeTimeline, nullptr);
        m_computeTimeline = VK_NULL_HANDLE;
    }
}

} // namespace glory
