#include "renderer/GBuffer.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Swapchain.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <vector>

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

GBuffer::GBuffer(const Device& device, const Swapchain& swapchain,
                 VkDescriptorSetLayout geometryLayout)
    : m_device(device), m_geometryLayout(geometryLayout)
{
    createGBufferSampler();
    createGBufferImages(swapchain);
    createGeometryPass(swapchain);
    createGeometryFramebuffer(swapchain);
    createGeometryPipeline(swapchain);
    createLightingPass(swapchain);
    createLightingFramebuffers(swapchain);
    createLightingDescriptors();
    updateLightingDescriptors();
    createLightingPipeline(swapchain);
    spdlog::info("G-Buffer deferred renderer created");
}

GBuffer::~GBuffer() { cleanup(); }

void GBuffer::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();

    destroySwapchainResources();

    if (m_geometryPipeline)       vkDestroyPipeline(dev, m_geometryPipeline, nullptr);
    if (m_geometryPipelineLayout) vkDestroyPipelineLayout(dev, m_geometryPipelineLayout, nullptr);
    if (m_geometryPass)           vkDestroyRenderPass(dev, m_geometryPass, nullptr);

    if (m_lightingPipeline)       vkDestroyPipeline(dev, m_lightingPipeline, nullptr);
    if (m_lightingPipelineLayout) vkDestroyPipelineLayout(dev, m_lightingPipelineLayout, nullptr);
    if (m_lightingPass)           vkDestroyRenderPass(dev, m_lightingPass, nullptr);

    if (m_lightingDescPool)   vkDestroyDescriptorPool(dev, m_lightingDescPool, nullptr);
    if (m_lightingDescLayout) vkDestroyDescriptorSetLayout(dev, m_lightingDescLayout, nullptr);
    if (m_gbufferSampler)     vkDestroySampler(dev, m_gbufferSampler, nullptr);

    spdlog::info("G-Buffer destroyed");
}

void GBuffer::recreate(const Swapchain& swapchain) {
    VkDevice dev = m_device.getDevice();
    destroySwapchainResources();

    createGBufferImages(swapchain);
    createGeometryFramebuffer(swapchain);
    createLightingFramebuffers(swapchain);
    updateLightingDescriptors();
    spdlog::info("G-Buffer recreated");
}

void GBuffer::destroySwapchainResources() {
    VkDevice dev = m_device.getDevice();
    if (m_geometryFB) vkDestroyFramebuffer(dev, m_geometryFB, nullptr);
    m_geometryFB = VK_NULL_HANDLE;
    for (auto fb : m_lightingFBs) vkDestroyFramebuffer(dev, fb, nullptr);
    m_lightingFBs.clear();

    m_albedoImage   = Image{};
    m_normalImage   = Image{};
    m_positionImage = Image{};
    m_depthImage    = Image{};
}

// ── G-Buffer Images ─────────────────────────────────────────────────────────
void GBuffer::createGBufferImages(const Swapchain& swapchain) {
    auto ext = swapchain.getExtent();
    m_albedoImage   = Image(m_device, ext.width, ext.height,
                            VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
    m_normalImage   = Image(m_device, ext.width, ext.height,
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
    m_positionImage = Image(m_device, ext.width, ext.height,
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
    m_depthImage    = Image(m_device, ext.width, ext.height,
                            m_device.findDepthFormat(),
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
}

// ── Geometry Pass (G-buffer fill) ───────────────────────────────────────────
void GBuffer::createGeometryPass(const Swapchain&) {
    std::array<VkAttachmentDescription, 4> attachments{};

    // Albedo
    attachments[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Normal
    attachments[1] = attachments[0];
    attachments[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Position
    attachments[2] = attachments[0];
    attachments[2].format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Depth
    attachments[3].format         = m_device.findDepthFormat();
    attachments[3].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRefs[] = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference depthRef = {3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 3;
    subpass.pColorAttachments       = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<uint32_t>(attachments.size());
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_geometryPass),
             "Failed to create geometry render pass");
}

void GBuffer::createGeometryFramebuffer(const Swapchain& swapchain) {
    auto ext = swapchain.getExtent();
    VkImageView views[] = {
        m_albedoImage.getImageView(),
        m_normalImage.getImageView(),
        m_positionImage.getImageView(),
        m_depthImage.getImageView(),
    };

    VkFramebufferCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass      = m_geometryPass;
    ci.attachmentCount = 4;
    ci.pAttachments    = views;
    ci.width           = ext.width;
    ci.height          = ext.height;
    ci.layers          = 1;

    VK_CHECK(vkCreateFramebuffer(m_device.getDevice(), &ci, nullptr, &m_geometryFB),
             "Failed to create geometry framebuffer");
}

void GBuffer::createGeometryPipeline(const Swapchain& swapchain) {
    VkDevice dev = m_device.getDevice();

    auto vertCode = readFile(std::string(SHADER_DIR) + "gbuffer.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "gbuffer.frag.spv");
    VkShaderModule vertModule = createShaderModule(dev, vertCode);
    VkShaderModule fragModule = createShaderModule(dev, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    auto bindingDesc = Vertex::getBindingDescription();
    auto attrDescs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInput.pVertexAttributeDescriptions    = attrDescs.data();

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
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    // 3 color blend attachments (MRT) — no blending
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachments = {
        blendAttachment, blendAttachment, blendAttachment};

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 3;
    colorBlend.pAttachments    = blendAttachments.data();

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts    = &m_geometryLayout;

    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_geometryPipelineLayout),
             "Failed to create geometry pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &multisample;
    pipelineCI.pDepthStencilState  = &depthStencil;
    pipelineCI.pColorBlendState    = &colorBlend;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_geometryPipelineLayout;
    pipelineCI.renderPass          = m_geometryPass;
    pipelineCI.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI, nullptr,
                                       &m_geometryPipeline),
             "Failed to create geometry pipeline");

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);
    spdlog::info("G-Buffer geometry pipeline created");
}

// ── Lighting Pass ───────────────────────────────────────────────────────────
void GBuffer::createLightingPass(const Swapchain& swapchain) {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = swapchain.getImageFormat();
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments    = &colorAttachment;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_lightingPass),
             "Failed to create lighting render pass");
}

void GBuffer::createLightingFramebuffers(const Swapchain& swapchain) {
    auto& imageViews = swapchain.getImageViews();
    auto ext = swapchain.getExtent();

    m_lightingFBs.resize(imageViews.size());
    for (size_t i = 0; i < imageViews.size(); ++i) {
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = m_lightingPass;
        ci.attachmentCount = 1;
        ci.pAttachments    = &imageViews[i];
        ci.width           = ext.width;
        ci.height          = ext.height;
        ci.layers          = 1;

        VK_CHECK(vkCreateFramebuffer(m_device.getDevice(), &ci, nullptr, &m_lightingFBs[i]),
                 "Failed to create lighting framebuffer");
    }
}

void GBuffer::createGBufferSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_NEAREST;
    ci.minFilter = VK_FILTER_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VK_CHECK(vkCreateSampler(m_device.getDevice(), &ci, nullptr, &m_gbufferSampler),
             "Failed to create g-buffer sampler");
}

void GBuffer::createLightingDescriptors() {
    VkDevice dev = m_device.getDevice();

    // Layout: 3 samplers (albedo, normal, position) + 1 UBO (light)
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_lightingDescLayout),
             "Failed to create lighting desc layout");

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    poolCI.maxSets       = 1;

    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_lightingDescPool),
             "Failed to create lighting desc pool");

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_lightingDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_lightingDescLayout;

    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, &m_lightingDescSet),
             "Failed to allocate lighting descriptor set");
}

void GBuffer::updateLightingDescriptors() {
    std::array<VkDescriptorImageInfo, 3> imageInfos{};
    VkImageView views[] = {
        m_albedoImage.getImageView(),
        m_normalImage.getImageView(),
        m_positionImage.getImageView(),
    };

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        imageInfos[i].sampler     = m_gbufferSampler;
        imageInfos[i].imageView   = views[i];
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_lightingDescSet;
        writes[i].dstBinding      = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imageInfos[i];
    }

    vkUpdateDescriptorSets(m_device.getDevice(), static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void GBuffer::createLightingPipeline(const Swapchain& swapchain) {
    VkDevice dev = m_device.getDevice();

    auto vertCode = readFile(std::string(SHADER_DIR) + "deferred.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "deferred.frag.spv");
    VkShaderModule vertModule = createShaderModule(dev, vertCode);
    VkShaderModule fragModule = createShaderModule(dev, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    // No vertex input — fullscreen triangle
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

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts    = &m_lightingDescLayout;

    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_lightingPipelineLayout),
             "Failed to create lighting pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &multisample;
    pipelineCI.pColorBlendState    = &colorBlend;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_lightingPipelineLayout;
    pipelineCI.renderPass          = m_lightingPass;
    pipelineCI.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI, nullptr,
                                       &m_lightingPipeline),
             "Failed to create lighting pipeline");

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);
    spdlog::info("Deferred lighting pipeline created");
}

} // namespace glory
