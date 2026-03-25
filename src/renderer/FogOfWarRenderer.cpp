#include "renderer/FogOfWarRenderer.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <array>

namespace glory {

// ── loadShader ───────────────────────────────────────────────────────────────
VkShaderModule FogOfWarRenderer::loadShader(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("FoW: cannot open shader: " + path);
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> code(sz);
    file.seekg(0); file.read(code.data(), static_cast<std::streamsize>(sz));
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &mod), "FoW shader module");
    return mod;
}

// ── imageBarrier ─────────────────────────────────────────────────────────────
void FogOfWarRenderer::imageBarrier(VkCommandBuffer cmd, VkImage image,
    VkImageLayout oldLayout, VkImageLayout newLayout,
    VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
    VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage)
{
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask        = srcStage;
    b.srcAccessMask       = srcAccess;
    b.dstStageMask        = dstStage;
    b.dstAccessMask       = dstAccess;
    b.oldLayout           = oldLayout;
    b.newLayout           = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &depInfo);
}

// ── createSampler ─────────────────────────────────────────────────────────────
void FogOfWarRenderer::createSampler() {
    VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter        = VK_FILTER_LINEAR;
    ci.minFilter        = VK_FILTER_LINEAR;
    ci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.anisotropyEnable = VK_FALSE;
    ci.maxAnisotropy    = 1.0f;
    ci.compareEnable    = VK_FALSE;
    ci.minLod           = 0.0f;
    ci.maxLod           = 0.0f;
    ci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_sampler), "FoW sampler");
}

// ── createImages ─────────────────────────────────────────────────────────────
void FogOfWarRenderer::createImages() {
    m_lowResInput = Image(*m_device, 128, 128, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    m_hiResOutput = Image(*m_device, 512, 512, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    m_pingPong = Image(*m_device, 512, 512, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
}

// ── transitionInitialLayouts ──────────────────────────────────────────────────
void FogOfWarRenderer::transitionInitialLayouts() {
    auto poolLock = m_device->lockGraphicsPool();
    VkCommandPool pool  = m_device->getGraphicsCommandPool();

    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device->getDevice(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    imageBarrier(cmd, m_hiResOutput.getImage(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_2_NONE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    imageBarrier(cmd, m_pingPong.getImage(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_2_NONE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos    = &cmdInfo;
    m_device->submitGraphics(1, &submitInfo);
    m_device->graphicsQueueWaitIdle();

    vkFreeCommandBuffers(m_device->getDevice(), pool, 1, &cmd);
}

// ── uploadInitialGrid ─────────────────────────────────────────────────────────
void FogOfWarRenderer::uploadInitialGrid() {
    VmaAllocator alloc = m_device->getAllocator();
    m_stagingBuffer = Buffer(alloc, 128 * 128,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    void* mapped = m_stagingBuffer.map();
    std::memset(mapped, 255, 128 * 128);
    m_stagingBuffer.unmap();
    m_stagingBuffer.flush();

    auto poolLock = m_device->lockGraphicsPool();
    VkCommandPool pool  = m_device->getGraphicsCommandPool();

    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device->getDevice(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    imageBarrier(cmd, m_lowResInput.getImage(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = {0, 0, 0};
    region.imageExtent                     = {128, 128, 1};

    vkCmdCopyBufferToImage(cmd, m_stagingBuffer.getBuffer(), m_lowResInput.getImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    imageBarrier(cmd, m_lowResInput.getImage(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos    = &cmdInfo;
    m_device->submitGraphics(1, &submitInfo);
    m_device->graphicsQueueWaitIdle();

    vkFreeCommandBuffers(m_device->getDevice(), pool, 1, &cmd);
}

// ── createDescriptors ────────────────────────────────────────────────────────
void FogOfWarRenderer::createDescriptors() {
    VkDevice dev = m_device->getDevice();

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    // binding 0: input sampler2D
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    // binding 1: output storage image (r8)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "FoW descriptor set layout");

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets       = 3;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "FoW descriptor pool");

    VkDescriptorSetLayout layouts[3] = {m_descLayout, m_descLayout, m_descLayout};
    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts        = layouts;

    VkDescriptorSet sets[3];
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, sets), "FoW descriptor sets");
    m_upsampleDesc = sets[0];
    m_hBlurDesc    = sets[1];
    m_vBlurDesc    = sets[2];
}

// ── writeDescriptors ─────────────────────────────────────────────────────────
void FogOfWarRenderer::writeDescriptors() {
    VkDevice dev = m_device->getDevice();

    auto writeSet = [&](VkDescriptorSet set,
                        VkImageView inputView, VkImageLayout inputLayout,
                        VkImageView outputView) {
        VkDescriptorImageInfo inputInfo{};
        inputInfo.imageLayout = inputLayout;
        inputInfo.imageView   = inputView;
        inputInfo.sampler     = m_sampler;

        VkDescriptorImageInfo outputInfo{};
        outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        outputInfo.imageView   = outputView;
        outputInfo.sampler     = VK_NULL_HANDLE;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &inputInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &outputInfo;

        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    // upsample: lowRes → hiRes
    writeSet(m_upsampleDesc,
             m_lowResInput.getImageView(),  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             m_hiResOutput.getImageView());

    // hBlur: hiRes → pingPong
    writeSet(m_hBlurDesc,
             m_hiResOutput.getImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             m_pingPong.getImageView());

    // vBlur: pingPong → hiRes
    writeSet(m_vBlurDesc,
             m_pingPong.getImageView(),    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             m_hiResOutput.getImageView());
}

// ── createUpsamplePipeline ────────────────────────────────────────────────────
void FogOfWarRenderer::createUpsamplePipeline() {
    VkDevice dev = m_device->getDevice();

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts    = &m_descLayout;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_upsampleLayout),
             "FoW upsample pipeline layout");

    VkShaderModule mod = loadShader(std::string(SHADER_DIR) + "fow_upsample.comp.spv");

    VkComputePipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeCI.layout             = m_upsampleLayout;
    pipeCI.stage.sType        = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage        = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module       = mod;
    pipeCI.stage.pName        = "main";

    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_upsamplePipeline),
             "FoW upsample pipeline");
    vkDestroyShaderModule(dev, mod, nullptr);
}

// ── createBlurPipeline ────────────────────────────────────────────────────────
void FogOfWarRenderer::createBlurPipeline() {
    VkDevice dev = m_device->getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(int32_t);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_blurLayout),
             "FoW blur pipeline layout");

    VkShaderModule mod = loadShader(std::string(SHADER_DIR) + "fow_blur.comp.spv");

    VkComputePipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeCI.layout             = m_blurLayout;
    pipeCI.stage.sType        = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage        = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module       = mod;
    pipeCI.stage.pName        = "main";

    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_blurPipeline),
             "FoW blur pipeline");
    vkDestroyShaderModule(dev, mod, nullptr);
}

// ── init ─────────────────────────────────────────────────────────────────────
void FogOfWarRenderer::init(const Device& device) {
    m_device = &device;
    createSampler();
    createImages();
    transitionInitialLayouts();
    uploadInitialGrid();
    createDescriptors();
    writeDescriptors();
    createUpsamplePipeline();
    createBlurPipeline();
    spdlog::info("FogOfWarRenderer initialized (128x128 -> 512x512)");
}

// ── updateVisibility ──────────────────────────────────────────────────────────
void FogOfWarRenderer::updateVisibility(const uint8_t* grid, uint32_t width, uint32_t height) {
    if (width != 128 || height != 128) return;
    void* mapped = m_stagingBuffer.map();
    std::memcpy(mapped, grid, width * height);
    m_stagingBuffer.unmap();
    m_stagingBuffer.flush();
    m_dirty = true;
}

// ── dispatch ─────────────────────────────────────────────────────────────────
void FogOfWarRenderer::dispatch(VkCommandBuffer cmd) {
    // 1. Upload dirty grid
    if (m_dirty) {
        imageBarrier(cmd, m_lowResInput.getImage(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);

        VkBufferImageCopy region{};
        region.bufferOffset                    = 0;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset                     = {0, 0, 0};
        region.imageExtent                     = {128, 128, 1};
        vkCmdCopyBufferToImage(cmd, m_stagingBuffer.getBuffer(), m_lowResInput.getImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        imageBarrier(cmd, m_lowResInput.getImage(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

        m_dirty = false;
    }

    // 2. Prepare hiResOutput for compute write
    // First frame: already GENERAL from transitionInitialLayouts
    // Subsequent frames: transition from SHADER_READ_ONLY (left by step 9)
    if (!m_firstDispatch) {
        imageBarrier(cmd, m_hiResOutput.getImage(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    // 3. Upsample: lowRes(128) → hiRes(512)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsamplePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_upsampleLayout,
                            0, 1, &m_upsampleDesc, 0, nullptr);
    vkCmdDispatch(cmd, 32, 32, 1);  // 512/16=32

    // 4. Barrier: hiRes GENERAL → SHADER_READ_ONLY for hBlur to read
    imageBarrier(cmd, m_hiResOutput.getImage(),
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    // 5. Prepare pingPong for hBlur write
    // First frame: pingPong is GENERAL from transitionInitialLayouts
    // Subsequent frames: SHADER_READ_ONLY from previous vBlur read
    if (!m_firstDispatch) {
        imageBarrier(cmd, m_pingPong.getImage(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    // 6. Horizontal blur: hiRes → pingPong
    int32_t horizontal = 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurLayout,
                            0, 1, &m_hBlurDesc, 0, nullptr);
    vkCmdPushConstants(cmd, m_blurLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(int32_t), &horizontal);
    vkCmdDispatch(cmd, 32, 32, 1);

    // 7. pingPong GENERAL → SHADER_READ_ONLY for vBlur to read;
    //    hiRes SHADER_READ_ONLY → GENERAL for vBlur to write
    imageBarrier(cmd, m_pingPong.getImage(),
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    imageBarrier(cmd, m_hiResOutput.getImage(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    // 8. Vertical blur: pingPong → hiRes
    int32_t vertical = 0;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_blurLayout,
                            0, 1, &m_vBlurDesc, 0, nullptr);
    vkCmdPushConstants(cmd, m_blurLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(int32_t), &vertical);
    vkCmdDispatch(cmd, 32, 32, 1);

    // 9. Final barrier: hiRes GENERAL → SHADER_READ_ONLY for fragment shader
    imageBarrier(cmd, m_hiResOutput.getImage(),
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    m_firstDispatch = false;
}

// ── destroy ──────────────────────────────────────────────────────────────────
void FogOfWarRenderer::destroy() {
    if (!m_device) return;

    vkDeviceWaitIdle(m_device->getDevice());
    VkDevice dev = m_device->getDevice();

    if (m_upsamplePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_upsamplePipeline, nullptr);
    m_upsamplePipeline = VK_NULL_HANDLE;

    if (m_upsampleLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_upsampleLayout, nullptr);
    m_upsampleLayout = VK_NULL_HANDLE;

    if (m_blurPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_blurPipeline, nullptr);
    m_blurPipeline = VK_NULL_HANDLE;

    if (m_blurLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_blurLayout, nullptr);
    m_blurLayout = VK_NULL_HANDLE;

    if (m_descPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    m_descPool     = VK_NULL_HANDLE;
    m_upsampleDesc = VK_NULL_HANDLE;
    m_hBlurDesc    = VK_NULL_HANDLE;
    m_vBlurDesc    = VK_NULL_HANDLE;

    if (m_descLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    m_descLayout = VK_NULL_HANDLE;

    if (m_sampler != VK_NULL_HANDLE)
        vkDestroySampler(dev, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;

    m_hiResOutput  = Image{};
    m_pingPong     = Image{};
    m_lowResInput  = Image{};
    m_stagingBuffer = Buffer{};

    m_device = nullptr;
}

} // namespace glory
