#include "renderer/Pipeline.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Swapchain.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <stdexcept>

namespace glory {

Pipeline::Pipeline(const Device& device, const Swapchain& swapchain,
                   VkDescriptorSetLayout descriptorSetLayout,
                   VkRenderPass externalRenderPass)
    : m_device(device)
{
    m_depthFormat = m_device.findDepthFormat();
    if (externalRenderPass != VK_NULL_HANDLE) {
        m_renderPass = externalRenderPass;
        m_ownsRenderPass = false;
    } else {
        createRenderPass(swapchain.getImageFormat());
        m_ownsRenderPass = true;
    }
    createGraphicsPipeline(swapchain.getExtent(), descriptorSetLayout);
    if (m_ownsRenderPass) {
        createDepthResources(swapchain);
        createFramebuffers(swapchain);
    }
}

Pipeline::~Pipeline() { cleanup(); }

void Pipeline::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    destroyFramebuffers();
    m_depthImage = Image{};

    if (m_graphicsPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_device.getDevice(), m_graphicsPipeline, nullptr);
    if (m_wireframePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_device.getDevice(), m_wireframePipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_device.getDevice(), m_pipelineLayout, nullptr);
    if (m_ownsRenderPass && m_renderPass != VK_NULL_HANDLE)
        vkDestroyRenderPass(m_device.getDevice(), m_renderPass, nullptr);

    spdlog::info("Pipeline resources destroyed");
}

void Pipeline::recreateFramebuffers(const Swapchain& swapchain) {
    if (!m_ownsRenderPass) return; // framebuffers managed externally
    destroyFramebuffers();
    m_depthImage = Image{};
    createDepthResources(swapchain);
    createFramebuffers(swapchain);
}

// ── Render pass ─────────────────────────────────────────────────────────────
void Pipeline::createRenderPass(VkFormat imageFormat) {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = imageFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format         = m_depthFormat;
    depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<uint32_t>(attachments.size());
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dependency;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_renderPass),
             "Failed to create render pass");
    spdlog::info("Render pass created (with depth)");
}

// ── Graphics pipeline ───────────────────────────────────────────────────────
void Pipeline::createGraphicsPipeline(VkExtent2D /*extent*/,
                                      VkDescriptorSetLayout descriptorSetLayout) {
    auto vertCode = readFile(std::string(SHADER_DIR) + "triangle.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "triangle.frag.spv");

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName  = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName  = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    // Vertex input — binding 0 (per-vertex) + binding 1 (per-instance)
    std::array<VkVertexInputBindingDescription, 2> bindingDescs = {
        Vertex::getBindingDescription(),
        InstanceData::getBindingDescription()
    };
    auto vertexAttrs   = Vertex::getAttributeDescriptions();
    auto instanceAttrs = InstanceData::getAttributeDescriptions();
    std::array<VkVertexInputAttributeDescription, 15> allAttrs{};
    std::copy(vertexAttrs.begin(), vertexAttrs.end(), allAttrs.begin());
    std::copy(instanceAttrs.begin(), instanceAttrs.end(), allAttrs.begin() + vertexAttrs.size());

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = static_cast<uint32_t>(bindingDescs.size());
    vertexInput.pVertexBindingDescriptions      = bindingDescs.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(allAttrs.size());
    vertexInput.pVertexAttributeDescriptions    = allAttrs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable        = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode  = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth    = 1.0f;
    rasterizer.cullMode     = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable  = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable       = VK_TRUE;
    depthStencil.depthWriteEnable      = VK_TRUE;
    depthStencil.depthCompareOp        = VK_COMPARE_OP_GREATER;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState noWriteBlend{};  // charDepth: no write for static geometry
    VkPipelineColorBlendAttachmentState staticBlends[2] = {blendAttachment, noWriteBlend};

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable   = VK_FALSE;
    colorBlend.attachmentCount = 2;
    colorBlend.pAttachments    = staticBlends;

    // No push constants — per-entity data is in the instance buffer (binding 1)
    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &descriptorSetLayout;
    layoutCI.pushConstantRangeCount = 0;
    layoutCI.pPushConstantRanges    = nullptr;

    VK_CHECK(vkCreatePipelineLayout(m_device.getDevice(), &layoutCI, nullptr, &m_pipelineLayout),
             "Failed to create pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &multisampling;
    pipelineCI.pDepthStencilState  = &depthStencil;
    pipelineCI.pColorBlendState    = &colorBlend;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_pipelineLayout;
    pipelineCI.renderPass          = m_renderPass;
    pipelineCI.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(m_device.getDevice(), VK_NULL_HANDLE,
                                       1, &pipelineCI, nullptr, &m_graphicsPipeline),
             "Failed to create graphics pipeline");

    // Wireframe variant — same setup but VK_POLYGON_MODE_LINE, no culling
    rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    VK_CHECK(vkCreateGraphicsPipelines(m_device.getDevice(), VK_NULL_HANDLE,
                                       1, &pipelineCI, nullptr, &m_wireframePipeline),
             "Failed to create wireframe pipeline");
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;

    // Shader modules not needed after pipeline creation
    vkDestroyShaderModule(m_device.getDevice(), fragModule, nullptr);
    vkDestroyShaderModule(m_device.getDevice(), vertModule, nullptr);

    spdlog::info("Graphics pipeline created");
}

// ── Depth resources ─────────────────────────────────────────────────────────
void Pipeline::createDepthResources(const Swapchain& swapchain) {
    m_depthImage = Image(
        m_device,
        swapchain.getExtent().width,
        swapchain.getExtent().height,
        m_depthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    spdlog::trace("Depth image created (format {})", static_cast<int>(m_depthFormat));
}

// ── Framebuffers ────────────────────────────────────────────────────────────
void Pipeline::createFramebuffers(const Swapchain& swapchain) {
    const auto& views = swapchain.getImageViews();
    m_framebuffers.resize(views.size());

    for (size_t i = 0; i < views.size(); ++i) {
        std::array<VkImageView, 2> attachments = {
            views[i], m_depthImage.getImageView()
        };

        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = m_renderPass;
        ci.attachmentCount = static_cast<uint32_t>(attachments.size());
        ci.pAttachments    = attachments.data();
        ci.width           = swapchain.getExtent().width;
        ci.height          = swapchain.getExtent().height;
        ci.layers          = 1;

        VK_CHECK(vkCreateFramebuffer(m_device.getDevice(), &ci, nullptr, &m_framebuffers[i]),
                 "Failed to create framebuffer");
    }
    spdlog::trace("{} framebuffers created", m_framebuffers.size());
}

void Pipeline::destroyFramebuffers() {
    for (auto fb : m_framebuffers)
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(m_device.getDevice(), fb, nullptr);
    m_framebuffers.clear();
}

// ── Shader helpers ──────────────────────────────────────────────────────────
VkShaderModule Pipeline::createShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}

std::vector<char> Pipeline::readFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open file: " + filepath);

    auto fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

} // namespace glory
