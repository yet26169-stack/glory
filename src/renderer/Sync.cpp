#include "renderer/Sync.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

namespace glory {

Sync::Sync(const Device& device, uint32_t swapchainImageCount) : m_device(device) {
    createCommandPool();
    createCommandBuffers();
    createSyncObjects(swapchainImageCount);
}

Sync::~Sync() { cleanup(); }

void Sync::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();

    for (auto& fence : m_inFlightFences)
        if (fence != VK_NULL_HANDLE) vkDestroyFence(dev, fence, nullptr);
    for (auto& sem : m_renderFinished)
        if (sem != VK_NULL_HANDLE) vkDestroySemaphore(dev, sem, nullptr);
    for (auto& sem : m_imageAvailable)
        if (sem != VK_NULL_HANDLE) vkDestroySemaphore(dev, sem, nullptr);

    if (m_commandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(dev, m_commandPool, nullptr);

    spdlog::info("Sync objects destroyed");
}

void Sync::createCommandPool() {
    auto indices = m_device.getQueueFamilies();

    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = indices.graphicsFamily.value();

    VK_CHECK(vkCreateCommandPool(m_device.getDevice(), &ci, nullptr, &m_commandPool),
             "Failed to create command pool");
    spdlog::info("Command pool created");
}

void Sync::createCommandBuffers() {
    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = m_commandPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    VK_CHECK(vkAllocateCommandBuffers(m_device.getDevice(), &ai, m_commandBuffers.data()),
             "Failed to allocate command buffers");
    spdlog::info("{} command buffers allocated", MAX_FRAMES_IN_FLIGHT);
}

void Sync::createSyncObjects(uint32_t swapchainImageCount) {
    m_imageAvailable.resize(MAX_FRAMES_IN_FLIGHT);
    m_renderFinished.resize(swapchainImageCount);
    m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice dev = m_device.getDevice();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(dev, &semCI, nullptr, &m_imageAvailable[i]),
                 "Failed to create image-available semaphore");
        VK_CHECK(vkCreateFence(dev, &fenceCI, nullptr, &m_inFlightFences[i]),
                 "Failed to create in-flight fence");
    }
    for (uint32_t i = 0; i < swapchainImageCount; ++i) {
        VK_CHECK(vkCreateSemaphore(dev, &semCI, nullptr, &m_renderFinished[i]),
                 "Failed to create render-finished semaphore");
    }
    spdlog::info("Sync objects created ({} frames in flight, {} render semaphores)",
                 MAX_FRAMES_IN_FLIGHT, swapchainImageCount);
}

void Sync::recreateRenderFinishedSemaphores(uint32_t swapchainImageCount) {
    VkDevice dev = m_device.getDevice();
    for (auto& sem : m_renderFinished)
        if (sem != VK_NULL_HANDLE) vkDestroySemaphore(dev, sem, nullptr);

    m_renderFinished.resize(swapchainImageCount);
    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < swapchainImageCount; ++i) {
        VK_CHECK(vkCreateSemaphore(dev, &semCI, nullptr, &m_renderFinished[i]),
                 "Failed to create render-finished semaphore");
    }
}

} // namespace glory
