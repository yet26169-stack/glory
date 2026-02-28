#include "renderer/Material.h"
#include "renderer/Device.h"
#include "renderer/Texture.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

namespace glory {

Material::Material(const Device& device, VkDescriptorSetLayout layout,
                   VkDescriptorPool pool, uint32_t frameCount)
    : m_vkDevice(device.getDevice())
{
    std::vector<VkDescriptorSetLayout> layouts(frameCount, layout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = pool;
    allocInfo.descriptorSetCount = frameCount;
    allocInfo.pSetLayouts        = layouts.data();

    m_descriptorSets.resize(frameCount);
    VK_CHECK(vkAllocateDescriptorSets(m_vkDevice, &allocInfo, m_descriptorSets.data()),
             "Failed to allocate material descriptor sets");
}

Material::~Material() = default;

Material::Material(Material&& other) noexcept
    : m_props(other.m_props)
    , m_descriptorSets(std::move(other.m_descriptorSets))
    , m_vkDevice(other.m_vkDevice)
{
    other.m_vkDevice = VK_NULL_HANDLE;
}

Material& Material::operator=(Material&& other) noexcept {
    if (this != &other) {
        m_props          = other.m_props;
        m_descriptorSets = std::move(other.m_descriptorSets);
        m_vkDevice       = other.m_vkDevice;
        other.m_vkDevice = VK_NULL_HANDLE;
    }
    return *this;
}

void Material::setAlbedoTexture(const Texture& tex) {
    for (auto& set : m_descriptorSets) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView   = tex.getImageView();
        imgInfo.sampler     = tex.getSampler();

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 1;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;

        vkUpdateDescriptorSets(m_vkDevice, 1, &write, 0, nullptr);
    }
}

void Material::bind(VkCommandBuffer cmd, VkPipelineLayout pipelineLayout,
                    uint32_t frameIndex) const
{
    VkDescriptorSet set = m_descriptorSets[frameIndex];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &set, 0, nullptr);
}

} // namespace glory
