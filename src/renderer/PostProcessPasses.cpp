#include "renderer/BloomPass.h"
#include "renderer/RenderFormats.h"
#include "renderer/VkCheck.h"
#include <array>
#include <fstream>
#include <stdexcept>
#include "renderer/ToneMapPass.h"
#include "renderer/SSAOPass.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <random>
#include "renderer/InkingPass.h"
#include <vector>
#include "renderer/HiZPass.h"
#include <algorithm>
#include "renderer/ShadowPass.h"
#include "renderer/Descriptors.h"
#include "renderer/Buffer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <limits>

namespace glory {

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

// ─── BloomPass implementation ─────────────────────────────────────────────────

void BloomPass::init(const Device& device, VkImageView hdrColorView, VkSampler sampler,
                    uint32_t width, uint32_t height) {
    m_device = &device;
    m_hdrColorView = hdrColorView;
    m_sampler = sampler;
    m_width = width / 2;
    m_height = height / 2;

    createImages();
    createDescriptorSetLayout();
    createPipelines();
    createDescriptorPool();
    createDescriptorSets();
}

void BloomPass::recreate(VkImageView hdrColorView, uint32_t width, uint32_t height) {
    m_hdrColorView = hdrColorView;
    m_width = width / 2;
    m_height = height / 2;

    m_blurImages.clear();

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device->getDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    createImages();
    createDescriptorPool();
    createDescriptorSets();
}

void BloomPass::destroy() {
    if (!m_device) return;

    if (m_extractPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device->getDevice(), m_extractPipeline, nullptr);
        m_extractPipeline = VK_NULL_HANDLE;
    }
    if (m_blurPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device->getDevice(), m_blurPipeline, nullptr);
        m_blurPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device->getDevice(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device->getDevice(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device->getDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_blurImages.clear();
}

void BloomPass::dispatch(VkCommandBuffer cmd) {
    VkImageSubresourceRange fullRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Helper: set viewport/scissor for bloom half-res render targets
    auto setViewportScissor = [&]() {
        VkViewport vp{0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1};
        VkRect2D   sc{{0, 0}, {m_width, m_height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    };

    // 1. Extract bright areas (HDR -> Blur0)
    {
        // Transition Blur0 from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL
        {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.image = m_blurImages[0].getImage();
            barrier.subresourceRange = fullRange;

            VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(cmd, &depInfo);
        }

        VkRenderingAttachmentInfo colorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttach.imageView = m_blurImages[0].getImageView();
        colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderInfo.renderArea = {{0, 0}, {m_width, m_height}};
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttach;

        vkCmdBeginRendering(cmd, &renderInfo);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_extractPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSets[0], 0, nullptr);

        BloomPushConstants pc{0, 1.0f};
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPushConstants), &pc);

        setViewportScissor();
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        // Transition Blur0 to SHADER_READ_ONLY_OPTIMAL for sampling
        {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image = m_blurImages[0].getImage();
            barrier.subresourceRange = fullRange;

            VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers    = &barrier;
            vkCmdPipelineBarrier2(cmd, &depInfo);
        }
    }

    // 2. Gaussian Blur (Ping-pong)
    int passes = 5;
    for (int i = 0; i < passes; ++i) {
        // Horizontal blur (Blur0 -> Blur1)
        {
            VkImageLayout oldLayout = (i == 0) ? VK_IMAGE_LAYOUT_UNDEFINED
                                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkPipelineStageFlags2 srcStage2 = (i == 0) ? VK_PIPELINE_STAGE_2_NONE
                                                        : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            VkAccessFlags2 srcAccess2 = (i == 0) ? VK_ACCESS_2_NONE
                                                  : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

            // Transition Blur1 to COLOR_ATTACHMENT_OPTIMAL
            {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask  = srcStage2;
                barrier.srcAccessMask = srcAccess2;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.oldLayout = oldLayout;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.image = m_blurImages[1].getImage();
                barrier.subresourceRange = fullRange;

                VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depInfo.imageMemoryBarrierCount = 1;
                depInfo.pImageMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(cmd, &depInfo);
            }

            VkRenderingAttachmentInfo colorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            colorAttach.imageView = m_blurImages[1].getImageView();
            colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

            VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            renderInfo.renderArea = {{0, 0}, {m_width, m_height}};
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAttach;

            vkCmdBeginRendering(cmd, &renderInfo);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSets[1], 0, nullptr);

            BloomPushConstants pc{1, 1.0f};
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPushConstants), &pc);

            setViewportScissor();
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);

            // Transition Blur1 to SHADER_READ_ONLY_OPTIMAL for sampling
            {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.image = m_blurImages[1].getImage();
                barrier.subresourceRange = fullRange;

                VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depInfo.imageMemoryBarrierCount = 1;
                depInfo.pImageMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(cmd, &depInfo);
            }
        }

        // Vertical blur (Blur1 -> Blur0)
        {
            // Transition Blur0 to COLOR_ATTACHMENT_OPTIMAL (was SHADER_READ_ONLY from previous pass)
            {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.image = m_blurImages[0].getImage();
                barrier.subresourceRange = fullRange;

                VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depInfo.imageMemoryBarrierCount = 1;
                depInfo.pImageMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(cmd, &depInfo);
            }

            VkRenderingAttachmentInfo colorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            colorAttach.imageView = m_blurImages[0].getImageView();
            colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

            VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            renderInfo.renderArea = {{0, 0}, {m_width, m_height}};
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAttach;

            vkCmdBeginRendering(cmd, &renderInfo);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSets[2], 0, nullptr);

            BloomPushConstants pc{0, 1.0f};
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPushConstants), &pc);

            setViewportScissor();
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);

            // Transition Blur0 to SHADER_READ_ONLY_OPTIMAL for next pass / final sampling
            {
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.image = m_blurImages[0].getImage();
                barrier.subresourceRange = fullRange;

                VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                depInfo.imageMemoryBarrierCount = 1;
                depInfo.pImageMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(cmd, &depInfo);
            }
        }
    }
}

void BloomPass::createImages() {
    m_blurImages.clear();
    for (int i = 0; i < 2; ++i) {
        m_blurImages.emplace_back(*m_device, m_width, m_height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

void BloomPass::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout),
             "Create Bloom descriptor set layout");
}

void BloomPass::createPipelines() {
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(BloomPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    VK_CHECK(vkCreatePipelineLayout(m_device->getDevice(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout),
             "Create Bloom pipeline layout");

    // Helper to read spv

    // Shaders
    auto vertCode = readFile(std::string(SHADER_DIR) + "tonemap.vert.spv");
    auto extractFragCode = readFile(std::string(SHADER_DIR) + "bloom_extract.frag.spv");
    auto blurFragCode = readFile(std::string(SHADER_DIR) + "bloom_blur.frag.spv");

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule extractFragModule = createShaderModule(extractFragCode);
    VkShaderModule blurFragModule = createShaderModule(blurFragCode);

    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = extractFragModule;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_width;
    viewport.height = (float)m_height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynStates;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;

    RenderFormats bloomFmts = RenderFormats::colorOnly(VK_FORMAT_R16G16B16A16_SFLOAT);
    VkPipelineRenderingCreateInfo dynCI = bloomFmts.pipelineRenderingCI();
    pipelineInfo.pNext = &dynCI;
    pipelineInfo.renderPass = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(m_device->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_extractPipeline),
             "Create Extract pipeline");

    shaderStages[1].module = blurFragModule;
    VK_CHECK(vkCreateGraphicsPipelines(m_device->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_blurPipeline),
             "Create Blur pipeline");

    vkDestroyShaderModule(m_device->getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_device->getDevice(), extractFragModule, nullptr);
    vkDestroyShaderModule(m_device->getDevice(), blurFragModule, nullptr);
}

void BloomPass::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 3;

    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &poolInfo, nullptr, &m_descriptorPool),
             "Create Bloom descriptor pool");
}

void BloomPass::createDescriptorSets() {
    std::vector<VkDescriptorSetLayout> layouts(3, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(3);
    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &allocInfo, m_descriptorSets.data()),
             "Allocate Bloom descriptor sets");

    // 0: Extract (HDR -> Blur0)
    VkDescriptorImageInfo imageInfo0{};
    imageInfo0.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo0.imageView = m_hdrColorView;
    imageInfo0.sampler = m_sampler;

    // 1: Blur H (Blur0 -> Blur1)
    VkDescriptorImageInfo imageInfo1{};
    imageInfo1.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo1.imageView = m_blurImages[0].getImageView();
    imageInfo1.sampler = m_sampler;

    // 2: Blur V (Blur1 -> Blur0)
    VkDescriptorImageInfo imageInfo2{};
    imageInfo2.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo2.imageView = m_blurImages[1].getImageView();
    imageInfo2.sampler = m_sampler;

    VkWriteDescriptorSet descriptorWrites[3] = {};
    for (int i = 0; i < 3; ++i) {
        descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[i].dstSet = m_descriptorSets[i];
        descriptorWrites[i].dstBinding = 0;
        descriptorWrites[i].dstArrayElement = 0;
        descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[i].descriptorCount = 1;
    }
    descriptorWrites[0].pImageInfo = &imageInfo0;
    descriptorWrites[1].pImageInfo = &imageInfo1;
    descriptorWrites[2].pImageInfo = &imageInfo2;

    vkUpdateDescriptorSets(m_device->getDevice(), 3, descriptorWrites, 0, nullptr);
}

VkShaderModule BloomPass::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &mod);
    return mod;
}

// ─── ToneMapPass implementation ───────────────────────────────────────────────

void ToneMapPass::init(const Device& device, const RenderFormats& formats,
                      VkImageView hdrView, VkImageView bloomView, VkSampler sampler) {
    m_device = &device;
    m_formats = formats;
    m_hdrView = hdrView;
    m_bloomView = bloomView;
    m_sampler = sampler;

    createDescriptorSetLayout();
    createDescriptorPool();
    createDescriptorSet();
    createPipeline();
}

void ToneMapPass::render(VkCommandBuffer cmd, float exposure, float bloomStrength,
                         uint32_t enableVignette, uint32_t enableColorGrade,
                         float chromaticAberration, float desaturation) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    ToneMapPushConstants pc{exposure, bloomStrength, enableVignette, enableColorGrade, chromaticAberration, desaturation};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ToneMapPushConstants), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void ToneMapPass::updateDescriptorSets(VkImageView hdrView, VkImageView bloomView) {
    m_hdrView = hdrView;
    m_bloomView = bloomView;

    VkDescriptorImageInfo hdrInfo{};
    hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hdrInfo.imageView = m_hdrView;
    hdrInfo.sampler = m_sampler;

    VkDescriptorImageInfo bloomInfo{};
    bloomInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bloomInfo.imageView = m_bloomView;
    bloomInfo.sampler = m_sampler;

    VkWriteDescriptorSet descriptorWrites[2] = {};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = m_descriptorSet;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &hdrInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = m_descriptorSet;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &bloomInfo;

    vkUpdateDescriptorSets(m_device->getDevice(), 2, descriptorWrites, 0, nullptr);
}

void ToneMapPass::destroy() {
    if (!m_device) return;

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device->getDevice(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device->getDevice(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device->getDevice(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device->getDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
}

void ToneMapPass::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout),
             "Create ToneMap descriptor set layout");
}

void ToneMapPass::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &poolInfo, nullptr, &m_descriptorPool),
             "Create ToneMap descriptor pool");
}

void ToneMapPass::createDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &allocInfo, &m_descriptorSet),
             "Allocate ToneMap descriptor set");

    updateDescriptorSets(m_hdrView, m_bloomView);
}

void ToneMapPass::createPipeline() {
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ToneMapPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    VK_CHECK(vkCreatePipelineLayout(m_device->getDevice(), &pipelineLayoutInfo, nullptr, &m_pipelineLayout),
             "Create ToneMap pipeline layout");


    auto vertCode = readFile(std::string(SHADER_DIR) + "tonemap.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "tonemap.frag.spv");

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragModule;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();
    pipelineInfo.pNext     = &fmtCI;
    pipelineInfo.renderPass = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(m_device->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline),
             "Create ToneMap pipeline");

    vkDestroyShaderModule(m_device->getDevice(), vertModule, nullptr);
    vkDestroyShaderModule(m_device->getDevice(), fragModule, nullptr);
}

VkShaderModule ToneMapPass::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &mod);
    return mod;
}

// ─── SSAOPass implementation ──────────────────────────────────────────────────

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

// ─── InkingPass implementation ────────────────────────────────────────────────

void InkingPass::init(const Device& device, const RenderFormats& formats,
                      VkImageView characterDepthView, VkSampler sampler) {
    m_device  = &device;
    m_sampler = sampler;
    createDescriptorSet(characterDepthView);
    createPipeline(formats);
}

void InkingPass::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();
    vkDeviceWaitIdle(dev);
    if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);            m_pipeline       = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descLayout)     { vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr); m_descLayout    = VK_NULL_HANDLE; }
    if (m_descPool)       { vkDestroyDescriptorPool(dev, m_descPool, nullptr);        m_descPool      = VK_NULL_HANDLE; }
    m_descSet = VK_NULL_HANDLE;
    m_device  = nullptr;
}

void InkingPass::createDescriptorSet(VkImageView characterDepthView) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Inking Desc Layout");

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &poolCI, nullptr, &m_descPool), "Create Inking Desc Pool");

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &allocInfo, &m_descSet), "Alloc Inking Desc Set");

    updateInput(characterDepthView);
}

void InkingPass::updateInput(VkImageView characterDepthView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // matches GENERAL in loadRenderPass subpass ref
    imgInfo.imageView   = characterDepthView;
    imgInfo.sampler     = m_sampler;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device->getDevice(), 1, &write, 0, nullptr);
}

void InkingPass::createPipeline(const RenderFormats& formats) {
    VkDevice dev = m_device->getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(InkPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Inking Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "inking.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "inking.frag.spv");

    VkShaderModule vertMod = createShaderModule(vertCode);
    VkShaderModule fragMod = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    viCI.vertexBindingDescriptionCount   = 0;
    viCI.pVertexBindingDescriptions      = nullptr;
    viCI.vertexAttributeDescriptionCount = 0;
    viCI.pVertexAttributeDescriptions    = nullptr;

    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1;
    vpCI.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsCI.polygonMode = VK_POLYGON_MODE_FILL;
    rsCI.cullMode    = VK_CULL_MODE_NONE;
    rsCI.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsCI.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable  = VK_FALSE;
    dsCI.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable         = VK_TRUE;
    blend.colorWriteMask      = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;

    // Second attachment (charDepth) must be declared with write-mask=0 so the inking
    // pass leaves it unchanged while still matching the render pass's 2-attachment structure.
    VkPipelineColorBlendAttachmentState inkBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments    = inkBlends;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2;
    dynCI.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeCI.stageCount          = 2;
    pipeCI.pStages             = stages;
    pipeCI.pVertexInputState   = &viCI;
    pipeCI.pInputAssemblyState = &iaCI;
    pipeCI.pViewportState      = &vpCI;
    pipeCI.pRasterizationState = &rsCI;
    pipeCI.pMultisampleState   = &msCI;
    pipeCI.pDepthStencilState  = &dsCI;
    pipeCI.pColorBlendState    = &cbCI;
    pipeCI.pDynamicState       = &dynCI;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    pipeCI.pNext               = &fmtCI;
    pipeCI.layout              = m_pipelineLayout;
    pipeCI.renderPass          = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline), "Create Inking Pipeline");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule InkingPass::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &module), "Create Inking Shader Module");
    return module;
}

void InkingPass::render(VkCommandBuffer cmd, float threshold, float thickness,
                        const glm::vec4& inkColor) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descSet, 0, nullptr);
    InkPC pc{ inkColor, threshold, thickness };
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(InkPC), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ─── HiZPass + GpuCuller implementation ────────────────────────────────────────

// ── helpers ─────────────────────────────────────────────────────────────────



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
    m_descSets.clear();

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
    m_descSets.clear();
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

// ─── ShadowPass implementation ────────────────────────────────────────────────

// ── helpers ─────────────────────────────────────────────────────────────────



// ── init / destroy ──────────────────────────────────────────────────────────

void ShadowPass::init(const Device& device, VkDescriptorSetLayout mainLayout) {
    m_device = &device;
    createAtlasImage();
    createSampler();
    createPipelines(mainLayout);
    spdlog::info("ShadowPass initialized ({}×{}, {} cascades)",
                 SHADOW_MAP_SIZE * CASCADE_COUNT, SHADOW_MAP_SIZE, CASCADE_COUNT);
}

void ShadowPass::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_staticPipeline)  vkDestroyPipeline(dev, m_staticPipeline, nullptr);
    if (m_skinnedPipeline) vkDestroyPipeline(dev, m_skinnedPipeline, nullptr);
    if (m_pipelineLayout)  vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_sampler)         vkDestroySampler(dev, m_sampler, nullptr);
    if (m_atlasView)       vkDestroyImageView(dev, m_atlasView, nullptr);
    if (m_atlasImage) {
        vmaDestroyImage(m_device->getAllocator(), m_atlasImage, m_atlasAlloc);
    }
    m_device = nullptr;
}

// ── cascade computation ─────────────────────────────────────────────────────

void ShadowPass::updateCascades(const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec3& lightDir,
                                float nearClip, float farClip) {
    // Practical split scheme (Nvidia GPU Gems 3, Ch. 10)
    float splits[CASCADE_COUNT + 1];
    splits[0] = nearClip;
    for (uint32_t i = 1; i <= CASCADE_COUNT; ++i) {
        float p = static_cast<float>(i) / CASCADE_COUNT;
        float log  = nearClip * std::pow(farClip / nearClip, p);
        float uni  = nearClip + (farClip - nearClip) * p;
        splits[i]  = SPLIT_LAMBDA * log + (1.0f - SPLIT_LAMBDA) * uni;
    }

    glm::mat4 invView = glm::inverse(view);

    for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
        float cNear = splits[c];
        float cFar  = splits[c + 1];

        // Build sub-frustum projection (same aspect/fov, different near/far)
        // Extract fov and aspect from the projection matrix
        float tanHalfFovY = 1.0f / proj[1][1];
        float aspect      = proj[1][1] / proj[0][0];

        // Frustum corners in view space
        float xn = cNear * tanHalfFovY * aspect;
        float yn = cNear * tanHalfFovY;
        float xf = cFar  * tanHalfFovY * aspect;
        float yf = cFar  * tanHalfFovY;

        std::array<glm::vec4, 8> corners = {{
            {-xn, -yn, -cNear, 1.0f}, { xn, -yn, -cNear, 1.0f},
            { xn,  yn, -cNear, 1.0f}, {-xn,  yn, -cNear, 1.0f},
            {-xf, -yf, -cFar,  1.0f}, { xf, -yf, -cFar,  1.0f},
            { xf,  yf, -cFar,  1.0f}, {-xf,  yf, -cFar,  1.0f},
        }};

        // Transform to world space
        glm::vec3 center(0.0f);
        for (auto& corner : corners) {
            corner = invView * corner;
            center += glm::vec3(corner);
        }
        center /= 8.0f;

        // Light view matrix (looking from center along light direction)
        glm::vec3 lightDir3 = glm::normalize(lightDir);
        glm::mat4 lightView = glm::lookAt(center - lightDir3 * 50.0f,
                                           center,
                                           glm::vec3(0.0f, 1.0f, 0.0f));

        // Find AABB of frustum corners in light space
        glm::vec3 minBounds(std::numeric_limits<float>::max());
        glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
        for (const auto& corner : corners) {
            glm::vec3 ls = glm::vec3(lightView * corner);
            minBounds = glm::min(minBounds, ls);
            maxBounds = glm::max(maxBounds, ls);
        }

        // Expand Z range to include shadow casters behind the frustum
        float zExtra = 80.0f;
        minBounds.z -= zExtra;

        // Texel snapping: round min/max to shadow map texel grid
        float worldTexelSize = (maxBounds.x - minBounds.x) / SHADOW_MAP_SIZE;
        if (worldTexelSize > 0.0f) {
            minBounds.x = std::floor(minBounds.x / worldTexelSize) * worldTexelSize;
            maxBounds.x = std::ceil(maxBounds.x / worldTexelSize) * worldTexelSize;
            minBounds.y = std::floor(minBounds.y / worldTexelSize) * worldTexelSize;
            maxBounds.y = std::ceil(maxBounds.y / worldTexelSize) * worldTexelSize;
        }

        glm::mat4 lightProj = glm::ortho(minBounds.x, maxBounds.x,
                                          minBounds.y, maxBounds.y,
                                          minBounds.z, maxBounds.z);

        // Vulkan clip-space Y is inverted vs OpenGL
        lightProj[1][1] *= -1.0f;

        m_cascades[c].lightViewProj = lightProj * lightView;
        m_cascades[c].splitDepth    = cFar;
    }
}

// ── recording ───────────────────────────────────────────────────────────────

void ShadowPass::recordCommands(VkCommandBuffer cmd,
                                DrawFn staticDrawFn,
                                DrawFn skinnedDrawFn) {
    // Transition atlas from UNDEFINED to DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.image         = m_atlasImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    VkRenderingAttachmentInfo depthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttach.imageView   = m_atlasView;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea        = {{0, 0}, {SHADOW_MAP_SIZE * CASCADE_COUNT, SHADOW_MAP_SIZE}};
    renderInfo.layerCount        = 1;
    renderInfo.pDepthAttachment  = &depthAttach;

    vkCmdBeginRendering(cmd, &renderInfo);

    for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
        // Set viewport/scissor to this cascade's tile
        VkViewport vp{};
        vp.x        = static_cast<float>(c * SHADOW_MAP_SIZE);
        vp.y        = 0.0f;
        vp.width    = static_cast<float>(SHADOW_MAP_SIZE);
        vp.height   = static_cast<float>(SHADOW_MAP_SIZE);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D scissor{};
        scissor.offset = {static_cast<int32_t>(c * SHADOW_MAP_SIZE), 0};
        scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Static meshes
        if (staticDrawFn) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_staticPipeline);
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &m_cascades[c].lightViewProj);
            staticDrawFn(cmd, c);
        }

        // Skinned meshes
        if (skinnedDrawFn) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &m_cascades[c].lightViewProj);
            skinnedDrawFn(cmd, c);
        }
    }

    vkCmdEndRendering(cmd);

    // Transition atlas to SHADER_READ_ONLY_OPTIMAL for sampling in main pass
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &depInfo);
}

// ── parallel recording variant ──────────────────────────────────────────────

void ShadowPass::recordCommandsParallel(VkCommandBuffer cmd,
                                        SecondaryDrawFn staticDrawFn,
                                        SecondaryDrawFn skinnedDrawFn) {
    // Transition atlas from UNDEFINED to DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.image         = m_atlasImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    VkRenderingAttachmentInfo depthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttach.imageView   = m_atlasView;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.flags             = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    renderInfo.renderArea        = {{0, 0}, {SHADOW_MAP_SIZE * CASCADE_COUNT, SHADOW_MAP_SIZE}};
    renderInfo.layerCount        = 1;
    renderInfo.pDepthAttachment  = &depthAttach;

    vkCmdBeginRendering(cmd, &renderInfo);

    for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
        VkViewport vp{};
        vp.x        = static_cast<float>(c * SHADOW_MAP_SIZE);
        vp.y        = 0.0f;
        vp.width    = static_cast<float>(SHADOW_MAP_SIZE);
        vp.height   = static_cast<float>(SHADOW_MAP_SIZE);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = {static_cast<int32_t>(c * SHADOW_MAP_SIZE), 0};
        scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};

        if (staticDrawFn) {
            auto cbs = staticDrawFn(c, m_cascades[c].lightViewProj, vp, scissor);
            if (!cbs.empty())
                vkCmdExecuteCommands(cmd, static_cast<uint32_t>(cbs.size()), cbs.data());
        }
        if (skinnedDrawFn) {
            auto cbs = skinnedDrawFn(c, m_cascades[c].lightViewProj, vp, scissor);
            if (!cbs.empty())
                vkCmdExecuteCommands(cmd, static_cast<uint32_t>(cbs.size()), cbs.data());
        }
    }

    vkCmdEndRendering(cmd);

    // Transition atlas to SHADER_READ_ONLY_OPTIMAL for sampling in main pass
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &depInfo);
}

void ShadowPass::bindToDescriptors(Descriptors& descriptors) {
    descriptors.updateShadowMap(m_atlasView, m_sampler);
}

// ── private: Vulkan resource creation ───────────────────────────────────────

void ShadowPass::createAtlasImage() {
    VkImageCreateInfo imgCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = DEPTH_FORMAT;
    imgCI.extent        = {SHADOW_MAP_SIZE * CASCADE_COUNT, SHADOW_MAP_SIZE, 1};
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VK_CHECK(vmaCreateImage(m_device->getAllocator(), &imgCI, &allocCI,
                            &m_atlasImage, &m_atlasAlloc, nullptr),
             "Failed to create shadow atlas image");

    VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.image    = m_atlasImage;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = DEPTH_FORMAT;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    VK_CHECK(vkCreateImageView(m_device->getDevice(), &viewCI, nullptr, &m_atlasView),
             "Failed to create shadow atlas image view");
}

void ShadowPass::createSampler() {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // no shadow outside atlas
    ci.compareEnable = VK_TRUE;
    ci.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL; // hardware PCF
    ci.maxAnisotropy = 1.0f;

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_sampler),
             "Failed to create shadow sampler");
}


void ShadowPass::createPipelines(VkDescriptorSetLayout mainLayout) {
    VkDevice dev = m_device->getDevice();
    std::string shaderDir = SHADER_DIR;

    // Push constant: mat4 lightViewProj (64 bytes) + uint boneBaseIndex (4 bytes, skinned only)
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(glm::mat4) + sizeof(uint32_t); // 68 bytes

    VkPipelineLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &mainLayout; // reuse main layout for bone SSBO access
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pushRange;

    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout),
             "Failed to create shadow pipeline layout");

    // ── Common pipeline state ──────────────────────────────────────────────

    VkPipelineInputAssemblyStateCreateInfo iaState{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor are dynamic
    VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount = 1;
    vpState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsState{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsState.depthClampEnable        = VK_FALSE;
    rsState.rasterizerDiscardEnable = VK_FALSE;
    rsState.polygonMode             = VK_POLYGON_MODE_FILL;
    rsState.lineWidth               = 1.0f;
    rsState.cullMode                = VK_CULL_MODE_FRONT_BIT; // front-face culling for shadow bias
    rsState.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.depthBiasEnable         = VK_TRUE;
    rsState.depthBiasConstantFactor = 1.5f;
    rsState.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo msState{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsState.depthTestEnable  = VK_TRUE;
    dsState.depthWriteEnable = VK_TRUE;
    dsState.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    // No color attachments — depth-only
    VkPipelineColorBlendStateCreateInfo cbState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbState.attachmentCount = 0;

    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynState.pDynamicStates    = dynStates.data();

    // ── Static mesh shadow pipeline ────────────────────────────────────────

    {
        auto vertCode = readFile(shaderDir + "/shadow.vert.spv");
        VkShaderModule vertMod = createShaderModule(dev, vertCode);

        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vertMod;
        vertStage.pName  = "main";

        // Vertex input: binding 0 = Vertex, binding 1 = InstanceData
        auto vertexBinding  = Vertex::getBindingDescription();
        auto vertexAttribs  = Vertex::getAttributeDescriptions();
        auto instanceBinding = InstanceData::getBindingDescription();
        auto instanceAttribs = InstanceData::getAttributeDescriptions();

        std::vector<VkVertexInputBindingDescription> bindings = {vertexBinding, instanceBinding};
        std::vector<VkVertexInputAttributeDescription> attribs;
        for (auto& a : vertexAttribs) attribs.push_back(a);
        for (auto& a : instanceAttribs) attribs.push_back(a);

        VkPipelineVertexInputStateCreateInfo viState{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        viState.vertexBindingDescriptionCount   = static_cast<uint32_t>(bindings.size());
        viState.pVertexBindingDescriptions      = bindings.data();
        viState.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
        viState.pVertexAttributeDescriptions    = attribs.data();

        RenderFormats shadowFmts = RenderFormats::depthOnly(DEPTH_FORMAT);
        VkPipelineRenderingCreateInfo dynCI = shadowFmts.pipelineRenderingCI();

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext               = &dynCI;
        ci.stageCount          = 1; // vertex only
        ci.pStages             = &vertStage;
        ci.pVertexInputState   = &viState;
        ci.pInputAssemblyState = &iaState;
        ci.pViewportState      = &vpState;
        ci.pRasterizationState = &rsState;
        ci.pMultisampleState   = &msState;
        ci.pDepthStencilState  = &dsState;
        ci.pColorBlendState    = &cbState;
        ci.pDynamicState       = &dynState;
        ci.layout              = m_pipelineLayout;
        ci.renderPass          = VK_NULL_HANDLE;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &m_staticPipeline),
                 "Failed to create static shadow pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
    }

    // ── Skinned mesh shadow pipeline ───────────────────────────────────────

    {
        auto vertCode = readFile(shaderDir + "/shadow_skinned.vert.spv");
        VkShaderModule vertMod = createShaderModule(dev, vertCode);

        VkPipelineShaderStageCreateInfo vertStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vertMod;
        vertStage.pName  = "main";

        auto vertexBinding   = SkinnedVertex::getBindingDescription();
        auto vertexAttribs   = SkinnedVertex::getAttributeDescriptions();
        auto instanceBinding = InstanceData::getBindingDescription();
        auto instanceAttribs = InstanceData::getAttributeDescriptions();
        // Shift instance locations +2 so they don't collide with SkinnedVertex
        // joints (loc 4) and weights (loc 5). Matches main skinned pipeline.
        for (auto& a : instanceAttribs) { a.location += 2; }

        std::vector<VkVertexInputBindingDescription> bindings = {vertexBinding, instanceBinding};
        std::vector<VkVertexInputAttributeDescription> attribs;
        for (auto& a : vertexAttribs) attribs.push_back(a);
        for (auto& a : instanceAttribs) attribs.push_back(a);

        VkPipelineVertexInputStateCreateInfo viState{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        viState.vertexBindingDescriptionCount   = static_cast<uint32_t>(bindings.size());
        viState.pVertexBindingDescriptions      = bindings.data();
        viState.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
        viState.pVertexAttributeDescriptions    = attribs.data();

        RenderFormats skinnedShadowFmts = RenderFormats::depthOnly(DEPTH_FORMAT);
        VkPipelineRenderingCreateInfo skinnedDynCI = skinnedShadowFmts.pipelineRenderingCI();

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext               = &skinnedDynCI;
        ci.stageCount          = 1;
        ci.pStages             = &vertStage;
        ci.pVertexInputState   = &viState;
        ci.pInputAssemblyState = &iaState;
        ci.pViewportState      = &vpState;
        ci.pRasterizationState = &rsState;
        ci.pMultisampleState   = &msState;
        ci.pDepthStencilState  = &dsState;
        ci.pColorBlendState    = &cbState;
        ci.pDynamicState       = &dynState;
        ci.layout              = m_pipelineLayout;
        ci.renderPass          = VK_NULL_HANDLE;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &m_skinnedPipeline),
                 "Failed to create skinned shadow pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
    }
}

} // namespace glory
