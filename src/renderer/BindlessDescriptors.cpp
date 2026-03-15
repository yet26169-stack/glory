#include "renderer/BindlessDescriptors.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <array>
#include <stdexcept>

namespace glory {

BindlessDescriptors::BindlessDescriptors(const Device& device) : m_device(device) {
    createLayout();
    createPool();
    createSet();
}

BindlessDescriptors::~BindlessDescriptors() { cleanup(); }

void BindlessDescriptors::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();
    if (m_pool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_layout, nullptr);
    spdlog::info("BindlessDescriptors destroyed");
}

uint32_t BindlessDescriptors::registerTexture(VkImageView view, VkSampler sampler) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_nextTextureSlot >= MAX_TEXTURES)
        throw std::runtime_error("BindlessDescriptors: texture slots exhausted");

    uint32_t slot = m_nextTextureSlot++;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = view;
    imgInfo.sampler     = sampler;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = m_set;
    write.dstBinding      = 0;
    write.dstArrayElement = slot;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
    return slot;
}

uint32_t BindlessDescriptors::registerStorageBuffer(VkBuffer buffer,
                                                     VkDeviceSize offset,
                                                     VkDeviceSize range) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_nextStorageSlot >= MAX_STORAGE_BUFFERS)
        throw std::runtime_error("BindlessDescriptors: storage buffer slots exhausted");

    uint32_t slot = m_nextStorageSlot++;

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = buffer;
    bufInfo.offset = offset;
    bufInfo.range  = range;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = m_set;
    write.dstBinding      = 1;
    write.dstArrayElement = slot;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
    return slot;
}

// ── Private ─────────────────────────────────────────────────────────────────

void BindlessDescriptors::createLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    // binding 0: sampled image array [MAX_TEXTURES]
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = MAX_TEXTURES;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 1: storage buffer array [MAX_STORAGE_BUFFERS]
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = MAX_STORAGE_BUFFERS;
    bindings[1].stageFlags      = VK_SHADER_STAGE_ALL;

    std::array<VkDescriptorBindingFlags, 2> bindingFlags{};
    bindingFlags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                    | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bindingFlags[1] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                    | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsCI.bindingCount  = static_cast<uint32_t>(bindingFlags.size());
    flagsCI.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.pNext        = &flagsCI;
    ci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &ci, nullptr, &m_layout),
             "BindlessDescriptors: Failed to create layout");
    spdlog::info("BindlessDescriptors layout created (textures={}, SSBOs={})",
                 MAX_TEXTURES, MAX_STORAGE_BUFFERS);
}

void BindlessDescriptors::createPool() {
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = MAX_TEXTURES;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_STORAGE_BUFFERS;

    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    ci.maxSets       = 1;
    ci.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    ci.pPoolSizes    = poolSizes.data();

    VK_CHECK(vkCreateDescriptorPool(m_device.getDevice(), &ci, nullptr, &m_pool),
             "BindlessDescriptors: Failed to create pool");
}

void BindlessDescriptors::createSet() {
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_layout;

    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &m_set),
             "BindlessDescriptors: Failed to allocate set");
    spdlog::info("BindlessDescriptors set allocated");
}

} // namespace glory
