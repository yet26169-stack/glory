#include "renderer/SSRPass.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <stdexcept>

namespace glory {

// ── helpers ─────────────────────────────────────────────────────────────────
static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Failed to open shader: " + path);
    auto sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

static VkShaderModule createShaderModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "Create SSR shader module");
    return mod;
}

// ═══════════════════════════════════════════════════════════════════════════
// SSRPass
// ═══════════════════════════════════════════════════════════════════════════

void SSRPass::init(const Device& device, uint32_t width, uint32_t height,
                   VkImageView depthView, VkSampler depthSampler,
                   VkImageView hdrColorView, VkSampler hdrSampler,
                   VkImageView hizView, VkSampler hizSampler) {
    m_device = &device;
    m_width  = width / 2;  // half-res
    m_height = height / 2;

    createImage();
    createSampler();
    createPipeline();
    createDescriptors(depthView, depthSampler, hdrColorView, hdrSampler,
                      hizView, hizSampler);

    spdlog::info("SSRPass initialized ({}x{} half-res)", m_width, m_height);
}

void SSRPass::recreate(uint32_t width, uint32_t height,
                       VkImageView depthView, VkSampler depthSampler,
                       VkImageView hdrColorView, VkSampler hdrSampler,
                       VkImageView hizView, VkSampler hizSampler) {
    m_width  = width / 2;
    m_height = height / 2;

    m_reflectionImage = Image{};

    if (m_descPool) {
        vkDestroyDescriptorPool(m_device->getDevice(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }

    createImage();
    createDescriptors(depthView, depthSampler, hdrColorView, hdrSampler,
                      hizView, hizSampler);
}

void SSRPass::destroy() { destroyResources(); }

void SSRPass::dispatch(VkCommandBuffer cmd,
                       const glm::mat4& invProj, const glm::mat4& proj,
                       const glm::mat4& view,
                       float maxDistance, float thickness,
                       uint32_t maxSteps) {
    if (!m_enabled) return;

    VkImageSubresourceRange fullRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Transition reflection image to GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = m_reflectionImage.getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);

    struct SSRPushConstants {
        glm::mat4 invProj;
        glm::mat4 proj;
        glm::mat4 view;
        float     maxDistance;
        float     thickness;
        float     roughnessCutoff;
        uint32_t  maxSteps;
        glm::vec4 fallbackColor;
    } pc{};

    pc.invProj         = invProj;
    pc.proj            = proj;
    pc.view            = view;
    pc.maxDistance      = maxDistance;
    pc.thickness       = thickness;
    pc.roughnessCutoff = 0.5f;
    pc.maxSteps        = maxSteps;
    pc.fallbackColor   = glm::vec4(0.05f, 0.05f, 0.08f, 0.0f);

    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_width  + 7) / 8;
    uint32_t gy = (m_height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Transition reflection image to SHADER_READ_ONLY for sampling
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = m_reflectionImage.getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void SSRPass::createImage() {
    m_reflectionImage = Image(*m_device, m_width, m_height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
}

void SSRPass::createSampler() {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod       = 1.0f;

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_sampler),
             "Create SSR sampler");
}

void SSRPass::createPipeline() {
    VkDevice dev = m_device->getDevice();

    // binding 0: depth, binding 1: hdrColor, binding 2: hizPyramid, binding 3: reflectionOut
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "Create SSR descriptor layout");

    // Push constant: 3×mat4(192) + 4×float(16) + vec4(16) = 224 bytes
    VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 224};

    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &m_descLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_pipeLayout),
             "Create SSR pipeline layout");

    auto code = readFile(std::string(SHADER_DIR) + "ssr.comp.spv");
    VkShaderModule mod = createShaderModule(dev, code);

    VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = mod;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_pipeLayout;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline),
             "Create SSR compute pipeline");

    vkDestroyShaderModule(dev, mod, nullptr);
}

void SSRPass::createDescriptors(VkImageView depthView, VkSampler depthSampler,
                                VkImageView hdrColorView, VkSampler hdrSampler,
                                VkImageView hizView, VkSampler hizSampler) {
    VkDevice dev = m_device->getDevice();

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "Create SSR descriptor pool");

    VkDescriptorSetAllocateInfo allocCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocCI.descriptorPool     = m_descPool;
    allocCI.descriptorSetCount = 1;
    allocCI.pSetLayouts        = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocCI, &m_descSet),
             "Allocate SSR descriptor set");

    VkDescriptorImageInfo depthInfo{depthSampler, depthView,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo colorInfo{hdrSampler, hdrColorView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo hizInfo{hizSampler, hizView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo reflInfo{{}, m_reflectionImage.getImageView(),
        VK_IMAGE_LAYOUT_GENERAL};

    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = m_descSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo     = &depthInfo;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo     = &colorInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo     = &hizInfo;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo     = &reflInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void SSRPass::destroyResources() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)   { vkDestroyPipeline(dev, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipeLayout) { vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr); m_pipeLayout = VK_NULL_HANDLE; }
    if (m_descLayout) { vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr); m_descLayout = VK_NULL_HANDLE; }
    if (m_descPool)   { vkDestroyDescriptorPool(dev, m_descPool, nullptr); m_descPool = VK_NULL_HANDLE; }
    if (m_sampler)    { vkDestroySampler(dev, m_sampler, nullptr); m_sampler = VK_NULL_HANDLE; }

    m_reflectionImage = Image{};
}

} // namespace glory
