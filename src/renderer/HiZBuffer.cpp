#include "renderer/HiZBuffer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <string>
#include <fstream>
#include <vector>

namespace glory {

void HiZBuffer::init(Device& device, uint32_t width, uint32_t height) {
    m_device    = device.getDevice();
    m_allocator = device.getAllocator();

    // Compute mip count from max dimension
    m_mipCount = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    // Create the Hi-Z image (R32F, all mip levels) via raw VMA
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = VK_FORMAT_R32_SFLOAT;
    imgCI.extent        = {width, height, 1};
    imgCI.mipLevels     = m_mipCount;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VK_CHECK(vmaCreateImage(m_allocator, &imgCI, &allocCI, &m_hizImage, &m_hizAlloc, nullptr),
             "Create Hi-Z image");

    // Create one VkImageView per mip level
    m_mipViews.resize(m_mipCount, VK_NULL_HANDLE);
    for (uint32_t mip = 0; mip < m_mipCount; ++mip) {
        VkImageViewCreateInfo viewCI{};
        viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCI.image    = m_hizImage;
        viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format   = VK_FORMAT_R32_SFLOAT;
        viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
        VK_CHECK(vkCreateImageView(m_device, &viewCI, nullptr, &m_mipViews[mip]),
                 "Create Hi-Z mip view");
    }

    // TODO (Phase 4.2): create descriptor sets and pipeline for hiz_generate.comp
    spdlog::info("HiZBuffer: initialised {} mip levels ({}x{})", m_mipCount, width, height);
}

void HiZBuffer::generate(VkCommandBuffer /*cmd*/, VkImageView /*depthView*/,
                          uint32_t /*frameIdx*/) {
    // TODO (Phase 4.2): dispatch hiz_generate.comp for each mip level
}

VkImageView HiZBuffer::getMipView(uint32_t mip) const {
    if (mip >= m_mipViews.size()) return VK_NULL_HANDLE;
    return m_mipViews[mip];
}

void HiZBuffer::destroy() {
    for (auto& v : m_mipViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(m_device, v, nullptr);
    }
    m_mipViews.clear();
    if (m_genPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_genPipeline, nullptr);
    if (m_genLayout   != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_genLayout, nullptr);
    if (m_descPool    != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
    if (m_descLayout  != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_descLayout, nullptr);
    if (m_hizImage    != VK_NULL_HANDLE) vmaDestroyImage(m_allocator, m_hizImage, m_hizAlloc);
    m_hizImage = VK_NULL_HANDLE;
    m_hizAlloc = VK_NULL_HANDLE;
    m_mipCount = 0;
}

} // namespace glory
