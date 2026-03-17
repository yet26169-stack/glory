#include "renderer/HiZPass.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
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
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "Failed to create shader module");
    return mod;
}

// ═══════════════════════════════════════════════════════════════════════════
// HiZPass
// ═══════════════════════════════════════════════════════════════════════════

void HiZPass::init(const Device& device, uint32_t width, uint32_t height) {
    m_device = &device;
    m_width  = width;
    m_height = height;
    m_mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    createPyramidImage();
    createMipViews();
    createSampler();
    createComputePipeline();
    createDescriptors();

    spdlog::info("HiZPass initialized ({}×{}, {} mip levels)", width, height, m_mipLevels);
}

void HiZPass::destroy() {
    destroyResources();
}

void HiZPass::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    destroyResources();
    m_width  = width;
    m_height = height;
    m_mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    createPyramidImage();
    createMipViews();
    createSampler();
    createComputePipeline();
    createDescriptors();
}

void HiZPass::generate(VkCommandBuffer cmd, VkImageView sourceDepthView) {
    // Transition pyramid image to general for compute writes
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image         = m_pyramidImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = m_mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

    // Generate each mip level
    for (uint32_t mip = 0; mip < m_mipLevels - 1; ++mip) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_pipeLayout, 0, 1, &m_descSets[mip], 0, nullptr);

        uint32_t dstW = std::max(1u, m_width  >> (mip + 1));
        uint32_t dstH = std::max(1u, m_height >> (mip + 1));
        uint32_t groupsX = (dstW + 7) / 8;
        uint32_t groupsY = (dstH + 7) / 8;
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        // Barrier between mip levels
        VkImageMemoryBarrier2 mipBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        mipBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mipBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mipBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mipBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        mipBarrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        mipBarrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        mipBarrier.image         = m_pyramidImage;
        mipBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        mipBarrier.subresourceRange.baseMipLevel   = mip + 1;
        mipBarrier.subresourceRange.levelCount     = 1;
        mipBarrier.subresourceRange.baseArrayLayer = 0;
        mipBarrier.subresourceRange.layerCount     = 1;

        VkDependencyInfo mipDepInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        mipDepInfo.imageMemoryBarrierCount = 1;
        mipDepInfo.pImageMemoryBarriers    = &mipBarrier;
        vkCmdPipelineBarrier2(cmd, &mipDepInfo);
    }

    // Final transition to shader-read for the cull pass
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount   = m_mipLevels;

    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);
}

// ── private ─────────────────────────────────────────────────────────────────

void HiZPass::createPyramidImage() {
    VkImageCreateInfo imgCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = VK_FORMAT_R32_SFLOAT;
    imgCI.extent        = {m_width, m_height, 1};
    imgCI.mipLevels     = m_mipLevels;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VK_CHECK(vmaCreateImage(m_device->getAllocator(), &imgCI, &allocCI,
                            &m_pyramidImage, &m_pyramidAlloc, nullptr),
             "Failed to create Hi-Z pyramid image");

    // Full-mip-chain image view
    VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.image    = m_pyramidImage;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = VK_FORMAT_R32_SFLOAT;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = m_mipLevels;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    VK_CHECK(vkCreateImageView(m_device->getDevice(), &viewCI, nullptr, &m_pyramidView),
             "Failed to create Hi-Z pyramid view");
}

void HiZPass::createMipViews() {
    m_mipViews.resize(m_mipLevels);
    for (uint32_t mip = 0; mip < m_mipLevels; ++mip) {
        VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewCI.image    = m_pyramidImage;
        viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format   = VK_FORMAT_R32_SFLOAT;
        viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCI.subresourceRange.baseMipLevel   = mip;
        viewCI.subresourceRange.levelCount     = 1;
        viewCI.subresourceRange.baseArrayLayer = 0;
        viewCI.subresourceRange.layerCount     = 1;

        VK_CHECK(vkCreateImageView(m_device->getDevice(), &viewCI, nullptr, &m_mipViews[mip]),
                 "Failed to create Hi-Z mip view");
    }
}

void HiZPass::createSampler() {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod       = static_cast<float>(m_mipLevels);

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_sampler),
             "Failed to create Hi-Z sampler");
}

void HiZPass::createComputePipeline() {
    VkDevice dev = m_device->getDevice();
    std::string shaderDir = SHADER_DIR;

    // Descriptor layout: binding 0 = input sampler, binding 1 = output storage image
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "Failed to create Hi-Z descriptor layout");

    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &m_descLayout;

    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_pipeLayout),
             "Failed to create Hi-Z pipeline layout");

    auto compCode = readFile(shaderDir + "/hiz_generate.comp.spv");
    VkShaderModule compMod = createShaderModule(dev, compCode);

    VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = compMod;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_pipeLayout;

    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline),
             "Failed to create Hi-Z compute pipeline");

    vkDestroyShaderModule(dev, compMod, nullptr);
}

void HiZPass::createDescriptors() {
    VkDevice dev = m_device->getDevice();
    uint32_t setCount = m_mipLevels > 0 ? m_mipLevels - 1 : 0;

    // Pool
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = setCount;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = setCount;

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = setCount;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();

    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "Failed to create Hi-Z descriptor pool");

    // Allocate sets
    std::vector<VkDescriptorSetLayout> layouts(setCount, m_descLayout);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = setCount;
    allocInfo.pSetLayouts        = layouts.data();

    m_descSets.resize(setCount);
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, m_descSets.data()),
             "Failed to allocate Hi-Z descriptor sets");

    // Write descriptors: each set reads mip N and writes mip N+1
    for (uint32_t mip = 0; mip < setCount; ++mip) {
        VkDescriptorImageInfo inputInfo{};
        inputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        inputInfo.imageView   = m_mipViews[mip];
        inputInfo.sampler     = m_sampler;

        VkDescriptorImageInfo outputInfo{};
        outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        outputInfo.imageView   = m_mipViews[mip + 1];

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descSets[mip];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &inputInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descSets[mip];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &outputInfo;

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void HiZPass::destroyResources() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)   vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_descPool)   vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_sampler)    vkDestroySampler(dev, m_sampler, nullptr);

    for (auto view : m_mipViews) {
        if (view) vkDestroyImageView(dev, view, nullptr);
    }
    m_mipViews.clear();

    if (m_pyramidView)  vkDestroyImageView(dev, m_pyramidView, nullptr);
    if (m_pyramidImage) vmaDestroyImage(m_device->getAllocator(), m_pyramidImage, m_pyramidAlloc);

    m_pipeline = VK_NULL_HANDLE;
    m_pipeLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_pyramidView = VK_NULL_HANDLE;
    m_pyramidImage = VK_NULL_HANDLE;
    m_pyramidAlloc = VK_NULL_HANDLE;
}

// ═══════════════════════════════════════════════════════════════════════════
// GpuCuller — GPU frustum + occlusion culling via compute shader
// ═══════════════════════════════════════════════════════════════════════════

void GpuCuller::init(const Device& device, uint32_t maxObjects) {
    m_device     = &device;
    m_maxObjects = maxObjects;

    VmaAllocator alloc = device.getAllocator();
    // Draw command size: uint32_t drawCount + maxObjects * VkDrawIndexedIndirectCommand (20 bytes each)
    VkDeviceSize drawBufSize = sizeof(uint32_t) + maxObjects * 20;
    VkDeviceSize visBufSize  = maxObjects * sizeof(uint32_t);

    constexpr uint32_t FRAMES = 2;
    m_frames.reserve(FRAMES);
    for (uint32_t i = 0; i < FRAMES; ++i) {
        FrameResources fr;
        fr.drawBuffer = Buffer(alloc, drawBufSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);

        fr.visibilityFlags = Buffer(alloc, visBufSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);

        fr.cullParamsBuffer = Buffer(alloc, sizeof(GpuCullParams),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        m_frames.push_back(std::move(fr));
    }

    createComputePipeline();
    createDescriptors();

    spdlog::info("GpuCuller initialized (max {} objects, {} frames)", maxObjects, FRAMES);
}

void GpuCuller::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)   vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_descPool)   vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);

    m_pipeline   = VK_NULL_HANDLE;
    m_pipeLayout = VK_NULL_HANDLE;
    m_descPool   = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_frames.clear();
    m_device = nullptr;
}

void GpuCuller::dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                         VkBuffer sceneBuffer, VkDeviceSize sceneBufferSize,
                         VkImageView hizView, VkSampler hizSampler,
                         const CullParams& params) {
    auto& fr = m_frames[frameIndex];
    VkDevice dev = m_device->getDevice();

    // Upload cull params
    GpuCullParams gpu{};
    gpu.viewProj = params.viewProj;
    std::memcpy(gpu.frustumPlanes, params.frustumPlanes, sizeof(gpu.frustumPlanes));
    gpu.screenSize = glm::vec4(
        static_cast<float>(params.screenWidth),
        static_cast<float>(params.screenHeight),
        1.0f / static_cast<float>(params.screenWidth),
        1.0f / static_cast<float>(params.screenHeight));
    gpu.objectCount = params.objectCount;
    gpu.phase       = params.phase;
    std::memcpy(fr.cullParamsBuffer.map(), &gpu, sizeof(gpu));
    fr.cullParamsBuffer.flush();

    // Reset drawCount to 0 (first 4 bytes of drawBuffer)
    vkCmdFillBuffer(cmd, fr.drawBuffer.getBuffer(), 0, sizeof(uint32_t), 0);

    // If phase 0, also clear visibility flags
    if (params.phase == 0) {
        vkCmdFillBuffer(cmd, fr.visibilityFlags.getBuffer(), 0,
                        m_maxObjects * sizeof(uint32_t), 0);
    }

    // Barrier: fillBuffer → compute shader read
    VkMemoryBarrier2 fillBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    fillBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    fillBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    fillBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    fillBarrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers    = &fillBarrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    // Update descriptor set with current frame's scene buffer and HiZ view
    VkDescriptorBufferInfo sceneBufInfo{};
    sceneBufInfo.buffer = sceneBuffer;
    sceneBufInfo.offset = 0;
    sceneBufInfo.range  = sceneBufferSize;

    VkDescriptorImageInfo hizInfo{};
    hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hizInfo.imageView   = hizView;
    hizInfo.sampler     = hizSampler;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet          = m_descSets[frameIndex];
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo     = &sceneBufInfo;

    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet          = m_descSets[frameIndex];
    writes[1].dstBinding      = 3;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &hizInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeLayout,
                            0, 1, &m_descSets[frameIndex], 0, nullptr);

    uint32_t groupCount = (params.objectCount + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Barrier: compute write → indirect draw read
    VkMemoryBarrier2 computeBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    computeBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    computeBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    computeBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                                 | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    computeBarrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
                                 | VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo postDep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    postDep.memoryBarrierCount = 1;
    postDep.pMemoryBarriers    = &computeBarrier;
    vkCmdPipelineBarrier2(cmd, &postDep);
}

VkBuffer GpuCuller::getIndirectBuffer(uint32_t frameIndex) const {
    return m_frames[frameIndex].drawBuffer.getBuffer();
}

VkBuffer GpuCuller::getCountBuffer(uint32_t frameIndex) const {
    return m_frames[frameIndex].drawBuffer.getBuffer();
}

void GpuCuller::createComputePipeline() {
    VkDevice dev = m_device->getDevice();
    std::string shaderDir = SHADER_DIR;

    // 6 bindings: scene SSBO, draw SSBO, visibility SSBO, HiZ sampler, cull params UBO, LOD SSBO
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[5].binding         = 5;
    bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "Failed to create GpuCuller descriptor layout");

    // No push constants — all params go via UBO
    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts    = &m_descLayout;

    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_pipeLayout),
             "Failed to create GpuCuller pipeline layout");

    auto compCode = readFile(shaderDir + "/occlusion_cull.comp.spv");
    VkShaderModule compMod = createShaderModule(dev, compCode);

    VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = compMod;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_pipeLayout;

    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline),
             "Failed to create GpuCuller compute pipeline");

    vkDestroyShaderModule(dev, compMod, nullptr);
}

void GpuCuller::createDescriptors() {
    VkDevice dev = m_device->getDevice();
    uint32_t frameCount = static_cast<uint32_t>(m_frames.size());

    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = frameCount * 4; // scene + draw + visibility + LOD per frame
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = frameCount;     // HiZ per frame
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = frameCount;     // cull params per frame

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    poolCI.maxSets       = frameCount;

    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "Failed to create GpuCuller descriptor pool");

    std::vector<VkDescriptorSetLayout> layouts(frameCount, m_descLayout);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = frameCount;
    allocInfo.pSetLayouts        = layouts.data();

    m_descSets.resize(frameCount);
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, m_descSets.data()),
             "Failed to allocate GpuCuller descriptor sets");

    // Write static bindings (draw buffer, visibility flags, cull params) — scene & HiZ are updated per-dispatch
    VkDeviceSize drawBufSize = sizeof(uint32_t) + m_maxObjects * 20;
    VkDeviceSize visBufSize  = m_maxObjects * sizeof(uint32_t);

    for (uint32_t i = 0; i < frameCount; ++i) {
        VkDescriptorBufferInfo drawInfo{};
        drawInfo.buffer = m_frames[i].drawBuffer.getBuffer();
        drawInfo.range  = drawBufSize;

        VkDescriptorBufferInfo visInfo{};
        visInfo.buffer = m_frames[i].visibilityFlags.getBuffer();
        visInfo.range  = visBufSize;

        VkDescriptorBufferInfo paramInfo{};
        paramInfo.buffer = m_frames[i].cullParamsBuffer.getBuffer();
        paramInfo.range  = sizeof(GpuCullParams);

        std::array<VkWriteDescriptorSet, 3> writes{};

        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet          = m_descSets[i];
        writes[0].dstBinding      = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &drawInfo;

        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet          = m_descSets[i];
        writes[1].dstBinding      = 2;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &visInfo;

        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet          = m_descSets[i];
        writes[2].dstBinding      = 4;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo     = &paramInfo;

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

} // namespace glory
