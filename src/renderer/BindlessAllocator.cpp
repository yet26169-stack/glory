#include "renderer/BindlessAllocator.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>

namespace glory {

void BindlessAllocator::init(VkDevice device, uint32_t /*framesInFlight*/) {
    m_device = device;

    // Descriptor pool with UPDATE_AFTER_BIND
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = MAX_TEXTURES;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_BUFFERS;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(device, &poolCI, nullptr, &m_pool), "Create bindless descriptor pool");

    // Layout with PARTIALLY_BOUND + UPDATE_AFTER_BIND flags
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = MAX_TEXTURES;
    bindings[0].stageFlags      = VK_SHADER_STAGE_ALL;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = MAX_BUFFERS;
    bindings[1].stageFlags      = VK_SHADER_STAGE_ALL;

    VkDescriptorBindingFlags bindFlags[2]{
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{};
    flagsCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsCI.bindingCount  = 2;
    flagsCI.pBindingFlags = bindFlags;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.pNext        = &flagsCI;
    layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_layout), "Create bindless descriptor set layout");

    // Allocate the single global descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_layout;
    VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &m_set), "Allocate bindless descriptor set");

    // Initialise free-slot lists
    m_freeTexSlots.resize(MAX_TEXTURES);
    for (uint32_t i = 0; i < MAX_TEXTURES; ++i) m_freeTexSlots[i] = MAX_TEXTURES - 1 - i;
    m_freeBufSlots.resize(MAX_BUFFERS);
    for (uint32_t i = 0; i < MAX_BUFFERS; ++i) m_freeBufSlots[i] = MAX_BUFFERS - 1 - i;

    spdlog::info("BindlessAllocator: initialised ({} textures, {} buffers)",
                 MAX_TEXTURES, MAX_BUFFERS);
}

uint32_t BindlessAllocator::allocTexture(VkImageView view, VkSampler sampler) {
    if (m_freeTexSlots.empty()) {
        spdlog::error("BindlessAllocator: out of texture slots");
        return UINT32_MAX;
    }
    uint32_t idx = m_freeTexSlots.back();
    m_freeTexSlots.pop_back();

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = view;
    imgInfo.sampler     = sampler;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_set;
    write.dstBinding      = 0;
    write.dstArrayElement = idx;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return idx;
}

uint32_t BindlessAllocator::allocBuffer(VkBuffer buf, VkDeviceSize offset,
                                         VkDeviceSize range) {
    if (m_freeBufSlots.empty()) {
        spdlog::error("BindlessAllocator: out of buffer slots");
        return UINT32_MAX;
    }
    uint32_t idx = m_freeBufSlots.back();
    m_freeBufSlots.pop_back();

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = buf;
    bufInfo.offset = offset;
    bufInfo.range  = range;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_set;
    write.dstBinding      = 1;
    write.dstArrayElement = idx;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &bufInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    return idx;
}

void BindlessAllocator::freeTexture(uint32_t idx) {
    m_freeTexSlots.push_back(idx);
}

void BindlessAllocator::freeBuffer(uint32_t idx) {
    m_freeBufSlots.push_back(idx);
}

void BindlessAllocator::destroy() {
    if (m_pool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    m_pool   = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_set    = VK_NULL_HANDLE;
}

} // namespace glory
