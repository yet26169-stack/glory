#include "renderer/ShadowPass.h"
#include "renderer/Descriptors.h"
#include "renderer/VkCheck.h"
#include "renderer/Buffer.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

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

// ── init / destroy ──────────────────────────────────────────────────────────

void ShadowPass::init(const Device& device, VkDescriptorSetLayout mainLayout) {
    m_device = &device;
    createAtlasImage();
    createSampler();
    createRenderPass();
    createFramebuffer();
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
    if (m_framebuffer)     vkDestroyFramebuffer(dev, m_framebuffer, nullptr);
    if (m_renderPass)      vkDestroyRenderPass(dev, m_renderPass, nullptr);
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
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass  = m_renderPass;
    rpBegin.framebuffer = m_framebuffer;
    rpBegin.renderArea  = {{0, 0}, {SHADOW_MAP_SIZE * CASCADE_COUNT, SHADOW_MAP_SIZE}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues    = &clear;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

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

    vkCmdEndRenderPass(cmd);
}

// ── descriptor binding ──────────────────────────────────────────────────────

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

void ShadowPass::createRenderPass() {
    VkAttachmentDescription depthAttach{};
    depthAttach.format         = DEPTH_FORMAT;
    depthAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    // Dependency: external → depth write, then depth write → fragment read
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpCI.attachmentCount = 1;
    rpCI.pAttachments    = &depthAttach;
    rpCI.subpassCount    = 1;
    rpCI.pSubpasses      = &subpass;
    rpCI.dependencyCount = static_cast<uint32_t>(deps.size());
    rpCI.pDependencies   = deps.data();

    VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &rpCI, nullptr, &m_renderPass),
             "Failed to create shadow render pass");
}

void ShadowPass::createFramebuffer() {
    VkFramebufferCreateInfo fbCI{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbCI.renderPass      = m_renderPass;
    fbCI.attachmentCount = 1;
    fbCI.pAttachments    = &m_atlasView;
    fbCI.width           = SHADOW_MAP_SIZE * CASCADE_COUNT;
    fbCI.height          = SHADOW_MAP_SIZE;
    fbCI.layers          = 1;

    VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &fbCI, nullptr, &m_framebuffer),
             "Failed to create shadow framebuffer");
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

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
        ci.renderPass          = m_renderPass;
        ci.subpass             = 0;

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

        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
        ci.renderPass          = m_renderPass;
        ci.subpass             = 0;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &m_skinnedPipeline),
                 "Failed to create skinned shadow pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
    }
}

} // namespace glory
