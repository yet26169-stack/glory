#include "renderer/ColorGradePass.h"
#include "renderer/VkCheck.h"
#include "renderer/Buffer.h"

#include <spdlog/spdlog.h>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <stb_image.h>

namespace glory {

// ── File reader (same pattern as other passes) ──────────────────────────────
static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Failed to open shader: " + path);
    auto sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

// ═════════════════════════════════════════════════════════════════════════════
// init / destroy
// ═════════════════════════════════════════════════════════════════════════════

void ColorGradePass::init(const Device& device, const RenderFormats& formats,
                          VkImageView sceneView, VkSampler sampler,
                          VkImageView lutView, uint32_t lutSize) {
    m_device    = &device;
    m_formats   = formats;
    m_sceneView = sceneView;
    m_lutView   = lutView;
    m_sampler   = sampler;
    m_lutSize   = lutSize;

    createLutSampler();
    createDescriptorSetLayout();
    createDescriptorPool();
    createDescriptorSet();
    createPipeline();

    spdlog::info("[ColorGrade] Initialized (LUT size {})", m_lutSize);
}

void ColorGradePass::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline != VK_NULL_HANDLE)       { vkDestroyPipeline(dev, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_lutSampler != VK_NULL_HANDLE)     { vkDestroySampler(dev, m_lutSampler, nullptr); m_lutSampler = VK_NULL_HANDLE; }
}

// ═════════════════════════════════════════════════════════════════════════════
// render
// ═════════════════════════════════════════════════════════════════════════════

void ColorGradePass::render(VkCommandBuffer cmd, float lutIntensity) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    PushConstants pc{lutIntensity, static_cast<float>(m_lutSize)};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Descriptor set management
// ═════════════════════════════════════════════════════════════════════════════

void ColorGradePass::updateDescriptorSets(VkImageView sceneView, VkImageView lutView) {
    m_sceneView = sceneView;
    m_lutView   = lutView;

    VkDescriptorImageInfo sceneInfo{};
    sceneInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneInfo.imageView   = m_sceneView;
    sceneInfo.sampler     = m_sampler;

    VkDescriptorImageInfo lutInfo{};
    lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    lutInfo.imageView   = m_lutView;
    lutInfo.sampler     = m_lutSampler;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &sceneInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &lutInfo;

    vkUpdateDescriptorSets(m_device->getDevice(), 2, writes, 0, nullptr);
}

void ColorGradePass::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding bindings[2]{};
    // Binding 0: scene (tone-mapped result)
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Binding 1: 3D LUT texture
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings    = bindings;

    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &layoutInfo, nullptr,
                                         &m_descriptorSetLayout),
             "Create ColorGrade descriptor set layout");
}

void ColorGradePass::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 1;

    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &poolInfo, nullptr,
                                    &m_descriptorPool),
             "Create ColorGrade descriptor pool");
}

void ColorGradePass::createDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorSetLayout;

    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &allocInfo, &m_descriptorSet),
             "Allocate ColorGrade descriptor set");

    updateDescriptorSets(m_sceneView, m_lutView);
}

// ═════════════════════════════════════════════════════════════════════════════
// Pipeline
// ═════════════════════════════════════════════════════════════════════════════

void ColorGradePass::createPipeline() {
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    VK_CHECK(vkCreatePipelineLayout(m_device->getDevice(), &layoutInfo, nullptr,
                                    &m_pipelineLayout),
             "Create ColorGrade pipeline layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "color_grade.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "color_grade.frag.spv");

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAsm{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth   = 1.0f;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments    = &blendAttach;

    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynState.pDynamicStates    = dynStates.data();

    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pipelineCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAsm;
    pipelineCI.pViewportState      = &vpState;
    pipelineCI.pRasterizationState = &raster;
    pipelineCI.pMultisampleState   = &ms;
    pipelineCI.pColorBlendState    = &blending;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_pipelineLayout;
    pipelineCI.pNext               = &fmtCI;
    pipelineCI.renderPass          = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(m_device->getDevice(), VK_NULL_HANDLE,
                                       1, &pipelineCI, nullptr, &m_pipeline),
             "Create ColorGrade pipeline");

    vkDestroyShaderModule(m_device->getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_device->getDevice(), fragModule, nullptr);
}

void ColorGradePass::createLutSampler() {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod       = 0.0f;

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_lutSampler),
             "Create LUT sampler");
}

VkShaderModule ColorGradePass::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &mod);
    return mod;
}

// ═════════════════════════════════════════════════════════════════════════════
// 3D LUT creation helpers
// ═════════════════════════════════════════════════════════════════════════════

static void uploadLUT3D(const Device& device, VkImage image,
                        const uint8_t* pixels, uint32_t size) {
    VkDeviceSize byteSize = size * size * size * 4;

    // Staging buffer
    Buffer staging(device.getAllocator(), byteSize,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* mapped = staging.map();
    std::memcpy(mapped, pixels, byteSize);
    staging.unmap();

    // One-shot command buffer
    VkCommandPool pool;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        ci.queueFamilyIndex = device.getQueueFamilies().graphicsFamily.value();
        VK_CHECK(vkCreateCommandPool(device.getDevice(), &ci, nullptr, &pool),
                 "LUT upload cmd pool");
    }

    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device.getDevice(), &ai, &cmd);
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Transition UNDEFINED → TRANSFER_DST
    {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask       = 0;
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.image               = image;
        bar.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
    }

    // Copy buffer → 3D image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {size, size, size};
    vkCmdCopyBufferToImage(cmd, staging.getBuffer(), image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.image               = image;
        bar.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(device.getGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.getGraphicsQueue());

    vkDestroyCommandPool(device.getDevice(), pool, nullptr);
}

/// Create a VK_IMAGE_TYPE_3D image + view for a LUT.
static ColorGradePass::LUTImage createLUT3DImage(const Device& device, uint32_t size) {
    ColorGradePass::LUTImage lut;
    lut.size = size;

    VkImageCreateInfo imgCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgCI.imageType     = VK_IMAGE_TYPE_3D;
    imgCI.extent        = {size, size, size};
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgCI.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VK_CHECK(vmaCreateImage(device.getAllocator(), &imgCI, &allocCI,
                            &lut.image, &lut.allocation, nullptr),
             "Create 3D LUT image");

    VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.image    = lut.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewCI.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VK_CHECK(vkCreateImageView(device.getDevice(), &viewCI, nullptr, &lut.imageView),
             "Create 3D LUT image view");

    return lut;
}

ColorGradePass::LUTImage ColorGradePass::createNeutralLUT(const Device& device,
                                                           uint32_t size) {
    // Build identity pixel data: output(r,g,b) = input(r,g,b)
    std::vector<uint8_t> pixels(size * size * size * 4);
    for (uint32_t b = 0; b < size; ++b) {
        for (uint32_t g = 0; g < size; ++g) {
            for (uint32_t r = 0; r < size; ++r) {
                uint32_t idx = (b * size * size + g * size + r) * 4;
                pixels[idx + 0] = static_cast<uint8_t>(r * 255 / (size - 1));
                pixels[idx + 1] = static_cast<uint8_t>(g * 255 / (size - 1));
                pixels[idx + 2] = static_cast<uint8_t>(b * 255 / (size - 1));
                pixels[idx + 3] = 255;
            }
        }
    }

    LUTImage lut = createLUT3DImage(device, size);
    uploadLUT3D(device, lut.image, pixels.data(), size);
    spdlog::info("[ColorGrade] Created neutral {}³ LUT", size);
    return lut;
}

ColorGradePass::LUTImage ColorGradePass::loadLUT(const Device& device,
                                                   const std::string& path) {
    int w, h, ch;
    uint8_t* raw = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!raw) {
        spdlog::warn("[ColorGrade] Failed to load LUT '{}', using neutral", path);
        return createNeutralLUT(device);
    }

    // Determine LUT size from the horizontal-strip layout: width = size², height = size
    uint32_t size = static_cast<uint32_t>(h);
    if (static_cast<uint32_t>(w) != size * size) {
        spdlog::warn("[ColorGrade] LUT '{}' has unexpected dimensions {}x{} "
                     "(expected {}x{}), using neutral",
                     path, w, h, size * size, size);
        stbi_image_free(raw);
        return createNeutralLUT(device);
    }

    // Re-order from horizontal strip (size slices side-by-side) to linear 3D data
    // Strip layout: for each blue slice z, the 2D sub-image at x=[z*size..(z+1)*size), y=[0..size)
    // 3D data layout: for z (blue), y (green), x (red) → contiguous
    std::vector<uint8_t> pixels(size * size * size * 4);
    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                uint32_t srcX = z * size + x;
                uint32_t srcIdx = (y * w + srcX) * 4;
                uint32_t dstIdx = (z * size * size + y * size + x) * 4;
                std::memcpy(&pixels[dstIdx], &raw[srcIdx], 4);
            }
        }
    }

    stbi_image_free(raw);

    LUTImage lut = createLUT3DImage(device, size);
    uploadLUT3D(device, lut.image, pixels.data(), size);
    spdlog::info("[ColorGrade] Loaded {}³ LUT from '{}'", size, path);
    return lut;
}

void ColorGradePass::destroyLUT(const Device& device, LUTImage& lut) {
    if (lut.imageView != VK_NULL_HANDLE)
        vkDestroyImageView(device.getDevice(), lut.imageView, nullptr);
    if (lut.image != VK_NULL_HANDLE)
        vmaDestroyImage(device.getAllocator(), lut.image, lut.allocation);
    lut = {};
}

} // namespace glory
