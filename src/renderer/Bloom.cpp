#include "renderer/Bloom.h"
#include "renderer/Device.h"
#include "renderer/Swapchain.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>

namespace glory {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open shader file: " + path);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}
} // anonymous namespace

Bloom::Bloom(const Device& device, const Swapchain& swapchain, VkImageView hdrImageView)
    : m_device(device)
{
    createSampler();
    createImages(swapchain);
    createRenderPass();
    createFramebuffers();
    createDescriptors();
    updateDescriptors(hdrImageView);
    createPipelines();
    spdlog::info("Bloom pipeline created ({}x{})", m_width, m_height);
}

Bloom::~Bloom() { cleanup(); }

void Bloom::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();

    destroySwapchainResources();

    if (m_extractPipeline) vkDestroyPipeline(dev, m_extractPipeline, nullptr);
    if (m_extractLayout)   vkDestroyPipelineLayout(dev, m_extractLayout, nullptr);
    if (m_blurPipeline)    vkDestroyPipeline(dev, m_blurPipeline, nullptr);
    if (m_blurLayout)      vkDestroyPipelineLayout(dev, m_blurLayout, nullptr);
    if (m_renderPass)      vkDestroyRenderPass(dev, m_renderPass, nullptr);
    if (m_descPool)        vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout)      vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_sampler)         vkDestroySampler(dev, m_sampler, nullptr);

    spdlog::info("Bloom destroyed");
}

void Bloom::recreate(const Swapchain& swapchain, VkImageView hdrImageView) {
    destroySwapchainResources();
    createImages(swapchain);
    createFramebuffers();
    updateDescriptors(hdrImageView);
}

void Bloom::destroySwapchainResources() {
    VkDevice dev = m_device.getDevice();
    if (m_fbA) vkDestroyFramebuffer(dev, m_fbA, nullptr);
    if (m_fbB) vkDestroyFramebuffer(dev, m_fbB, nullptr);
    m_fbA = VK_NULL_HANDLE;
    m_fbB = VK_NULL_HANDLE;
    m_imageA = Image{};
    m_imageB = Image{};
}

void Bloom::createImages(const Swapchain& swapchain) {
    auto ext = swapchain.getExtent();
    m_width  = std::max(1u, ext.width / 2);
    m_height = std::max(1u, ext.height / 2);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_imageA = Image(m_device, m_width, m_height,
                     VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT);
    m_imageB = Image(m_device, m_width, m_height,
                     VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT);
}

void Bloom::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    // Wait for previous pass's color output before we read/write
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments    = &colorAttachment;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_renderPass),
             "Failed to create bloom render pass");
}

void Bloom::createFramebuffers() {
    VkDevice dev = m_device.getDevice();

    auto makeFB = [&](VkImageView view) {
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = m_renderPass;
        ci.attachmentCount = 1;
        ci.pAttachments    = &view;
        ci.width           = m_width;
        ci.height          = m_height;
        ci.layers          = 1;
        VkFramebuffer fb;
        VK_CHECK(vkCreateFramebuffer(dev, &ci, nullptr, &fb),
                 "Failed to create bloom framebuffer");
        return fb;
    };

    m_fbA = makeFB(m_imageA.getImageView());
    m_fbB = makeFB(m_imageB.getImageView());
}

void Bloom::createSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter     = VK_FILTER_LINEAR;
    ci.minFilter     = VK_FILTER_LINEAR;
    ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VK_CHECK(vkCreateSampler(m_device.getDevice(), &ci, nullptr, &m_sampler),
             "Failed to create bloom sampler");
}

void Bloom::createDescriptors() {
    VkDevice dev = m_device.getDevice();

    // Layout: 1 combined image sampler at binding 0
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;

    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "Failed to create bloom desc layout");

    // Pool: 3 sets × 1 sampler each
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    poolCI.maxSets       = 3;

    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "Failed to create bloom desc pool");

    // Allocate 3 descriptor sets
    VkDescriptorSetLayout layouts[3] = {m_descLayout, m_descLayout, m_descLayout};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts        = layouts;

    VkDescriptorSet sets[3];
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, sets),
             "Failed to allocate bloom descriptor sets");
    m_descHDR = sets[0];
    m_descA   = sets[1];
    m_descB   = sets[2];
}

void Bloom::updateDescriptors(VkImageView hdrImageView) {
    auto writeSet = [&](VkDescriptorSet set, VkImageView view) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = m_sampler;
        imgInfo.imageView   = view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imgInfo;
        return write;
    };

    std::array<VkWriteDescriptorSet, 3> writes;
    VkDescriptorImageInfo imgInfos[3];

    for (int i = 0; i < 3; ++i) {
        imgInfos[i].sampler     = m_sampler;
        imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    imgInfos[0].imageView = hdrImageView;
    imgInfos[1].imageView = m_imageA.getImageView();
    imgInfos[2].imageView = m_imageB.getImageView();

    VkDescriptorSet sets[3] = {m_descHDR, m_descA, m_descB};
    for (int i = 0; i < 3; ++i) {
        writes[i] = {};
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = sets[i];
        writes[i].dstBinding      = 0;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imgInfos[i];
    }

    vkUpdateDescriptorSets(m_device.getDevice(), 3, writes.data(), 0, nullptr);
}

void Bloom::createPipelines() {
    VkDevice dev = m_device.getDevice();

    auto vertCode        = readFile(std::string(SHADER_DIR) + "postprocess.vert.spv");
    auto extractFragCode = readFile(std::string(SHADER_DIR) + "bloom_extract.frag.spv");
    auto blurFragCode    = readFile(std::string(SHADER_DIR) + "bloom_blur.frag.spv");

    VkShaderModule vertMod        = createShaderModule(dev, vertCode);
    VkShaderModule extractFragMod = createShaderModule(dev, extractFragCode);
    VkShaderModule blurFragMod    = createShaderModule(dev, blurFragCode);

    // Shared pipeline state
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    // --- Extract pipeline ---
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset     = 0;
        pushRange.size       = sizeof(float); // threshold

        VkPipelineLayoutCreateInfo layoutCI{};
        layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutCI.setLayoutCount         = 1;
        layoutCI.pSetLayouts            = &m_descLayout;
        layoutCI.pushConstantRangeCount = 1;
        layoutCI.pPushConstantRanges    = &pushRange;

        VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_extractLayout),
                 "Failed to create bloom extract pipeline layout");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = extractFragMod;
        stages[1].pName  = "main";

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vertexInput;
        ci.pInputAssemblyState = &inputAssembly;
        ci.pViewportState      = &viewportState;
        ci.pRasterizationState = &rasterizer;
        ci.pMultisampleState   = &multisample;
        ci.pColorBlendState    = &colorBlend;
        ci.pDynamicState       = &dynState;
        ci.layout              = m_extractLayout;
        ci.renderPass          = m_renderPass;
        ci.subpass             = 0;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                           &m_extractPipeline),
                 "Failed to create bloom extract pipeline");
    }

    // --- Blur pipeline ---
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset     = 0;
        pushRange.size       = 2 * sizeof(float); // dirX, dirY

        VkPipelineLayoutCreateInfo layoutCI{};
        layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutCI.setLayoutCount         = 1;
        layoutCI.pSetLayouts            = &m_descLayout;
        layoutCI.pushConstantRangeCount = 1;
        layoutCI.pPushConstantRanges    = &pushRange;

        VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_blurLayout),
                 "Failed to create bloom blur pipeline layout");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = blurFragMod;
        stages[1].pName  = "main";

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vertexInput;
        ci.pInputAssemblyState = &inputAssembly;
        ci.pViewportState      = &viewportState;
        ci.pRasterizationState = &rasterizer;
        ci.pMultisampleState   = &multisample;
        ci.pColorBlendState    = &colorBlend;
        ci.pDynamicState       = &dynState;
        ci.layout              = m_blurLayout;
        ci.renderPass          = m_renderPass;
        ci.subpass             = 0;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr,
                                           &m_blurPipeline),
                 "Failed to create bloom blur pipeline");
    }

    vkDestroyShaderModule(dev, blurFragMod, nullptr);
    vkDestroyShaderModule(dev, extractFragMod, nullptr);
    vkDestroyShaderModule(dev, vertMod, nullptr);
}

void Bloom::record(VkCommandBuffer cmd, float threshold) {
    VkViewport vp{0.f, 0.f, static_cast<float>(m_width), static_cast<float>(m_height), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};

    auto beginPass = [&](VkFramebuffer fb) {
        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 1.f}};

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_renderPass;
        rp.framebuffer       = fb;
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = {m_width, m_height};
        rp.clearValueCount   = 1;
        rp.pClearValues      = &clear;

        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    };

    // Pass 1: Extract bright pixels from HDR → imageA
    {
        beginPass(m_fbA);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_extractPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_extractLayout, 0, 1, &m_descHDR, 0, nullptr);
        vkCmdPushConstants(cmd, m_extractLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(float), &threshold);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // Pass 2: Horizontal blur imageA → imageB
    {
        beginPass(m_fbB);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_blurLayout, 0, 1, &m_descA, 0, nullptr);
        float dir[2] = {1.0f, 0.0f};
        vkCmdPushConstants(cmd, m_blurLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 2 * sizeof(float), dir);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // Pass 3: Vertical blur imageB → imageA (final result)
    {
        beginPass(m_fbA);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_blurLayout, 0, 1, &m_descB, 0, nullptr);
        float dir[2] = {0.0f, 1.0f};
        vkCmdPushConstants(cmd, m_blurLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 2 * sizeof(float), dir);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

} // namespace glory
