#include "renderer/Descriptors.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

    m_uniformBuffers.clear();
    m_lightBuffers.clear();
    m_boneBuffers.clear();

    if (m_pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_layout, nullptr);

    spdlog::info("Descriptors destroyed");
}

void Descriptors::updateUniformBuffer(uint32_t frameIndex, const UniformBufferObject& ubo) {
    std::memcpy(m_uniformBuffers[frameIndex].map(), &ubo, sizeof(ubo));
}

void Descriptors::updateLightBuffer(uint32_t frameIndex, const LightUBO& light) {
    std::memcpy(m_lightBuffers[frameIndex].map(), &light, sizeof(light));
}

void Descriptors::updateBoneBuffer(uint32_t frameIndex,
                                   const std::vector<glm::mat4>& matrices) {
    // Convenience: write to slot 0
    writeBoneSlot(frameIndex, 0, matrices);
}

uint32_t Descriptors::writeBoneSlot(uint32_t frameIndex, uint32_t slotIndex,
                                    const std::vector<glm::mat4>& matrices) {
    uint32_t slot = std::min(slotIndex, MAX_SKINNED_CHARS - 1);
    uint32_t count = std::min(static_cast<uint32_t>(matrices.size()), MAX_BONES);
    size_t offsetBytes = static_cast<size_t>(slot) * MAX_BONES * sizeof(glm::mat4);
    std::memcpy(static_cast<char*>(m_boneBuffers[frameIndex].map()) + offsetBytes,
                matrices.data(), sizeof(glm::mat4) * count);
    // Return the index of the first bone for this slot (used as push constant
    // boneBaseIndex in the vertex shader)
    return slot * MAX_BONES;
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
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};

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

    // binding 4: bone matrix SSBO (vertex stage — GPU skinning)
    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    // Binding flags
    std::array<VkDescriptorBindingFlags, 5> bindingFlags{};
    bindingFlags[0] = 0;
    bindingFlags[1] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                    | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    bindingFlags[2] = 0;
    bindingFlags[3] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bindingFlags[4] = 0;

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
    spdlog::info("Descriptor set layout created (bindless: {} textures, bone SSBO binding 4)",
                 MAX_BINDLESS_TEXTURES);
}

void Descriptors::createUniformBuffers(uint32_t frameCount) {
    VmaAllocator alloc = m_device.getAllocator();

    m_uniformBuffers.reserve(frameCount);
    m_lightBuffers.reserve(frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_uniformBuffers.emplace_back(alloc, sizeof(UniformBufferObject),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_lightBuffers.emplace_back(alloc, sizeof(LightUBO),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_CPU_TO_GPU);
    }

    // Bone SSBO: MAX_SKINNED_CHARS * MAX_BONES mat4s, CPU_TO_GPU, persistently mapped via Buffer
    VkDeviceSize boneSize = sizeof(glm::mat4) * MAX_BONES * MAX_SKINNED_CHARS;
    m_boneBuffers.reserve(frameCount);
    for (uint32_t i = 0; i < frameCount; ++i) {
        m_boneBuffers.emplace_back(alloc, boneSize,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VMA_MEMORY_USAGE_CPU_TO_GPU);
        
        // Initialize all slots to identity matrices
        glm::mat4 identity(1.0f);
        void* mapped = m_boneBuffers[i].map();
        for (uint32_t b = 0; b < MAX_BONES * MAX_SKINNED_CHARS; ++b) {
            std::memcpy(static_cast<char*>(mapped) + b * sizeof(glm::mat4),
                        &identity, sizeof(glm::mat4));
        }
    }

    spdlog::info("{} uniform + light + bone buffers created (persistently mapped)", frameCount);
}

void Descriptors::createPool(uint32_t frameCount) {
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = frameCount * 2; // UBO + light UBO per frame
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = frameCount * (MAX_BINDLESS_TEXTURES + 1); // bindless array + shadow map
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = frameCount; // bone SSBO per frame

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
        uboBufInfo.buffer = m_uniformBuffers[i].getBuffer();
        uboBufInfo.offset = 0;
        uboBufInfo.range  = sizeof(UniformBufferObject);

        VkDescriptorBufferInfo lightBufInfo{};
        lightBufInfo.buffer = m_lightBuffers[i].getBuffer();
        lightBufInfo.offset = 0;
        lightBufInfo.range  = sizeof(LightUBO);

        VkDescriptorBufferInfo boneBufInfo{};
        boneBufInfo.buffer = m_boneBuffers[i].getBuffer();
        boneBufInfo.offset = 0;
        boneBufInfo.range  = sizeof(glm::mat4) * MAX_BONES * MAX_SKINNED_CHARS;

        std::array<VkWriteDescriptorSet, 3> writes{};

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

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_sets[i];
        writes[2].dstBinding      = 4;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo     = &boneBufInfo;

        vkUpdateDescriptorSets(m_device.getDevice(),
                               static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    spdlog::info("{} descriptor sets allocated and written", frameCount);
}

} // namespace glory
