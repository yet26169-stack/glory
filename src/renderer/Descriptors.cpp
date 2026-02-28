#include "renderer/Descriptors.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>

namespace glory {

Descriptors::Descriptors(const Device& device, uint32_t frameCount)
    : m_device(device), m_frameCount(frameCount)
{
    createLayout();
    createUniformBuffers(frameCount);
    createPool(frameCount);
    createSets(frameCount);
}

Descriptors::~Descriptors() { cleanup(); }

void Descriptors::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();
    VmaAllocator alloc = m_device.getAllocator();

    for (size_t i = 0; i < m_uniformBuffers.size(); ++i)
        vmaDestroyBuffer(alloc, m_uniformBuffers[i], m_uniformAllocations[i]);
    for (size_t i = 0; i < m_lightBuffers.size(); ++i)
        vmaDestroyBuffer(alloc, m_lightBuffers[i], m_lightAllocations[i]);

    if (m_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_layout, nullptr);

    spdlog::info("Descriptors destroyed");
}

void Descriptors::updateUniformBuffer(uint32_t frameIndex, const UniformBufferObject& ubo) {
    std::memcpy(m_uniformMapped[frameIndex], &ubo, sizeof(ubo));
}

void Descriptors::updateLightBuffer(uint32_t frameIndex, const LightUBO& light) {
    std::memcpy(m_lightMapped[frameIndex], &light, sizeof(light));
}

void Descriptors::writeBindlessTexture(uint32_t arrayIndex, VkImageView imageView, VkSampler sampler) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = imageView;
    imgInfo.sampler     = sampler;

    for (uint32_t i = 0; i < m_frameCount; ++i) {
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_sets[i];
        write.dstBinding      = 1; // bindless texture array
        write.dstArrayElement = arrayIndex;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;

        vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
    }
}

void Descriptors::updateShadowMap(VkImageView depthView, VkSampler shadowSampler) {
    for (uint32_t i = 0; i < m_frameCount; ++i) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imgInfo.imageView   = depthView;
        imgInfo.sampler     = shadowSampler;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_sets[i];
        write.dstBinding      = 3;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;

        vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
    }
}

// ── Private ─────────────────────────────────────────────────────────────────
void Descriptors::createLayout() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

    // binding 0: UBO (vertex + fragment)
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 1: bindless texture array (diffuse + normal maps)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = MAX_BINDLESS_TEXTURES;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 2: light UBO
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 3: shadow map
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding flags: UPDATE_AFTER_BIND + PARTIALLY_BOUND for bindless array
    std::array<VkDescriptorBindingFlags, 4> bindingFlags{};
    bindingFlags[0] = 0;
    bindingFlags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                    | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    bindingFlags[2] = 0;
    bindingFlags[3] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCI{};
    flagsCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsCI.bindingCount  = static_cast<uint32_t>(bindingFlags.size());
    flagsCI.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.pNext        = &flagsCI;
    ci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &ci, nullptr, &m_layout),
             "Failed to create descriptor set layout");
    spdlog::info("Descriptor set layout created (bindless: {} textures)", MAX_BINDLESS_TEXTURES);
}

void Descriptors::createUniformBuffers(uint32_t frameCount) {
    VmaAllocator alloc = m_device.getAllocator();

    auto makeMappedBuffers = [&](VkDeviceSize size, uint32_t count,
                                  std::vector<VkBuffer>& bufs,
                                  std::vector<VmaAllocation>& allocs,
                                  std::vector<void*>& mapped)
    {
        bufs.resize(count);
        allocs.resize(count);
        mapped.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkBufferCreateInfo bufCI{};
            bufCI.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufCI.size        = size;
            bufCI.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocCI{};
            allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo allocInfo{};
            VK_CHECK(vmaCreateBuffer(alloc, &bufCI, &allocCI,
                                     &bufs[i], &allocs[i], &allocInfo),
                     "Failed to create uniform buffer");
            mapped[i] = allocInfo.pMappedData;
        }
    };

    makeMappedBuffers(sizeof(UniformBufferObject), frameCount,
                      m_uniformBuffers, m_uniformAllocations, m_uniformMapped);
    makeMappedBuffers(sizeof(LightUBO), frameCount,
                      m_lightBuffers, m_lightAllocations, m_lightMapped);

    spdlog::info("{} uniform + light buffers created (persistently mapped)", frameCount);
}

void Descriptors::createPool(uint32_t frameCount) {
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = frameCount * 2; // UBO + light UBO per frame
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = frameCount * (MAX_BINDLESS_TEXTURES + 1); // bindless array + shadow map

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    ci.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    ci.pPoolSizes    = poolSizes.data();
    ci.maxSets       = frameCount;

    VK_CHECK(vkCreateDescriptorPool(m_device.getDevice(), &ci, nullptr, &m_pool),
             "Failed to create descriptor pool");
}

void Descriptors::createSets(uint32_t frameCount) {
    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_layout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = frameCount;
    allocInfo.pSetLayouts        = layouts.data();

    m_sets.resize(frameCount);
    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, m_sets.data()),
             "Failed to allocate descriptor sets");

    for (uint32_t i = 0; i < frameCount; ++i) {
        VkDescriptorBufferInfo uboBufInfo{};
        uboBufInfo.buffer = m_uniformBuffers[i];
        uboBufInfo.offset = 0;
        uboBufInfo.range  = sizeof(UniformBufferObject);

        VkDescriptorBufferInfo lightBufInfo{};
        lightBufInfo.buffer = m_lightBuffers[i];
        lightBufInfo.offset = 0;
        lightBufInfo.range  = sizeof(LightUBO);

        std::array<VkWriteDescriptorSet, 2> writes{};

        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_sets[i];
        writes[0].dstBinding      = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &uboBufInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_sets[i];
        writes[1].dstBinding      = 2;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &lightBufInfo;

        vkUpdateDescriptorSets(m_device.getDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    spdlog::info("{} descriptor sets allocated and written", frameCount);
}

} // namespace glory
