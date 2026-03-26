#include "renderer/SSRPass.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <stdexcept>
#include <array>

namespace glory {

// ── File helpers (same as other passes) ──────────────────────────────────────
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
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "Create shader module");
    return mod;
}

// ── init ─────────────────────────────────────────────────────────────────────
void SSRPass::init(const Device& device, uint32_t width, uint32_t height,
                   VkImageView colorView, VkImageView depthView,
                   VkSampler sceneSampler) {
    m_device     = &device;
    m_fullWidth  = width;
    m_fullHeight = height;
    m_ssrWidth   = width / 2;
    m_ssrHeight  = height / 2;

    createImage();
    createSampler();
    createPipeline();
    createDescriptors(colorView, depthView, sceneSampler);

    spdlog::info("[SSR] Initialized ({}×{} → {}×{} output)",
                 width, height, m_ssrWidth, m_ssrHeight);
}

// ── dispatch ─────────────────────────────────────────────────────────────────
void SSRPass::dispatch(VkCommandBuffer cmd,
                       const glm::mat4& viewProj,
                       const glm::mat4& invViewProj,
                       const glm::vec3& cameraPos,
                       float maxDistance,
                       float thickness,
                       uint32_t maxSteps,
                       float stepStride) {
    if (!m_enabled) return;

    VkImageSubresourceRange fullRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Transition SSR output: UNDEFINED/SHADER_READ → GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image         = m_ssrImage.getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);

    // Push constants: match the shader layout
    struct SSRPushConstants {
        glm::mat4 viewProj;       // 64 bytes, offset 0
        glm::mat4 invViewProj;    // 64 bytes, offset 64
        glm::vec4 cameraPos;      // 16 bytes, offset 128
        glm::vec2 screenSize;     //  8 bytes, offset 144
        float     maxDistance;     //  4 bytes, offset 152
        float     thickness;      //  4 bytes, offset 156
        uint32_t  maxSteps;       //  4 bytes, offset 160
        float     stepStride;     //  4 bytes, offset 164
        float     _pad0;          //  4 bytes, offset 168
        float     _pad1;          //  4 bytes, offset 172
    };                            // total = 176 bytes

    SSRPushConstants pc{};
    pc.viewProj    = viewProj;
    pc.invViewProj = invViewProj;
    pc.cameraPos   = glm::vec4(cameraPos, 0.0f);
    pc.screenSize  = glm::vec2(static_cast<float>(m_fullWidth),
                               static_cast<float>(m_fullHeight));
    pc.maxDistance  = maxDistance;
    pc.thickness   = thickness;
    pc.maxSteps    = maxSteps;
    pc.stepStride  = stepStride;
    pc._pad0       = 0.0f;
    pc._pad1       = 0.0f;

    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_ssrWidth  + 7) / 8;
    uint32_t gy = (m_ssrHeight + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Transition SSR output: GENERAL → SHADER_READ_ONLY for downstream sampling
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image         = m_ssrImage.getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

// ── recreate (resize) ────────────────────────────────────────────────────────
void SSRPass::recreate(uint32_t width, uint32_t height,
                       VkImageView colorView, VkImageView depthView,
                       VkSampler sceneSampler) {
    destroyResources();
    m_fullWidth  = width;
    m_fullHeight = height;
    m_ssrWidth   = width / 2;
    m_ssrHeight  = height / 2;
    createImage();
    createSampler();
    createPipeline();
    createDescriptors(colorView, depthView, sceneSampler);
    spdlog::info("[SSR] Recreated ({}×{})", m_ssrWidth, m_ssrHeight);
}

// ── destroy ──────────────────────────────────────────────────────────────────
void SSRPass::destroy() {
    destroyResources();
}

// ── Private: createImage ─────────────────────────────────────────────────────
void SSRPass::createImage() {
    m_ssrImage = Image(*m_device, m_ssrWidth, m_ssrHeight,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
}

// ── Private: createSampler ───────────────────────────────────────────────────
void SSRPass::createSampler() {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod       = 1.0f;

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_ssrSampler),
             "Create SSR sampler");
}

// ── Private: createPipeline ──────────────────────────────────────────────────
void SSRPass::createPipeline() {
    VkDevice dev = m_device->getDevice();

    // Descriptor set layout:
    //   binding 0: scene color (sampler2D)
    //   binding 1: depth (sampler2D)
    //   binding 2: SSR output (storage image, rgba16f)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "Create SSR descriptor layout");

    // Push constant: 176 bytes (2 mat4 + vec4 + vec2 + 4 floats/uint + 2 pad)
    VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 176};

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

// ── Private: createDescriptors ───────────────────────────────────────────────
void SSRPass::createDescriptors(VkImageView colorView, VkImageView depthView,
                                VkSampler sceneSampler) {
    VkDevice dev = m_device->getDevice();

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
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

    // binding 0: scene color
    VkDescriptorImageInfo colorInfo{};
    colorInfo.sampler     = sceneSampler;
    colorInfo.imageView   = colorView;
    colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // binding 1: depth
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = sceneSampler;
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    // binding 2: SSR output (storage image)
    VkDescriptorImageInfo ssrOutInfo{};
    ssrOutInfo.imageView   = m_ssrImage.getImageView();
    ssrOutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &colorInfo;

    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &depthInfo;

    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet          = m_descSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo      = &ssrOutInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ── Private: destroyResources ────────────────────────────────────────────────
void SSRPass::destroyResources() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    auto safeDestroy = [&](auto destroyFn, auto& handle) {
        if (handle) { destroyFn(dev, handle, nullptr); handle = VK_NULL_HANDLE; }
    };

    safeDestroy(vkDestroyPipeline,            m_pipeline);
    safeDestroy(vkDestroyPipelineLayout,      m_pipeLayout);
    safeDestroy(vkDestroyDescriptorSetLayout, m_descLayout);
    safeDestroy(vkDestroyDescriptorPool,      m_descPool);
    safeDestroy(vkDestroySampler,             m_ssrSampler);

    m_descSet = VK_NULL_HANDLE;
    m_ssrImage = Image{};
}

} // namespace glory
