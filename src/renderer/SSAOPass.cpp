#include "renderer/SSAOPass.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <fstream>
#include <random>
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
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "Create SSAO shader module");
    return mod;
}

// ═══════════════════════════════════════════════════════════════════════════
// SSAOPass
// ═══════════════════════════════════════════════════════════════════════════

void SSAOPass::init(const Device& device, uint32_t width, uint32_t height,
                    VkImageView depthView, VkSampler depthSampler) {
    m_device   = &device;
    m_aoWidth  = width / 2;
    m_aoHeight = height / 2;

    generateKernel();
    createImages();
    createSampler();
    createSSAOPipeline();
    createSSAODescriptors(depthView, depthSampler);
    createBlurPipeline();
    createBlurDescriptors(depthView, depthSampler);

    spdlog::info("SSAOPass initialized ({}x{} half-res)", m_aoWidth, m_aoHeight);
}

void SSAOPass::recreate(uint32_t width, uint32_t height,
                        VkImageView depthView, VkSampler depthSampler) {
    m_aoWidth  = width / 2;
    m_aoHeight = height / 2;

    m_aoImage = Image{};
    m_blurPingPong[0] = Image{};
    m_blurPingPong[1] = Image{};

    if (m_ssaoDescPool) {
        vkDestroyDescriptorPool(m_device->getDevice(), m_ssaoDescPool, nullptr);
        m_ssaoDescPool = VK_NULL_HANDLE;
    }
    if (m_blurDescPool) {
        vkDestroyDescriptorPool(m_device->getDevice(), m_blurDescPool, nullptr);
        m_blurDescPool = VK_NULL_HANDLE;
    }

    createImages();
    createSSAODescriptors(depthView, depthSampler);
    createBlurDescriptors(depthView, depthSampler);
}

void SSAOPass::destroy() {
    destroyResources();
}

void SSAOPass::dispatch(VkCommandBuffer cmd, const glm::mat4& invProj,
                        uint32_t sampleCount, float radius,
                        float bias, float intensity) {
    if (!m_enabled) return;

    VkImageSubresourceRange fullRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Transition AO image to GENERAL for compute write
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = m_aoImage.getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ── SSAO compute dispatch ───────────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_ssaoPipeLayout, 0, 1, &m_ssaoDescSet, 0, nullptr);

    struct SSAOPushConstants {
        glm::mat4 invProj;
        glm::vec4 sampleKernel[32];
        glm::vec4 noiseScale;
        float     radius;
        float     bias;
        float     intensity;
        uint32_t  sampleCount;
    } pc{};

    pc.invProj     = invProj;
    pc.noiseScale  = glm::vec4(m_aoWidth / 4.0f, m_aoHeight / 4.0f, 0.0f, 0.0f);
    pc.radius      = radius;
    pc.bias        = bias;
    pc.intensity   = intensity;
    pc.sampleCount = sampleCount;
    for (uint32_t i = 0; i < 32; ++i) pc.sampleKernel[i] = m_kernel[i];

    vkCmdPushConstants(cmd, m_ssaoPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_aoWidth  + 7) / 8;
    uint32_t gy = (m_aoHeight + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Transition AO image: GENERAL → SHADER_READ for blur input ───────────
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = m_aoImage.getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ── Bilateral blur: horizontal pass (AO → pingpong[0]) ─────────────────
    {
        // Transition pingpong[0] to GENERAL for write
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image = m_blurPingPong[0].getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_blurPipeLayout, 0, 1, &m_blurDescSets[0], 0, nullptr);

    struct BlurPC { uint32_t horizontal; float depthThreshold; } blurPC;
    blurPC.horizontal = 1;
    blurPC.depthThreshold = 0.05f;
    vkCmdPushConstants(cmd, m_blurPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(blurPC), &blurPC);

    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Transition: pingpong[0] → READ, pingpong[1] → GENERAL ──────────────
    {
        VkImageMemoryBarrier2 barriers[2]{};
        barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].image = m_blurPingPong[0].getImage();
        barriers[0].subresourceRange = fullRange;

        barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].image = m_blurPingPong[1].getImage();
        barriers[1].subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers    = barriers;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ── Bilateral blur: vertical pass (pingpong[0] → pingpong[1]) ──────────
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_blurPipeLayout, 0, 1, &m_blurDescSets[1], 0, nullptr);
    blurPC.horizontal = 0;
    vkCmdPushConstants(cmd, m_blurPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(blurPC), &blurPC);
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Final result is in blurPingPong[1] ─────────────────────────────────
    // H pass: aoImage → blurPingPong[0]
    // V pass: blurPingPong[0] → blurPingPong[1]
    // getAOView() returns blurPingPong[1].
    // Transition blurPingPong[1] to SHADER_READ_ONLY for downstream passes to sample
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = m_blurPingPong[1].getImage();
        barrier.subresourceRange = fullRange;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void SSAOPass::generateKernel() {
    std::mt19937 gen(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (uint32_t i = 0; i < 32; ++i) {
        // Random point in hemisphere (z > 0)
        glm::vec3 sample(
            dist(gen) * 2.0f - 1.0f,
            dist(gen) * 2.0f - 1.0f,
            dist(gen)  // z in [0, 1] for hemisphere
        );
        sample = glm::normalize(sample);
        sample *= dist(gen); // random length

        // Accelerate distribution toward center (more close samples)
        float scale = static_cast<float>(i) / 32.0f;
        scale = 0.1f + scale * scale * 0.9f; // lerp(0.1, 1.0, scale²)
        sample *= scale;

        m_kernel[i] = glm::vec4(sample, 0.0f);
    }
}

void SSAOPass::createImages() {
    // Half-resolution R8 images for AO
    m_aoImage = Image(*m_device, m_aoWidth, m_aoHeight,
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    m_blurPingPong[0] = Image(*m_device, m_aoWidth, m_aoHeight,
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    m_blurPingPong[1] = Image(*m_device, m_aoWidth, m_aoHeight,
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
}

void SSAOPass::createSampler() {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod       = 1.0f;

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_aoSampler),
             "Create SSAO sampler");
}

void SSAOPass::createSSAOPipeline() {
    VkDevice dev = m_device->getDevice();

    // binding 0: depth sampler, binding 1: AO output storage image
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_ssaoDescLayout),
             "Create SSAO descriptor layout");

    // Push constant: invProj(64) + kernel(512) + noiseScale(16) + params(16) = 608 bytes
    VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 608};

    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &m_ssaoDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_ssaoPipeLayout),
             "Create SSAO pipeline layout");

    auto code = readFile(std::string(SHADER_DIR) + "ssao.comp.spv");
    VkShaderModule mod = createShaderModule(dev, code);

    VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = mod;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_ssaoPipeLayout;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_ssaoPipeline),
             "Create SSAO compute pipeline");

    vkDestroyShaderModule(dev, mod, nullptr);
}

void SSAOPass::createSSAODescriptors(VkImageView depthView, VkSampler depthSampler) {
    VkDevice dev = m_device->getDevice();

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_ssaoDescPool),
             "Create SSAO descriptor pool");

    VkDescriptorSetAllocateInfo allocCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocCI.descriptorPool     = m_ssaoDescPool;
    allocCI.descriptorSetCount = 1;
    allocCI.pSetLayouts        = &m_ssaoDescLayout;
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocCI, &m_ssaoDescSet),
             "Allocate SSAO descriptor set");

    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = depthSampler;
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo aoOutInfo{};
    aoOutInfo.imageView   = m_aoImage.getImageView();
    aoOutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet          = m_ssaoDescSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &depthInfo;

    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet          = m_ssaoDescSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &aoOutInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void SSAOPass::createBlurPipeline() {
    VkDevice dev = m_device->getDevice();

    // binding 0: AO input (sampler), binding 1: AO output (storage), binding 2: depth (sampler)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_blurDescLayout),
             "Create SSAO blur descriptor layout");

    VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8}; // horizontal(u32) + depthThreshold(f32)

    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &m_blurDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_blurPipeLayout),
             "Create SSAO blur pipeline layout");

    auto code = readFile(std::string(SHADER_DIR) + "ssao_blur.comp.spv");
    VkShaderModule mod = createShaderModule(dev, code);

    VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = mod;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_blurPipeLayout;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_blurPipeline),
             "Create SSAO blur compute pipeline");

    vkDestroyShaderModule(dev, mod, nullptr);
}

void SSAOPass::createBlurDescriptors(VkImageView depthView, VkSampler depthSampler) {
    VkDevice dev = m_device->getDevice();

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}; // 2 sets × (input + depth)
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};         // 2 sets × output

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = 2;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_blurDescPool),
             "Create SSAO blur descriptor pool");

    VkDescriptorSetLayout layouts[2] = {m_blurDescLayout, m_blurDescLayout};
    VkDescriptorSetAllocateInfo allocCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocCI.descriptorPool     = m_blurDescPool;
    allocCI.descriptorSetCount = 2;
    allocCI.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocCI, m_blurDescSets.data()),
             "Allocate SSAO blur descriptor sets");

    VkDescriptorImageInfo depthInfo{depthSampler, depthView,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

    // Set 0: H blur — reads aoImage, writes blurPingPong[0]
    {
        VkDescriptorImageInfo inputInfo{m_aoSampler, m_aoImage.getImageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo outputInfo{{}, m_blurPingPong[0].getImageView(),
            VK_IMAGE_LAYOUT_GENERAL};

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_blurDescSets[0], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &inputInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_blurDescSets[0], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputInfo, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_blurDescSets[0], 2, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr};

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Set 1: V blur — reads blurPingPong[0], writes blurPingPong[1]
    {
        VkDescriptorImageInfo inputInfo{m_aoSampler, m_blurPingPong[0].getImageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo outputInfo{{}, m_blurPingPong[1].getImageView(),
            VK_IMAGE_LAYOUT_GENERAL};

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_blurDescSets[1], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &inputInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_blurDescSets[1], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputInfo, nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_blurDescSets[1], 2, 0, 1,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr};

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void SSAOPass::destroyResources() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    auto destroyPL = [&](VkPipeline& p) { if (p) { vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; } };
    auto destroyPLL = [&](VkPipelineLayout& l) { if (l) { vkDestroyPipelineLayout(dev, l, nullptr); l = VK_NULL_HANDLE; } };
    auto destroyDSL = [&](VkDescriptorSetLayout& l) { if (l) { vkDestroyDescriptorSetLayout(dev, l, nullptr); l = VK_NULL_HANDLE; } };
    auto destroyDP = [&](VkDescriptorPool& p) { if (p) { vkDestroyDescriptorPool(dev, p, nullptr); p = VK_NULL_HANDLE; } };

    destroyPL(m_ssaoPipeline);
    destroyPLL(m_ssaoPipeLayout);
    destroyDSL(m_ssaoDescLayout);
    destroyDP(m_ssaoDescPool);

    destroyPL(m_blurPipeline);
    destroyPLL(m_blurPipeLayout);
    destroyDSL(m_blurDescLayout);
    destroyDP(m_blurDescPool);

    if (m_aoSampler) { vkDestroySampler(dev, m_aoSampler, nullptr); m_aoSampler = VK_NULL_HANDLE; }

    m_aoImage = Image{};
    m_blurPingPong[0] = Image{};
    m_blurPingPong[1] = Image{};
}

} // namespace glory
