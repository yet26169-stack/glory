#include "renderer/ClickIndicatorRenderer.h"
#include "renderer/Device.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include "renderer/ShieldBubbleRenderer.h"
#include "renderer/Buffer.h"
#include <glm/gtc/constants.hpp>
#include <vector>
#include <cmath>
#include <cstring>
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include <algorithm>
#include "renderer/SpriteEffectRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "renderer/Model.h"
#include "renderer/VkCheck.h"
#include "renderer/DistortionRenderer.h"
#include "renderer/OutlineRenderer.h"
#include "renderer/RenderFormats.h"
#include "renderer/StaticSkinnedMesh.h"
#include <stdexcept>
#include <array>
#include "renderer/WaterRenderer.h"
#include <glm/glm.hpp>

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

static VkShaderModule makeShaderModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &mod);
    return mod;
}

// ─── ClickIndicatorRenderer ─────────────────────────────────────────────────

struct IndicatorVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

ClickIndicatorRenderer::ClickIndicatorRenderer(const Device& device, const RenderFormats& formats)
    : m_device(device) {
    try {
        m_texture = std::make_unique<Texture>(device, std::string(ASSET_DIR) + "textures/click_indicator_atlas.png");
    } catch (const std::exception& e) {
        spdlog::warn("Click indicator atlas not found: {} — using checkerboard fallback", e.what());
        m_texture = std::make_unique<Texture>(Texture::createCheckerboard(device, 4, 8));
    }
    
    createDescriptorSet();
    createPipeline(formats);
    createVertexBuffer();
    
    spdlog::info("ClickIndicatorRenderer initialized");
}

ClickIndicatorRenderer::~ClickIndicatorRenderer() {
    VkDevice dev = m_device.getDevice();
    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
}

void ClickIndicatorRenderer::createDescriptorSet() {
    VkDevice dev = m_device.getDevice();
    
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout);
    
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);
    
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    vkAllocateDescriptorSets(dev, &allocInfo, &m_descSet);
    
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_texture->getImageView();
    imageInfo.sampler = m_texture->getSampler();
    
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

void ClickIndicatorRenderer::createPipeline(const RenderFormats& formats) {
    VkDevice dev = m_device.getDevice();
    
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(PushConstants);
    
    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout);
    
    
    auto vcode = readFile(std::string(SHADER_DIR) + "click_indicator.vert.spv");
    auto fcode = readFile(std::string(SHADER_DIR) + "click_indicator.frag.spv");
    
    VkShaderModuleCreateInfo smCI{};
    smCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = vcode.size();
    smCI.pCode = (const uint32_t*)vcode.data();
    VkShaderModule vmod;
    vkCreateShaderModule(dev, &smCI, nullptr, &vmod);
    
    smCI.codeSize = fcode.size();
    smCI.pCode = (const uint32_t*)fcode.data();
    VkShaderModule fmod;
    vkCreateShaderModule(dev, &smCI, nullptr, &fmod);
    
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";
    
    VkVertexInputBindingDescription binding{0, sizeof(IndicatorVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(IndicatorVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(IndicatorVertex, uv)};
    
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;
    
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
    
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    
    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable = VK_TRUE;
    cbA.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cbA.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbA.colorBlendOp = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbA.alphaBlendOp = VK_BLEND_OP_ADD;
    cbA.colorWriteMask = 0xF;
    VkPipelineColorBlendAttachmentState clickBlends[2] = {cbA, {}};
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2; cb.pAttachments = clickBlends;
    
    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyns;
    
    VkGraphicsPipelineCreateInfo gpCI{};
    gpCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCI.stageCount = 2; gpCI.pStages = stages; gpCI.pVertexInputState = &vi;
    gpCI.pInputAssemblyState = &ia; gpCI.pViewportState = &vp; gpCI.pRasterizationState = &rs;
    gpCI.pMultisampleState = &ms; gpCI.pDepthStencilState = &ds; gpCI.pColorBlendState = &cb;
    gpCI.pDynamicState = &dy; gpCI.layout = m_pipelineLayout;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    gpCI.pNext = &fmtCI;
    gpCI.renderPass = VK_NULL_HANDLE;
    
    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_pipeline);
    
    vkDestroyShaderModule(dev, fmod, nullptr);
    vkDestroyShaderModule(dev, vmod, nullptr);
}

void ClickIndicatorRenderer::createVertexBuffer() {
    IndicatorVertex vertices[6] = {
        {{-1.0f, 0.18f, -1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, 0.18f, -1.0f}, {1.0f, 0.0f}},
        {{ 1.0f, 0.18f,  1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 0.18f, -1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, 0.18f,  1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 0.18f,  1.0f}, {0.0f, 1.0f}}
    };
    VkDeviceSize size = sizeof(vertices);
    
    m_vertexBuffer = Buffer(m_device.getAllocator(), size, 
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 
                           VMA_MEMORY_USAGE_CPU_TO_GPU);
    
    std::memcpy(m_vertexBuffer.map(), vertices, (size_t)size);
}

void ClickIndicatorRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, 
                                    const glm::vec3& center, float t, float size,
                                    const glm::vec4& tint) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    
    PushConstants pc{};
    pc.viewProj = viewProj;
    pc.center = center;
    pc.size = size;
    pc.gridCount = 8;
    pc.frameIndex = 1 + static_cast<int>(t * 54.0f); // Start from frame 1, go up to 55
    pc.tint = tint;
    
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);
    
    VkDeviceSize offsets[] = {0};
    VkBuffer buf = m_vertexBuffer.getBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);
    
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

// ─── ShieldBubbleRenderer ───────────────────────────────────────────────────

// ── Sphere generation ─────────────────────────────────────────────────────────
// UV sphere with N_LAT latitude bands and N_LON longitude bands.
// Vertex position on a unit sphere == outward normal, so we store only position.

static constexpr int N_LAT = 16;
static constexpr int N_LON = 32;

void ShieldBubbleRenderer::generateSphere(const Device& device) {
    std::vector<glm::vec3> verts;
    verts.reserve((N_LAT + 1) * (N_LON + 1));

    for (int lat = 0; lat <= N_LAT; ++lat) {
        float theta = glm::pi<float>() * float(lat) / float(N_LAT);
        float sinT  = std::sin(theta);
        float cosT  = std::cos(theta);
        for (int lon = 0; lon <= N_LON; ++lon) {
            float phi  = 2.0f * glm::pi<float>() * float(lon) / float(N_LON);
            verts.push_back({sinT * std::cos(phi), cosT, sinT * std::sin(phi)});
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve(N_LAT * N_LON * 6);

    for (int lat = 0; lat < N_LAT; ++lat) {
        for (int lon = 0; lon < N_LON; ++lon) {
            uint16_t i00 = uint16_t(lat       * (N_LON + 1) + lon);
            uint16_t i10 = uint16_t((lat + 1) * (N_LON + 1) + lon);
            uint16_t i01 = uint16_t(lat       * (N_LON + 1) + (lon + 1));
            uint16_t i11 = uint16_t((lat + 1) * (N_LON + 1) + (lon + 1));
            // Triangle 1
            indices.push_back(i00); indices.push_back(i10); indices.push_back(i01);
            // Triangle 2
            indices.push_back(i10); indices.push_back(i11); indices.push_back(i01);
        }
    }

    m_indexCount = uint32_t(indices.size());

    m_vertexBuffer = Buffer::createDeviceLocal(device, device.getAllocator(),
        verts.data(), verts.size() * sizeof(glm::vec3),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_indexBuffer = Buffer::createDeviceLocal(device, device.getAllocator(),
        indices.data(), indices.size() * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

// ── Shader loading ────────────────────────────────────────────────────────────

VkShaderModule ShieldBubbleRenderer::loadShaderModule(VkDevice dev, const char* spirvPath) {
    std::ifstream f(spirvPath, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        spdlog::error("ShieldBubbleRenderer: cannot open shader '{}'", spirvPath);
        return VK_NULL_HANDLE;
    }
    size_t sz = size_t(f.tellg());
    std::vector<char> code(sz);
    f.seekg(0); f.read(code.data(), sz);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &mod);
    return mod;
}

// ── Pipeline creation ─────────────────────────────────────────────────────────

VkPipeline ShieldBubbleRenderer::createPipeline(const RenderFormats& formats,
                                                  VkCullModeFlags cullMode,
                                                  VkBlendFactor   srcColorFactor,
                                                  VkBlendFactor   dstColorFactor) {
    VkShaderModule vmod = loadShaderModule(m_dev,
        (std::string(SHADER_DIR) + "shield_bubble.vert.spv").c_str());
    VkShaderModule fmod = loadShaderModule(m_dev,
        (std::string(SHADER_DIR) + "shield_bubble.frag.spv").c_str());

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName  = "main";

    // Vertex input: position only (location 0, vec3)
    VkVertexInputBindingDescription vbind{};
    vbind.binding   = 0;
    vbind.stride    = sizeof(glm::vec3);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr{};
    vattr.location = 0;
    vattr.binding  = 0;
    vattr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    vattr.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions    = &vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = cullMode;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType              = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable    = VK_TRUE;
    ds.depthWriteEnable   = VK_FALSE;
    ds.depthCompareOp     = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable          = VK_TRUE;
    cbA.srcColorBlendFactor  = srcColorFactor;
    cbA.dstColorBlendFactor  = dstColorFactor;
    cbA.colorBlendOp         = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor  = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor  = VK_BLEND_FACTOR_ZERO;
    cbA.alphaBlendOp         = VK_BLEND_OP_ADD;
    cbA.colorWriteMask       = 0xF;
    VkPipelineColorBlendAttachmentState shieldBlends[2] = {cbA, {}};
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments    = shieldBlends;

    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyns;

    VkGraphicsPipelineCreateInfo gpCI{};
    gpCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCI.stageCount          = 2;
    gpCI.pStages             = stages;
    gpCI.pVertexInputState   = &vi;
    gpCI.pInputAssemblyState = &ia;
    gpCI.pViewportState      = &vp;
    gpCI.pRasterizationState = &rs;
    gpCI.pMultisampleState   = &ms;
    gpCI.pDepthStencilState  = &ds;
    gpCI.pColorBlendState    = &cb;
    gpCI.pDynamicState       = &dy;
    gpCI.layout              = m_pipelineLayout;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    gpCI.pNext               = &fmtCI;
    gpCI.renderPass          = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline);

    vkDestroyShaderModule(m_dev, fmod, nullptr);
    vkDestroyShaderModule(m_dev, vmod, nullptr);

    return pipeline;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void ShieldBubbleRenderer::init(const Device& device, const RenderFormats& formats) {
    m_dev = device.getDevice();

    generateSphere(device);

    // Pipeline layout — push constants only, no descriptor sets
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ShieldPC);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(m_dev, &layoutCI, nullptr, &m_pipelineLayout);

    // Back-face pass: standard alpha blend (soft inner glass tint)
    m_backfacePipeline = createPipeline(formats,
        VK_CULL_MODE_FRONT_BIT,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    // Front-face pass: additive blend (Fresnel rim glows bright)
    m_frontfacePipeline = createPipeline(formats,
        VK_CULL_MODE_BACK_BIT,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE);

    spdlog::info("ShieldBubbleRenderer initialized ({} sphere indices)", m_indexCount);
}

void ShieldBubbleRenderer::destroy() {
    if (m_dev == VK_NULL_HANDLE) return;
    if (m_frontfacePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_dev, m_frontfacePipeline, nullptr);
    if (m_backfacePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(m_dev, m_backfacePipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(m_dev, m_pipelineLayout, nullptr);
    m_vertexBuffer = Buffer{};
    m_indexBuffer  = Buffer{};
    m_dev = VK_NULL_HANDLE;
}

// ── Render ────────────────────────────────────────────────────────────────────

void ShieldBubbleRenderer::render(VkCommandBuffer cmd,
                                   const glm::mat4& viewProj,
                                   const glm::vec3& sphereCenter,
                                   const glm::vec3& cameraPos,
                                   float radius,
                                   float time,
                                   float alpha) {
    ShieldPC pc{};
    pc.viewProj      = viewProj;
    pc.sphereCenter  = sphereCenter;
    pc.radius        = radius;
    pc.cameraPos     = cameraPos;
    pc.time          = time;
    pc.alpha         = alpha;

    VkDeviceSize zero = 0;
    VkBuffer vb = m_vertexBuffer.getBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT16);

    // Pass 1: back-face — inner glass tint (standard alpha blend)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_backfacePipeline);
    vkCmdPushConstants(cmd, m_pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(pc), &pc);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);

    // Pass 2: front-face — Fresnel rim glow (additive blend)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_frontfacePipeline);
    vkCmdPushConstants(cmd, m_pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(pc), &pc);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

// ─── ConeAbilityRenderer ────────────────────────────────────────────────────

static constexpr int N_RINGS = 14;
static constexpr int N_SEGS  = 32;

// ── Shader loading ────────────────────────────────────────────────────────────
VkShaderModule ConeAbilityRenderer::loadShader(VkDevice dev, const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        spdlog::error("ConeAbilityRenderer: cannot open shader '{}'", path);
        return VK_NULL_HANDLE;
    }
    size_t sz = size_t(f.tellg());
    std::vector<char> code(sz);
    f.seekg(0);
    f.read(code.data(), sz);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &mod);
    return mod;
}

// ── Cone mesh generation (FLAT 2D GROUND INDICATOR) ──────────────────────────
// Builds a flat polar fan in UV space. Each vertex encodes:
//   pos.x = u_angle  ∈ [0,1]   (0 = left edge, 0.5 = axis, 1 = right edge)
//   pos.z = v_radius ∈ [0,1]   (0 = apex tip,  1 = far arc)
//   pos.y = 0 (unused)
// The vertex shader reconstructs the actual XZ world position using the
// half-angle and range supplied via push constants.
void ConeAbilityRenderer::generateConeMesh(const Device& device) {
    std::vector<ConeVertex> verts;
    verts.reserve(N_RINGS * (N_SEGS + 1));

    for (int r = 0; r < N_RINGS; ++r) {
        const float v = float(r) / float(N_RINGS - 1);
        for (int s = 0; s <= N_SEGS; ++s) {
            const float u = float(s) / float(N_SEGS);
            ConeVertex cv;
            cv.pos = { u, 0.0f, v };
            cv.uv  = { u, v };
            verts.push_back(cv);
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve((N_RINGS - 1) * N_SEGS * 6);
    for (int r = 0; r < N_RINGS - 1; ++r) {
        for (int s = 0; s < N_SEGS; ++s) {
            uint16_t v0 = uint16_t( r      * (N_SEGS + 1) + s);
            uint16_t v1 = uint16_t( r      * (N_SEGS + 1) + s + 1);
            uint16_t v2 = uint16_t((r + 1) * (N_SEGS + 1) + s);
            uint16_t v3 = uint16_t((r + 1) * (N_SEGS + 1) + s + 1);
            indices.push_back(v0); indices.push_back(v1); indices.push_back(v3);
            indices.push_back(v0); indices.push_back(v3); indices.push_back(v2);
        }
    }

    m_coneIndexCount = uint32_t(indices.size());

    m_coneVB = Buffer::createDeviceLocal(device, device.getAllocator(),
        verts.data(), verts.size() * sizeof(ConeVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_coneIB = Buffer::createDeviceLocal(device, device.getAllocator(),
        indices.data(), indices.size() * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

// ── Lightning buffer ──────────────────────────────────────────────────────────
void ConeAbilityRenderer::createLightningBuffer() {
    const VkDeviceSize size = VkDeviceSize(N_BOLTS * N_BOLT_SEGS) * sizeof(glm::vec3);
    m_lightningVB = Buffer(m_allocator, size,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VMA_MEMORY_USAGE_CPU_TO_GPU);
    // Zero-initialise so first frame draws nothing visible
    void* ptr = m_lightningVB.map();
    std::memset(ptr, 0, size);
    m_lightningVB.flush();
    m_lightningVB.unmap();
}

void ConeAbilityRenderer::regenerateLightning(const glm::vec3& apex,
                                               const glm::vec3& axisDir,
                                               float halfAngleRad,
                                               float range) {
    // Flat XZ basis — arcs lie on the ground plane
    glm::vec3 fwd = glm::normalize(glm::vec3(axisDir.x, 0.0f, axisDir.z));
    glm::vec3 rt  = glm::vec3(-fwd.z, 0.0f, fwd.x);  // right in XZ
    float groundY = apex.y + 0.04f;                   // tiny lift above ground

    std::vector<glm::vec3> verts(N_BOLTS * N_BOLT_SEGS);

    for (int b = 0; b < N_BOLTS; ++b) {
        // Each bolt picks a random starting angle within the half-cone
        float boltAngle = (m_rdist(m_rng) * 2.0f - 1.0f) * halfAngleRad * 0.85f;

        for (int s = 0; s < N_BOLT_SEGS; ++s) {
            float v = float(s) / float(N_BOLT_SEGS - 1);
            float r = v * range;

            // Jitter angle as we advance — clamp to stay inside cone
            boltAngle += (m_rdist(m_rng) - 0.5f) * halfAngleRad * 0.35f;
            boltAngle  = glm::clamp(boltAngle, -halfAngleRad * 0.92f, halfAngleRad * 0.92f);

            glm::vec3 pos = apex
                          + (fwd * std::cos(boltAngle) + rt * std::sin(boltAngle)) * r;
            pos.y = groundY;
            verts[b * N_BOLT_SEGS + s] = pos;
        }
    }

    void* ptr = m_lightningVB.map();
    std::memcpy(ptr, verts.data(), verts.size() * sizeof(glm::vec3));
    m_lightningVB.flush();
    m_lightningVB.unmap();
}

// ── Pipeline factory ──────────────────────────────────────────────────────────
VkPipeline ConeAbilityRenderer::createPipeline(const RenderFormats&    formats,
                                                const std::string&  vertSpv,
                                                const std::string&  fragSpv,
                                                VkPrimitiveTopology topology,
                                                VkCullModeFlags     cullMode,
                                                VkBlendFactor       srcFactor,
                                                VkBlendFactor       dstFactor) {
    VkShaderModule vmod = loadShader(m_dev, vertSpv);
    VkShaderModule fmod = loadShader(m_dev, fragSpv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName  = "main";

    const bool isLightning = (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);

    VkVertexInputBindingDescription vbind{};
    vbind.binding   = 0;
    vbind.stride    = isLightning ? uint32_t(sizeof(glm::vec3))
                                  : uint32_t(sizeof(ConeVertex));
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // location 0 = position (vec3); cone also gets location 1 = uv (vec2)
    VkVertexInputAttributeDescription vattrs[2]{};
    vattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    uint32_t attrCount = 1;
    if (!isLightning) {
        vattrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,
                      uint32_t(offsetof(ConeVertex, uv)) };
        attrCount = 2;
    }

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = attrCount;
    vi.pVertexAttributeDescriptions    = vattrs;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topology;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = cullMode;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable         = VK_TRUE;
    cbA.srcColorBlendFactor = srcFactor;
    cbA.dstColorBlendFactor = dstFactor;
    cbA.colorBlendOp        = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.alphaBlendOp        = VK_BLEND_OP_ADD;
    cbA.colorWriteMask      = 0xF;
    VkPipelineColorBlendAttachmentState coneBlends[2] = {cbA, {}};
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2;
    cb.pAttachments    = coneBlends;

    VkDynamicState dyns[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyns;

    VkGraphicsPipelineCreateInfo gpCI{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpCI.stageCount          = 2;
    gpCI.pStages             = stages;
    gpCI.pVertexInputState   = &vi;
    gpCI.pInputAssemblyState = &ia;
    gpCI.pViewportState      = &vp;
    gpCI.pRasterizationState = &rs;
    gpCI.pMultisampleState   = &ms;
    gpCI.pDepthStencilState  = &ds;
    gpCI.pColorBlendState    = &cb;
    gpCI.pDynamicState       = &dy;
    gpCI.layout              = m_layout;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    gpCI.pNext               = &fmtCI;
    gpCI.renderPass          = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline);

    vkDestroyShaderModule(m_dev, fmod, nullptr);
    vkDestroyShaderModule(m_dev, vmod, nullptr);

    return pipeline;
}

// ── init ──────────────────────────────────────────────────────────────────────
void ConeAbilityRenderer::init(const Device& device, const RenderFormats& formats) {
    m_dev       = device.getDevice();
    m_allocator = device.getAllocator();

    generateConeMesh(device);
    createLightningBuffer();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ConePC);

    VkPipelineLayoutCreateInfo layoutCI{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(m_dev, &layoutCI, nullptr, &m_layout);

    const std::string sd = std::string(SHADER_DIR);

    // Pass 1: Interior energy (both faces, additive)
    m_energyPipeline = createPipeline(formats,
        sd + "cone_ability.vert.spv", sd + "cone_energy.frag.spv",
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);

    // Pass 2: Surface grid overlay (both faces, additive)
    m_gridPipeline = createPipeline(formats,
        sd + "cone_ability.vert.spv", sd + "cone_grid.frag.spv",
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);

    // Pass 3: Lightning arcs (line strip, fully additive)
    m_lightningPipeline = createPipeline(formats,
        sd + "cone_lightning.vert.spv", sd + "cone_lightning.frag.spv",
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);

    spdlog::info("ConeAbilityRenderer initialized ({} cone indices, {} lightning verts)",
                 m_coneIndexCount, N_BOLTS * N_BOLT_SEGS);
}

// ── update ────────────────────────────────────────────────────────────────────
void ConeAbilityRenderer::update(float dt,
                                  const glm::vec3& apex,
                                  const glm::vec3& axisDir,
                                  float halfAngleRad,
                                  float range,
                                  float elapsed) {
    // Restrict lightning to the current wave-front position (apex→waveFront)
    static constexpr float WAVE_DUR = 1.2f;
    float waveFront = std::min(elapsed / WAVE_DUR, 1.0f) * range;

    m_lightningTimer -= dt;
    if (m_lightningTimer <= 0.0f) {
        m_lightningTimer = LIGHTNING_RATE;
        regenerateLightning(apex, axisDir, halfAngleRad, waveFront > 0.5f ? waveFront : 0.5f);
    }
}

// ── render ────────────────────────────────────────────────────────────────────
void ConeAbilityRenderer::render(VkCommandBuffer  cmd,
                                  const glm::mat4& viewProj,
                                  const glm::vec3& apex,
                                  const glm::vec3& axisDir,
                                  float            halfAngleRad,
                                  float            range,
                                  const glm::vec3& cameraPos,
                                  float            time,
                                  float            elapsed,
                                  float            alpha) {
    ConePC pc{};
    pc.viewProj     = viewProj;
    pc.apex         = apex;
    pc.time         = time;
    pc.axisDir      = axisDir;
    pc.halfAngleTan = std::tan(halfAngleRad);
    pc.cameraPos    = cameraPos;
    pc.range        = range;
    pc.alpha        = alpha;
    pc.elapsed      = elapsed;
    pc.phase        = 0.0f;

    const VkDeviceSize zero = 0;
    VkBuffer           vb   = m_coneVB.getBuffer();

    // Bind cone geometry once — shared by energy and grid passes
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdBindIndexBuffer(cmd, m_coneIB.getBuffer(), 0, VK_INDEX_TYPE_UINT16);

    // ── Pass 1: Interior energy ───────────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_energyPipeline);
    vkCmdPushConstants(cmd, m_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(ConePC), &pc);
    vkCmdDrawIndexed(cmd, m_coneIndexCount, 1, 0, 0, 0);

    // ── Pass 2: Grid overlay ──────────────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
    vkCmdPushConstants(cmd, m_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(ConePC), &pc);
    vkCmdDrawIndexed(cmd, m_coneIndexCount, 1, 0, 0, 0);

    // Pass 3: Lightning arcs ────────────────────────────────────────────────
    VkBuffer lvb = m_lightningVB.getBuffer();
    if (lvb != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightningPipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &lvb, &zero);
        // Each bolt is an independent line strip
        for (int b = 0; b < N_BOLTS; ++b) {
            pc.phase = float(b) * 2.39996f; // golden-ratio offset
            vkCmdPushConstants(cmd, m_layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(ConePC), &pc);
            vkCmdDraw(cmd, N_BOLT_SEGS, 1, uint32_t(b * N_BOLT_SEGS), 0);
        }
    }

}

// ── destroy ───────────────────────────────────────────────────────────────────
void ConeAbilityRenderer::destroy() {
    if (m_dev == VK_NULL_HANDLE) return;

    if (m_lightningPipeline) vkDestroyPipeline(m_dev, m_lightningPipeline, nullptr);
    if (m_gridPipeline)      vkDestroyPipeline(m_dev, m_gridPipeline,      nullptr);
    if (m_energyPipeline)    vkDestroyPipeline(m_dev, m_energyPipeline,    nullptr);
    if (m_layout)            vkDestroyPipelineLayout(m_dev, m_layout,      nullptr);

    m_lightningPipeline = VK_NULL_HANDLE;
    m_gridPipeline      = VK_NULL_HANDLE;
    m_energyPipeline    = VK_NULL_HANDLE;
    m_layout            = VK_NULL_HANDLE;
    m_dev               = VK_NULL_HANDLE;
}

// ─── ExplosionRenderer ──────────────────────────────────────────────────────

static constexpr int DISK_RINGS = 12;
static constexpr int DISK_SEGS  = 32;
static constexpr int SPH_LAT    = 16;
static constexpr int SPH_LON    = 32;

// ── Shader loading ────────────────────────────────────────────────────────────
VkShaderModule ExplosionRenderer::loadShader(VkDevice dev, const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        spdlog::error("ExplosionRenderer: cannot open shader '{}'", path);
        return VK_NULL_HANDLE;
    }
    size_t sz = size_t(f.tellg());
    std::vector<char> code(sz);
    f.seekg(0);
    f.read(code.data(), sz);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &mod);
    return mod;
}

// ── Disk mesh generation ──────────────────────────────────────────────────────
// Flat polar grid: vertex = (cos θ, 0, sin θ) × ringFrac; UV = (ringFrac, θFrac)
void ExplosionRenderer::generateDiskMesh(const Device& device) {
    std::vector<DiskVertex> verts;
    verts.reserve((DISK_RINGS + 1) * (DISK_SEGS + 1));

    for (int r = 0; r <= DISK_RINGS; ++r) {
        float rf = float(r) / float(DISK_RINGS);  // ring fraction 0..1
        for (int s = 0; s <= DISK_SEGS; ++s) {
            float tf    = float(s) / float(DISK_SEGS);
            float theta = tf * glm::two_pi<float>();
            DiskVertex v{};
            v.pos = { std::cos(theta), 0.0f, std::sin(theta) };  // unit; shader scales by rf * maxRadius
            v.uv  = { rf, tf };
            verts.push_back(v);
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve(DISK_RINGS * DISK_SEGS * 6);
    for (int r = 0; r < DISK_RINGS; ++r) {
        for (int s = 0; s < DISK_SEGS; ++s) {
            uint16_t v0 = uint16_t( r      * (DISK_SEGS + 1) + s);
            uint16_t v1 = uint16_t( r      * (DISK_SEGS + 1) + s + 1);
            uint16_t v2 = uint16_t((r + 1) * (DISK_SEGS + 1) + s);
            uint16_t v3 = uint16_t((r + 1) * (DISK_SEGS + 1) + s + 1);
            indices.push_back(v0); indices.push_back(v2); indices.push_back(v1);
            indices.push_back(v1); indices.push_back(v2); indices.push_back(v3);
        }
    }

    m_diskIndexCount = uint32_t(indices.size());
    m_diskVB = Buffer::createDeviceLocal(device, device.getAllocator(),
        verts.data(), verts.size() * sizeof(DiskVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_diskIB = Buffer::createDeviceLocal(device, device.getAllocator(),
        indices.data(), indices.size() * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

// ── Sphere mesh generation ────────────────────────────────────────────────────
// UV sphere (position-only vertices on unit sphere).
void ExplosionRenderer::generateSphereMesh(const Device& device) {
    std::vector<glm::vec3> verts;
    verts.reserve((SPH_LAT + 1) * (SPH_LON + 1));

    for (int lat = 0; lat <= SPH_LAT; ++lat) {
        float phi = glm::pi<float>() * float(lat) / float(SPH_LAT);  // 0..π
        for (int lon = 0; lon <= SPH_LON; ++lon) {
            float theta = glm::two_pi<float>() * float(lon) / float(SPH_LON);
            verts.push_back({
                std::sin(phi) * std::cos(theta),
                std::cos(phi),
                std::sin(phi) * std::sin(theta)
            });
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve(SPH_LAT * SPH_LON * 6);
    for (int lat = 0; lat < SPH_LAT; ++lat) {
        for (int lon = 0; lon < SPH_LON; ++lon) {
            uint16_t v0 = uint16_t( lat      * (SPH_LON + 1) + lon);
            uint16_t v1 = uint16_t( lat      * (SPH_LON + 1) + lon + 1);
            uint16_t v2 = uint16_t((lat + 1) * (SPH_LON + 1) + lon);
            uint16_t v3 = uint16_t((lat + 1) * (SPH_LON + 1) + lon + 1);
            indices.push_back(v0); indices.push_back(v2); indices.push_back(v1);
            indices.push_back(v1); indices.push_back(v2); indices.push_back(v3);
        }
    }

    m_sphereIndexCount = uint32_t(indices.size());
    m_sphereVB = Buffer::createDeviceLocal(device, device.getAllocator(),
        verts.data(), verts.size() * sizeof(glm::vec3),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_sphereIB = Buffer::createDeviceLocal(device, device.getAllocator(),
        indices.data(), indices.size() * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

// ── Pipeline factory ──────────────────────────────────────────────────────────
VkPipeline ExplosionRenderer::createPipeline(const RenderFormats&   formats,
                                              const std::string& vertSpv,
                                              const std::string& fragSpv,
                                              VkCullModeFlags    cullMode,
                                              VkBlendFactor      srcFactor,
                                              VkBlendFactor      dstFactor) {
    VkShaderModule vmod = loadShader(m_dev, vertSpv);
    VkShaderModule fmod = loadShader(m_dev, fragSpv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName  = "main";

    // Disk uses pos+uv; sphere uses pos only.
    // Discriminate by checking the spv path name (simpler than passing a flag).
    const bool isDisk = (vertSpv.find("disk") != std::string::npos);

    VkVertexInputBindingDescription vbind{};
    vbind.binding   = 0;
    vbind.stride    = isDisk ? uint32_t(sizeof(DiskVertex)) : uint32_t(sizeof(glm::vec3));
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattrs[2]{};
    vattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    uint32_t attrCount = 1;
    if (isDisk) {
        vattrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(DiskVertex, uv)) };
        attrCount = 2;
    }

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = attrCount;
    vi.pVertexAttributeDescriptions    = vattrs;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = cullMode;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable         = VK_TRUE;
    cbA.srcColorBlendFactor = srcFactor;
    cbA.dstColorBlendFactor = dstFactor;
    cbA.colorBlendOp        = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.alphaBlendOp        = VK_BLEND_OP_ADD;
    cbA.colorWriteMask      = 0xF;
    VkPipelineColorBlendAttachmentState explBlends[2] = {cbA, {}};
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2;
    cb.pAttachments    = explBlends;

    VkDynamicState dyns[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyns;

    VkGraphicsPipelineCreateInfo gpCI{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpCI.stageCount          = 2;
    gpCI.pStages             = stages;
    gpCI.pVertexInputState   = &vi;
    gpCI.pInputAssemblyState = &ia;
    gpCI.pViewportState      = &vp;
    gpCI.pRasterizationState = &rs;
    gpCI.pMultisampleState   = &ms;
    gpCI.pDepthStencilState  = &ds;
    gpCI.pColorBlendState    = &cb;
    gpCI.pDynamicState       = &dy;
    gpCI.layout              = m_layout;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    gpCI.pNext               = &fmtCI;
    gpCI.renderPass          = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline);

    vkDestroyShaderModule(m_dev, fmod, nullptr);
    vkDestroyShaderModule(m_dev, vmod, nullptr);

    return pipeline;
}

// ── init ──────────────────────────────────────────────────────────────────────
void ExplosionRenderer::init(const Device& device, const RenderFormats& formats) {
    m_dev       = device.getDevice();
    m_allocator = device.getAllocator();

    generateDiskMesh(device);
    generateSphereMesh(device);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ExplosionPC);

    VkPipelineLayoutCreateInfo layoutCI{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(m_dev, &layoutCI, nullptr, &m_layout);

    const std::string sd = std::string(SHADER_DIR);

    // Pass 1: Shockwave disk (both faces, alpha-blend)
    m_shockwavePipeline = createPipeline(formats,
        sd + "explosion_disk.vert.spv",
        sd + "explosion_shockwave.frag.spv",
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    // Pass 2: Fireball sphere (both faces, additive)
    m_fireballPipeline = createPipeline(formats,
        sd + "explosion_sphere.vert.spv",
        sd + "explosion_fireball.frag.spv",
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);

    spdlog::info("ExplosionRenderer initialized ({} disk idx, {} sphere idx)",
                 m_diskIndexCount, m_sphereIndexCount);
}

// ── addExplosion ──────────────────────────────────────────────────────────────
void ExplosionRenderer::addExplosion(glm::vec3 center) {
    if (int(m_active.size()) >= MAX_EXPLOSIONS) return;
    m_active.push_back({ center, 0.0f, 4.0f });
}

// ── update ────────────────────────────────────────────────────────────────────
void ExplosionRenderer::update(float dt) {
    for (auto& exp : m_active)
        exp.elapsed += dt;

    m_active.erase(
        std::remove_if(m_active.begin(), m_active.end(),
            [](const ExplosionInstance& e) { return e.elapsed >= DURATION; }),
        m_active.end());
}

// ── render ────────────────────────────────────────────────────────────────────
void ExplosionRenderer::render(VkCommandBuffer  cmd,
                                const glm::mat4& viewProj,
                                const glm::vec3& cameraPos,
                                float            appTime) {
    if (m_active.empty()) return;
    if (!m_shockwavePipeline || !m_fireballPipeline) return;

    for (const auto& exp : m_active) {
        // Master alpha: fade out over the last 0.5s of DURATION
        float fadeOut = 1.0f - glm::clamp((exp.elapsed - (DURATION - 0.5f)) / 0.5f, 0.0f, 1.0f);

        ExplosionPC pc{};
        pc.viewProj  = viewProj;
        pc.center    = exp.center;
        pc.elapsed   = exp.elapsed;
        pc.cameraPos = cameraPos;
        pc.maxRadius = exp.maxRadius;
        pc.alpha     = fadeOut;
        pc.appTime   = appTime;

        // ── Pass 1: Shockwave disk ────────────────────────────────────────
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shockwavePipeline);
        vkCmdPushConstants(cmd, m_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ExplosionPC), &pc);
        VkBuffer diskBuf = m_diskVB.getBuffer();
        VkDeviceSize diskOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &diskBuf, &diskOff);
        vkCmdBindIndexBuffer(cmd, m_diskIB.getBuffer(), 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, m_diskIndexCount, 1, 0, 0, 0);

        // ── Pass 2: Fireball sphere ───────────────────────────────────────
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fireballPipeline);
        vkCmdPushConstants(cmd, m_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ExplosionPC), &pc);
        VkBuffer sphBuf = m_sphereVB.getBuffer();
        VkDeviceSize sphOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &sphBuf, &sphOff);
        vkCmdBindIndexBuffer(cmd, m_sphereIB.getBuffer(), 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, m_sphereIndexCount, 1, 0, 0, 0);
    }
}

// ── destroy ───────────────────────────────────────────────────────────────────
void ExplosionRenderer::destroy() {
    if (!m_dev) return;

    if (m_shockwavePipeline) { vkDestroyPipeline(m_dev, m_shockwavePipeline, nullptr); m_shockwavePipeline = VK_NULL_HANDLE; }
    if (m_fireballPipeline)  { vkDestroyPipeline(m_dev, m_fireballPipeline,  nullptr); m_fireballPipeline  = VK_NULL_HANDLE; }
    if (m_layout)            { vkDestroyPipelineLayout(m_dev, m_layout,      nullptr); m_layout            = VK_NULL_HANDLE; }

    m_diskVB.destroy();
    m_diskIB.destroy();
    m_sphereVB.destroy();
    m_sphereIB.destroy();

    m_dev = VK_NULL_HANDLE;
}

// ─── SpriteEffectRenderer ───────────────────────────────────────────────────

struct SpriteVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

// ─── Lifecycle ──────────────────────────────────────────────────────────────

SpriteEffectRenderer::~SpriteEffectRenderer() { destroy(); }

void SpriteEffectRenderer::init(const Device& device, const RenderFormats& formats) {
    m_device = &device;
    createDescriptorResources();
    createPipelines(formats);
    createVertexBuffer();
    spdlog::info("SpriteEffectRenderer initialized");
}

void SpriteEffectRenderer::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_alphaPipeline)    vkDestroyPipeline(dev, m_alphaPipeline, nullptr);
    if (m_additivePipeline) vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_pipelineLayout)   vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descPool)         vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout)       vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);

    m_alphaPipeline = m_additivePipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_effects.clear();
    m_active.clear();
    m_device = nullptr;
}

// ─── Descriptor resources ───────────────────────────────────────────────────

void SpriteEffectRenderer::createDescriptorResources() {
    VkDevice dev = m_device->getDevice();

    // Layout: one combined image sampler at binding 0
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout);

    // Pool: enough for 16 effect types
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 16;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 16;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);
}

VkDescriptorSet SpriteEffectRenderer::allocateDescSetForTexture(const Texture& tex) {
    VkDevice dev = m_device->getDevice();

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev, &allocInfo, &set);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = tex.getImageView();
    imgInfo.sampler     = tex.getSampler();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    return set;
}

// ─── Pipeline creation ──────────────────────────────────────────────────────


void SpriteEffectRenderer::createPipelines(const RenderFormats& formats) {
    VkDevice dev = m_device->getDevice();

    // Push constant range
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout);

    // Shader modules — billboard vertex shader for camera-facing sprites
    auto vcode = readFile(std::string(SHADER_DIR) + "sprite_effect_billboard.vert.spv");
    auto fAlpha    = readFile(std::string(SHADER_DIR) + "click_indicator.frag.spv");
    auto fAdditive = readFile(std::string(SHADER_DIR) + "sprite_effect_additive.frag.spv");

    auto createModule = [&](const std::vector<char>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode    = (const uint32_t*)code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule vMod      = createModule(vcode);
    VkShaderModule fAlphaMod = createModule(fAlpha);
    VkShaderModule fAddMod   = createModule(fAdditive);

    // Shared pipeline state
    VkVertexInputBindingDescription binding{0, sizeof(SpriteVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SpriteVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteVertex, uv)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyns;

    // ── Alpha-blend pipeline ─────────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fAlphaMod;
        stages[1].pName  = "main";

        VkPipelineColorBlendAttachmentState cb{};
        cb.blendEnable         = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp        = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.alphaBlendOp        = VK_BLEND_OP_ADD;
        cb.colorWriteMask      = 0xF;

        VkPipelineColorBlendAttachmentState spriteAlphaBlends[2] = {cb, {}};
        VkPipelineColorBlendStateCreateInfo cbCI{};
        cbCI.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = spriteAlphaBlends;

        VkGraphicsPipelineCreateInfo gpCI{};
        gpCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpCI.stageCount          = 2;
        gpCI.pStages             = stages;
        gpCI.pVertexInputState   = &vi;
        gpCI.pInputAssemblyState = &ia;
        gpCI.pViewportState      = &vp;
        gpCI.pRasterizationState = &rs;
        gpCI.pMultisampleState   = &ms;
        gpCI.pDepthStencilState  = &ds;
        gpCI.pColorBlendState    = &cbCI;
        gpCI.pDynamicState       = &dy;
        gpCI.layout              = m_pipelineLayout;
        VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
        gpCI.pNext               = &fmtCI;
        gpCI.renderPass          = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_alphaPipeline);
    }

    // ── Additive-blend pipeline ──────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fAddMod;
        stages[1].pName  = "main";

        VkPipelineColorBlendAttachmentState cb{};
        cb.blendEnable         = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.colorBlendOp        = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.alphaBlendOp        = VK_BLEND_OP_ADD;
        cb.colorWriteMask      = 0xF;

        VkPipelineColorBlendAttachmentState spriteAddBlends[2] = {cb, {}};
        VkPipelineColorBlendStateCreateInfo cbCI{};
        cbCI.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = spriteAddBlends;

        VkGraphicsPipelineCreateInfo gpCI{};
        gpCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpCI.stageCount          = 2;
        gpCI.pStages             = stages;
        gpCI.pVertexInputState   = &vi;
        gpCI.pInputAssemblyState = &ia;
        gpCI.pViewportState      = &vp;
        gpCI.pRasterizationState = &rs;
        gpCI.pMultisampleState   = &ms;
        gpCI.pDepthStencilState  = &ds;
        gpCI.pColorBlendState    = &cbCI;
        gpCI.pDynamicState       = &dy;
        gpCI.layout              = m_pipelineLayout;
        VkPipelineRenderingCreateInfo fmtCI2 = formats.pipelineRenderingCI();
        gpCI.pNext               = &fmtCI2;
        gpCI.renderPass          = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_additivePipeline);
    }

    vkDestroyShaderModule(dev, fAddMod, nullptr);
    vkDestroyShaderModule(dev, fAlphaMod, nullptr);
    vkDestroyShaderModule(dev, vMod, nullptr);
}

// ─── Vertex buffer (ground-plane billboard quad) ────────────────────────────

void SpriteEffectRenderer::createVertexBuffer() {
    // Billboard quad: XY offsets (camera-facing), UV maps to atlas cell
    SpriteVertex vertices[6] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},  // bottom-left
        {{ 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},  // bottom-right
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f}},  // top-right
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},  // bottom-left
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f}},  // top-right
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f}},  // top-left
    };
    VkDeviceSize size = sizeof(vertices);
    m_vertexBuffer = Buffer(m_device->getAllocator(), size,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(m_vertexBuffer.map(), vertices, (size_t)size);
}

// ─── Effect registration ────────────────────────────────────────────────────

uint32_t SpriteEffectRenderer::registerEffect(const std::string& name,
                                              const std::string& atlasPath,
                                              int gridCount, int frameCount,
                                              float duration, bool additive) {
    EffectType fx;
    fx.name       = name;
    fx.gridCount  = gridCount;
    fx.frameCount = frameCount;
    fx.duration   = duration;
    fx.additive   = additive;
    fx.atlas      = std::make_unique<Texture>(*m_device, atlasPath);
    fx.descSet    = allocateDescSetForTexture(*fx.atlas);

    uint32_t id = static_cast<uint32_t>(m_effects.size());
    m_effects.push_back(std::move(fx));
    spdlog::info("SpriteEffectRenderer: registered '{}' (id={}, {}×{} grid, {} frames, {:.2f}s, {})",
                 name, id, gridCount, gridCount, frameCount, duration,
                 additive ? "additive" : "alpha");
    return id;
}

// ─── Spawn / Update / Render ────────────────────────────────────────────────

void SpriteEffectRenderer::spawn(uint32_t effectId, const glm::vec3& position,
                                 float size, const glm::vec4& tint) {
    if (effectId >= m_effects.size()) return;
    if (static_cast<int>(m_active.size()) >= MAX_INSTANCES) {
        m_active.erase(m_active.begin()); // evict oldest
    }
    Instance inst;
    inst.effectId = effectId;
    inst.position = position;
    inst.size     = size;
    inst.tint     = tint;
    inst.duration = m_effects[effectId].duration;
    m_active.push_back(inst);
}

void SpriteEffectRenderer::update(float dt) {
    for (auto& inst : m_active)
        inst.elapsed += dt;

    // Remove expired
    m_active.erase(
        std::remove_if(m_active.begin(), m_active.end(),
                       [](const Instance& i) { return i.elapsed >= i.duration; }),
        m_active.end());
}

void SpriteEffectRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                  const glm::vec3& /*cameraPos*/) {
    if (m_active.empty()) return;

    VkDeviceSize offsets[] = {0};
    VkBuffer buf = m_vertexBuffer.getBuffer();

    // Group draws by pipeline (alpha vs additive) to minimize pipeline switches
    for (int pass = 0; pass < 2; ++pass) {
        bool wantAdditive = (pass == 1);
        VkPipeline pipeline = wantAdditive ? m_additivePipeline : m_alphaPipeline;
        bool bound = false;

        for (const auto& inst : m_active) {
            const auto& fx = m_effects[inst.effectId];
            if (fx.additive != wantAdditive) continue;

            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);
                bound = true;
            }

            // Bind this effect's atlas texture
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &fx.descSet, 0, nullptr);

            // Compute frame index from elapsed time
            float t = inst.elapsed / inst.duration;
            int frameIndex = static_cast<int>(t * static_cast<float>(fx.frameCount));
            if (frameIndex >= fx.frameCount) frameIndex = fx.frameCount - 1;

            PushConstants pc{};
            pc.viewProj   = viewProj;
            pc.center     = inst.position;
            pc.size       = inst.size;
            pc.frameIndex = frameIndex;
            pc.gridCount  = fx.gridCount;
            pc.tint       = inst.tint;

            vkCmdPushConstants(cmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);

            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }
}

// ─── GroundDecalRenderer ────────────────────────────────────────────────────

GroundDecalRenderer::GroundDecalRenderer(const Device& device, const RenderFormats& formats)
    : m_device(device), m_formats(formats) 
{
    m_defaultTexture = std::make_unique<Texture>(Texture::createDefault(device));
    m_quadMesh = std::make_unique<Model>(Model::createUnitQuad(device, device.getAllocator()));

    createDescriptorLayout();
    createPipelines();

    // Pool for decal descriptor sets: 2 samplers per set (binding 0 = decal tex, binding 1 = FoW)
    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets = 32;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device.getDevice(), &poolCI, nullptr, &m_descPool), "Create Decal pool");
}

GroundDecalRenderer::~GroundDecalRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    
    if (m_alphaPipeline) vkDestroyPipeline(dev, m_alphaPipeline, nullptr);
    if (m_additivePipeline) vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

void GroundDecalRenderer::registerDecal(const DecalDef& def) {
    m_defs[def.id] = def;
}

uint32_t GroundDecalRenderer::spawn(const std::string& decalDefId, glm::vec3 center, float radius, float rotation) {
    auto it = m_defs.find(decalDefId);
    if (it == m_defs.end()) {
        spdlog::warn("GroundDecalRenderer: unknown def '{}'", decalDefId);
        return 0;
    }

    DecalInstance inst;
    inst.handle = m_nextHandle++;
    inst.def = &it->second;
    inst.center = center;
    inst.radius = radius;
    inst.rotation = rotation;
    inst.texture = getOrLoadTexture(inst.def->texturePath);

    m_activeDecals.push_back(inst);
    return inst.handle;
}

void GroundDecalRenderer::update(float dt) {
    for (auto it = m_activeDecals.begin(); it != m_activeDecals.end(); ) {
        it->elapsed += dt;
        it->rotation += it->def->rotationSpeed * dt;

        if (it->elapsed >= it->def->duration) {
            it = m_activeDecals.erase(it);
        } else {
            ++it;
        }
    }
}

void GroundDecalRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime) {
    if (m_activeDecals.empty()) return;

    for (const auto& inst : m_activeDecals) {
        float alpha = 1.0f;
        if (inst.elapsed < inst.def->fadeInTime) {
            alpha = inst.elapsed / inst.def->fadeInTime;
        } else if (inst.elapsed > (inst.def->duration - inst.def->fadeOutTime)) {
            alpha = (inst.def->duration - inst.elapsed) / inst.def->fadeOutTime;
        }
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        DecalPC pc;
        pc.viewProj  = viewProj;
        pc.center    = inst.center;
        pc.radius    = inst.radius;
        pc.rotation  = inst.rotation;
        pc.alpha     = alpha;
        pc.elapsed   = inst.elapsed;
        pc.appTime   = appTime;
        pc.color     = inst.def->color;
        pc.fowMapMin = m_fowMapMin;
        pc.fowMapMax = m_fowMapMax;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inst.def->additive ? m_additivePipeline : m_alphaPipeline);
        
        VkDescriptorSet ds = getOrCreateDescriptorSet(inst.texture);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &ds, 0, nullptr);
        
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DecalPC), &pc);
        
        m_quadMesh->draw(cmd);
    }
}

void GroundDecalRenderer::destroy(uint32_t handle) {
    m_activeDecals.erase(std::remove_if(m_activeDecals.begin(), m_activeDecals.end(),
        [handle](const DecalInstance& d) { return d.handle == handle; }), m_activeDecals.end());
}

void GroundDecalRenderer::destroyAll() {
    m_activeDecals.clear();
}

void GroundDecalRenderer::createDescriptorLayout() {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    // binding 0: decal texture
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    // binding 1: Fog of War visibility texture
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 2;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Decal Layout");
}

void GroundDecalRenderer::createPipelines() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(DecalPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Decal Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "ground_decal.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "ground_decal.frag.spv");

    VkShaderModule vertMod = createShaderModule(vertCode);
    VkShaderModule fragMod = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    auto vertBinding = Vertex::getBindingDescription();
    auto vertAttrs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    viCI.vertexBindingDescriptionCount = 1;
    viCI.pVertexBindingDescriptions = &vertBinding;
    viCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertAttrs.size());
    viCI.pVertexAttributeDescriptions = vertAttrs.data();

    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1; vpCI.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsCI.polygonMode = VK_POLYGON_MODE_FILL; rsCI.lineWidth = 1.0f; rsCI.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable = VK_TRUE; dsCI.depthWriteEnable = VK_FALSE; dsCI.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    // Second attachment intentionally write-masked (brightness attachment unused by decals)
    VkPipelineColorBlendAttachmentState decalBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments = decalBlends;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2; dynCI.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeCI.stageCount = 2;
    pipeCI.pStages = stages;
    pipeCI.pVertexInputState = &viCI;
    pipeCI.pInputAssemblyState = &iaCI;
    pipeCI.pViewportState = &vpCI;
    pipeCI.pRasterizationState = &rsCI;
    pipeCI.pMultisampleState = &msCI;
    pipeCI.pDepthStencilState = &dsCI;
    pipeCI.pColorBlendState = &cbCI;
    pipeCI.pDynamicState = &dynCI;
    pipeCI.layout = m_pipelineLayout;
    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();
    pipeCI.pNext     = &fmtCI;
    pipeCI.renderPass = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_alphaPipeline), "Alpha Decal Pipe");

    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // Additive
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_additivePipeline), "Additive Decal Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule GroundDecalRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}

Texture* GroundDecalRenderer::getOrLoadTexture(const std::string& path) {
    if (path.empty()) return m_defaultTexture.get();
    auto it = m_textureCache.find(path);
    if (it != m_textureCache.end()) return it->second.get();

    try {
        auto tex = std::make_unique<Texture>(m_device, path);
        Texture* ptr = tex.get();
        m_textureCache[path] = std::move(tex);
        return ptr;
    } catch (const std::exception& e) {
        spdlog::warn("Failed to load decal texture: {} - {}", path, e.what());
        return m_defaultTexture.get();
    }
}

VkDescriptorSet GroundDecalRenderer::getOrCreateDescriptorSet(Texture* tex) {
    uint64_t key = reinterpret_cast<uint64_t>(tex->getImageView());

    auto it = m_descSets.find(key);
    if (it != m_descSets.end()) return it->second;

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;

    VkDescriptorSet ds;
    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &ds), "Alloc Decal Desc Set");

    // Binding 0: decal texture
    VkDescriptorImageInfo decalImg{};
    decalImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    decalImg.imageView   = tex->getImageView();
    decalImg.sampler     = tex->getSampler();

    // Binding 1: FoW texture (fall back to default white if not yet set)
    VkDescriptorImageInfo fowImg{};
    fowImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fowImg.imageView   = (m_fowView   != VK_NULL_HANDLE) ? m_fowView   : m_defaultTexture->getImageView();
    fowImg.sampler     = (m_fowSampler!= VK_NULL_HANDLE) ? m_fowSampler: m_defaultTexture->getSampler();

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = ds;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &decalImg;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = ds;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &fowImg;

    vkUpdateDescriptorSets(m_device.getDevice(), 2, writes, 0, nullptr);
    m_descSets[key] = ds;
    return ds;
}

void GroundDecalRenderer::setFogOfWar(VkImageView fowView, VkSampler fowSampler,
                                      glm::vec2 fowMapMin, glm::vec2 fowMapMax) {
    m_fowView    = fowView;
    m_fowSampler = fowSampler;
    m_fowMapMin  = fowMapMin;
    m_fowMapMax  = fowMapMax;
    // Clear cached descriptor sets so they're rebuilt with the correct FoW binding
    if (!m_descSets.empty()) {
        vkDeviceWaitIdle(m_device.getDevice());
        std::vector<VkDescriptorSet> sets;
        sets.reserve(m_descSets.size());
        for (auto& kv : m_descSets) sets.push_back(kv.second);
        vkFreeDescriptorSets(m_device.getDevice(), m_descPool,
                             static_cast<uint32_t>(sets.size()), sets.data());
        m_descSets.clear();
    }
}

// ─── DistortionRenderer ─────────────────────────────────────────────────────

DistortionRenderer::DistortionRenderer(const Device& device, const RenderFormats& formats, VkImageView sceneColorCopy, VkSampler sampler)
    : m_device(device), m_formats(formats), m_sceneColorCopy(sceneColorCopy), m_sampler(sampler) 
{
    m_sphereMesh = std::make_unique<Model>(Model::createSphere(device, device.getAllocator(), 16, 32));
    createDescriptorSet();
    createPipeline();
}

DistortionRenderer::~DistortionRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

void DistortionRenderer::registerDef(const DistortionDef& def) {
    m_defs[def.id] = def;
}

uint32_t DistortionRenderer::spawn(const std::string& defId, glm::vec3 position) {
    auto it = m_defs.find(defId);
    if (it == m_defs.end()) {
        spdlog::warn("DistortionRenderer: unknown def '{}'", defId);
        return 0;
    }
    DistortionInstance inst;
    inst.handle = m_nextHandle++;
    inst.def = &it->second;
    inst.position = position;
    m_active.push_back(inst);
    return inst.handle;
}

void DistortionRenderer::update(float dt) {
    for (auto it = m_active.begin(); it != m_active.end(); ) {
        it->elapsed += dt;
        if (it->elapsed >= it->def->duration) {
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
}

void DistortionRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime, uint32_t width, uint32_t height) {
    if (m_active.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    for (const auto& inst : m_active) {
        // mesh_effect.vert reads 128-byte MeshEffectPC; pack DistortionPC fields
        // into the first 96 bytes and zero-fill the remaining 32 for the vertex
        // shader's posRot/dir fields.
        struct {
            DistortionPC base;
            char pad[128 - sizeof(DistortionPC)];
        } pcFull{};
        pcFull.base.viewProj = viewProj;
        pcFull.base.center = inst.position;
        pcFull.base.radius = inst.def->radius;
        pcFull.base.strength = inst.def->strength;
        pcFull.base.elapsed = inst.elapsed;
        pcFull.base.screenSize = glm::vec2(static_cast<float>(width), static_cast<float>(height));

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, &pcFull);
        m_sphereMesh->draw(cmd);
    }
}

void DistortionRenderer::updateDescriptorSet(VkImageView sceneColorCopy) {
    m_sceneColorCopy = sceneColorCopy;
    
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = m_sceneColorCopy;
    imgInfo.sampler = m_sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void DistortionRenderer::createDescriptorSet() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Distort Layout");

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device.getDevice(), &poolCI, nullptr, &m_descPool), "Create Distort Pool");

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &m_descSet), "Alloc Distort Set");

    updateDescriptorSet(m_sceneColorCopy);
}

void DistortionRenderer::createPipeline() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // mesh_effect.vert declares 128-byte MeshEffectPC; distortion.frag uses 96-byte
    // DistortionPC.  Pipeline range must cover the larger of the two.
    pcRange.size = 128;

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Distort Pipe Layout");

    // We can reuse mesh_effect.vert for the vertex shader if we want, or create a specific one.
    // Spec says shaders/distortion.frag. Let's use mesh_effect.vert for vertices.
    auto vertCode = readFile(std::string(SHADER_DIR) + "mesh_effect.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "distortion.frag.spv");

    VkShaderModule vertMod = createShaderModule(vertCode);
    VkShaderModule fragMod = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    auto vertBinding = Vertex::getBindingDescription();
    auto vertAttrs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    viCI.vertexBindingDescriptionCount = 1;
    viCI.pVertexBindingDescriptions = &vertBinding;
    viCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertAttrs.size());
    viCI.pVertexAttributeDescriptions = vertAttrs.data();

    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1; vpCI.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsCI.polygonMode = VK_POLYGON_MODE_FILL; rsCI.lineWidth = 1.0f; rsCI.cullMode = VK_CULL_MODE_BACK_BIT;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable = VK_TRUE; dsCI.depthWriteEnable = VK_FALSE; dsCI.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // We want to OVERWRITE with distorted sample
    blend.colorBlendOp = VK_BLEND_OP_ADD;

    // Second attachment (charDepth) declared with write-mask=0; distortion only writes to color.
    VkPipelineColorBlendAttachmentState distBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments    = distBlends;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2; dynCI.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeCI.stageCount = 2;
    pipeCI.pStages = stages;
    pipeCI.pVertexInputState = &viCI;
    pipeCI.pInputAssemblyState = &iaCI;
    pipeCI.pViewportState = &vpCI;
    pipeCI.pRasterizationState = &rsCI;
    pipeCI.pMultisampleState = &msCI;
    pipeCI.pDepthStencilState = &dsCI;
    pipeCI.pColorBlendState = &cbCI;
    pipeCI.pDynamicState = &dynCI;
    pipeCI.layout = m_pipelineLayout;
    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();
    pipeCI.pNext     = &fmtCI;
    pipeCI.renderPass = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline), "Create Distort Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule DistortionRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module), "Failed Distort shader");
    return module;
}

// ─── OutlineRenderer ────────────────────────────────────────────────────────

// ── Outline vertex input: same layout as the skinned pipeline ────────────────
static void buildSkinnedVertexInput(
    VkPipelineVertexInputStateCreateInfo&        viCI,
    std::array<VkVertexInputBindingDescription, 2>& bindings,
    std::vector<VkVertexInputAttributeDescription>&  attrs)
{
    auto skinnedBind  = SkinnedVertex::getBindingDescription();
    auto skinnedAttrs = SkinnedVertex::getAttributeDescriptions();
    auto instBind     = InstanceData::getBindingDescription();
    auto instAttrs    = InstanceData::getAttributeDescriptions();
    instBind.binding  = 1;
    for (auto& a : instAttrs) { a.binding = 1; a.location += 2; }

    bindings[0] = skinnedBind;
    bindings[1] = instBind;

    attrs.insert(attrs.end(), skinnedAttrs.begin(), skinnedAttrs.end());
    attrs.insert(attrs.end(), instAttrs.begin(), instAttrs.end());

    viCI.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viCI.vertexBindingDescriptionCount   = 2;
    viCI.pVertexBindingDescriptions      = bindings.data();
    viCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    viCI.pVertexAttributeDescriptions    = attrs.data();
}

// ── Init / Destroy ────────────────────────────────────────────────────────────
void OutlineRenderer::init(const Device& device,
                           const RenderFormats& formats,
                           VkDescriptorSetLayout mainLayout) {
    m_device = &device;
    createPipelineLayout(mainLayout);
    createPipelines(formats);
}

void OutlineRenderer::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();
    vkDeviceWaitIdle(dev);
    if (m_outlineDrawPipeline)  { vkDestroyPipeline(dev, m_outlineDrawPipeline,  nullptr); m_outlineDrawPipeline  = VK_NULL_HANDLE; }
    if (m_stencilWritePipeline) { vkDestroyPipeline(dev, m_stencilWritePipeline, nullptr); m_stencilWritePipeline = VK_NULL_HANDLE; }
    if (m_outlineLayout)        { vkDestroyPipelineLayout(dev, m_outlineLayout,  nullptr); m_outlineLayout        = VK_NULL_HANDLE; }
    m_device = nullptr;
}

// ── Pipeline layout ───────────────────────────────────────────────────────────
void OutlineRenderer::createPipelineLayout(VkDescriptorSetLayout mainLayout) {
    // One push constant range covering both vertex and fragment stages.
    // Vertex reads  pc.boneBaseIndex (offset 0) and pc.outlineScale (offset 4).
    // Fragment reads pc.outlineColor (offset 16) via the inOutlineColor varying.
    // The stencil-write pipeline only pushes boneBaseIndex at offset 0 into its
    // own PC block (skinned.vert declares boneBaseIndex at offset 0 in its PC),
    // but the layout range is large enough to satisfy Vulkan validation.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(OutlinePC); // 32 bytes

    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &mainLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(m_device->getDevice(), &lci, nullptr, &m_outlineLayout),
             "Create outline pipeline layout");
}

// ── Pipeline creation ─────────────────────────────────────────────────────────
void OutlineRenderer::createPipelines(const RenderFormats& formats) {
    VkDevice dev = m_device->getDevice();

    // ── Shared: vertex input (skinned mesh + per-instance data) ──────────────
    std::array<VkVertexInputBindingDescription, 2> bindings{};
    std::vector<VkVertexInputAttributeDescription>  attrs;
    VkPipelineVertexInputStateCreateInfo viCI{};
    buildSkinnedVertexInput(viCI, bindings, attrs);

    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1;
    vpCI.scissorCount  = 1;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2;
    dynCI.pDynamicStates    = dynStates;

    // ── Both color attachments masked off (no color writes in either pass) ────
    // Stencil write:  writes stencil only
    // Outline draw:   will be overridden below for pass 2
    VkPipelineColorBlendAttachmentState blendOff{};
    blendOff.colorWriteMask = 0; // discard

    // ── Pass 1: stencil write pipeline ───────────────────────────────────────
    {
        auto vertCode = readFile(std::string(SHADER_DIR) + "skinned.vert.spv");
        auto fragCode = readFile(std::string(SHADER_DIR) + "outline_stencil.frag.spv");
        VkShaderModule vertMod = makeShaderModule(dev, vertCode);
        VkShaderModule fragMod = makeShaderModule(dev, fragCode);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName  = "main";

        VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rsCI.polygonMode = VK_POLYGON_MODE_FILL;
        rsCI.cullMode    = VK_CULL_MODE_BACK_BIT;
        rsCI.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rsCI.lineWidth   = 1.0f;

        // Stencil: always pass, write ref=1
        VkStencilOpState stencilOp{};
        stencilOp.failOp      = VK_STENCIL_OP_KEEP;
        stencilOp.passOp      = VK_STENCIL_OP_REPLACE;
        stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
        stencilOp.compareOp   = VK_COMPARE_OP_ALWAYS;
        stencilOp.compareMask = 0xFF;
        stencilOp.writeMask   = 0xFF;
        stencilOp.reference   = 1;

        VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dsCI.depthTestEnable       = VK_TRUE;
        dsCI.depthWriteEnable      = VK_TRUE;
        dsCI.depthCompareOp        = VK_COMPARE_OP_GREATER;        dsCI.stencilTestEnable     = VK_TRUE;
        dsCI.front                 = stencilOp;
        dsCI.back                  = stencilOp;

        // Disable color writes to BOTH attachments (stencil-only pass)
        std::array<VkPipelineColorBlendAttachmentState, 2> blends{ blendOff, blendOff };
        VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = blends.data();

        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &viCI;
        pci.pInputAssemblyState = &iaCI;
        pci.pViewportState      = &vpCI;
        pci.pRasterizationState = &rsCI;
        pci.pMultisampleState   = &msCI;
        pci.pDepthStencilState  = &dsCI;
        pci.pColorBlendState    = &cbCI;
        pci.pDynamicState       = &dynCI;
        VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
        pci.pNext               = &fmtCI;
        pci.layout              = m_outlineLayout;
        pci.renderPass          = VK_NULL_HANDLE;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_stencilWritePipeline),
                 "Create stencil-write pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
        vkDestroyShaderModule(dev, fragMod, nullptr);
    }

    // ── Pass 2: outline draw pipeline ─────────────────────────────────────────
    {
        auto vertCode = readFile(std::string(SHADER_DIR) + "outline.vert.spv");
        auto fragCode = readFile(std::string(SHADER_DIR) + "outline.frag.spv");
        VkShaderModule vertMod = makeShaderModule(dev, vertCode);
        VkShaderModule fragMod = makeShaderModule(dev, fragCode);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName  = "main";

        // Front-face culling: renders the inflated back-face shell visible only
        // where the original silhouette doesn't cover (stencil NOT_EQUAL).
        VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rsCI.polygonMode = VK_POLYGON_MODE_FILL;
        rsCI.cullMode    = VK_CULL_MODE_FRONT_BIT;
        rsCI.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rsCI.lineWidth   = 1.0f;

        // Stencil: draw only where NOT covered by the silhouette (ref=1 was set above)
        VkStencilOpState stencilOp{};
        stencilOp.failOp      = VK_STENCIL_OP_KEEP;
        stencilOp.passOp      = VK_STENCIL_OP_KEEP;
        stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
        stencilOp.compareOp   = VK_COMPARE_OP_NOT_EQUAL;
        stencilOp.compareMask = 0xFF;
        stencilOp.writeMask   = 0x00;
        stencilOp.reference   = 1;

        VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dsCI.depthTestEnable       = VK_TRUE;
        dsCI.depthWriteEnable      = VK_FALSE; // outline doesn't occlude scene
        dsCI.depthCompareOp        = VK_COMPARE_OP_GREATER_OR_EQUAL;        dsCI.stencilTestEnable     = VK_TRUE;
        dsCI.front                 = stencilOp;
        dsCI.back                  = stencilOp;

        // Attachment 0: full RGBA write for the outline color
        VkPipelineColorBlendAttachmentState blendOn{};
        blendOn.blendEnable    = VK_FALSE;
        blendOn.colorWriteMask = 0xF;

        // Attachment 1 (charDepth): no write — outline shell shouldn't contaminate it
        std::array<VkPipelineColorBlendAttachmentState, 2> blends{ blendOn, blendOff };
        VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = blends.data();

        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &viCI;
        pci.pInputAssemblyState = &iaCI;
        pci.pViewportState      = &vpCI;
        pci.pRasterizationState = &rsCI;
        pci.pMultisampleState   = &msCI;
        pci.pDepthStencilState  = &dsCI;
        pci.pColorBlendState    = &cbCI;
        pci.pDynamicState       = &dynCI;
        VkPipelineRenderingCreateInfo fmtCI2 = formats.pipelineRenderingCI();
        pci.pNext               = &fmtCI2;
        pci.layout              = m_outlineLayout;
        pci.renderPass          = VK_NULL_HANDLE;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_outlineDrawPipeline),
                 "Create outline draw pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
        vkDestroyShaderModule(dev, fragMod, nullptr);
    }
}

// ── Render ────────────────────────────────────────────────────────────────────
void OutlineRenderer::renderOutline(VkCommandBuffer cmd,
                                    VkDescriptorSet ds,
                                    VkBuffer        instBuf,
                                    VkDeviceSize    instOffset,
                                    uint32_t        boneBase,
                                    const StaticSkinnedMesh& mesh,
                                    float           outlineScale,
                                    const glm::vec4& outlineColor) {
    // ── Pass 1: write stencil=1 into the character silhouette ────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_stencilWritePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_outlineLayout, 0, 1, &ds, 0, nullptr);
    vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &instOffset);
    // skinned.vert reads boneBaseIndex at offset 0 in its own PC block
    vkCmdPushConstants(cmd, m_outlineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t), &boneBase);
    mesh.bind(cmd);
    mesh.draw(cmd);

    // ── Pass 2: draw inflated shell where stencil != 1 ───────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlineDrawPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_outlineLayout, 0, 1, &ds, 0, nullptr);
    vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &instOffset);

    OutlinePC pc{};
    pc.boneBaseIndex = boneBase;
    pc.outlineScale  = outlineScale;
    pc.outlineColor  = outlineColor;
    vkCmdPushConstants(cmd, m_outlineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(OutlinePC), &pc);
    mesh.bind(cmd);
    mesh.draw(cmd);
}

// ─── WaterRenderer ──────────────────────────────────────────────────────────

#ifndef SHADER_DIR
#  define SHADER_DIR ""
#endif

// ── Grid mesh generation ──────────────────────────────────────────────────────

static constexpr uint32_t GRID = 64;

void WaterRenderer::createMesh() {
    const uint32_t vertCount  = (GRID + 1) * (GRID + 1);
    const uint32_t indexCount = GRID * GRID * 6;

    std::vector<WaterVertex> verts;
    verts.reserve(vertCount);

    for (uint32_t z = 0; z <= GRID; ++z) {
        for (uint32_t x = 0; x <= GRID; ++x) {
            WaterVertex v{};
            v.position = { static_cast<float>(x) / GRID, 0.0f,
                           static_cast<float>(z) / GRID };
            v.uv       = { static_cast<float>(x) / GRID,
                           static_cast<float>(z) / GRID };
            verts.push_back(v);
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(indexCount);
    for (uint32_t z = 0; z < GRID; ++z) {
        for (uint32_t x = 0; x < GRID; ++x) {
            uint32_t tl = z * (GRID + 1) + x;
            uint32_t tr = tl + 1;
            uint32_t bl = tl + (GRID + 1);
            uint32_t br = bl + 1;
            indices.insert(indices.end(), { tl, bl, tr, tr, bl, br });
        }
    }

    m_indexCount = static_cast<uint32_t>(indices.size());

    m_vertexBuffer = Buffer::createDeviceLocal(*m_device, m_device->getAllocator(),
        verts.data(), verts.size() * sizeof(WaterVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_indexBuffer = Buffer::createDeviceLocal(*m_device, m_device->getAllocator(),
        indices.data(), indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    spdlog::info("WaterRenderer: grid {}x{} ({} verts, {} indices)", GRID, GRID,
                 vertCount, indexCount);
}

// ── Procedural textures ───────────────────────────────────────────────────────

// Encode R,G,B floats [0,1] into a little-endian RGBA uint32_t
// (as expected by VK_FORMAT_R8G8B8A8_UNORM).
static inline uint32_t packRGBA(float r, float g, float b, float a = 1.0f) {
    auto ub = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f) & 0xFF; };
    return ub(r) | (ub(g) << 8) | (ub(b) << 16) | (ub(a) << 24);
}

void WaterRenderer::createTextures(const Device& device,
                                   BindlessDescriptors& bindless) {
    // ── 1. Water surface normal map (128×128) ─────────────────────────────
    // Two overlapping sine waves to simulate wind-driven ripples.
    {
        constexpr uint32_t W = 128, H = 128;
        std::vector<uint32_t> pixels(W * H);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float u = static_cast<float>(x) / W;
                float v = static_cast<float>(y) / H;
                // Two-frequency ripple
                float nx = std::sin(u * glm::two_pi<float>() * 4.0f
                                    + std::cos(v * glm::two_pi<float>() * 2.0f)) * 0.35f;
                float ny = std::sin(v * glm::two_pi<float>() * 4.0f
                                    + std::sin(u * glm::two_pi<float>() * 3.0f)) * 0.35f;
                float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                pixels[y * W + x] = packRGBA(nx * 0.5f + 0.5f,
                                              ny * 0.5f + 0.5f,
                                              nz * 0.5f + 0.5f);
            }
        }
        m_normalMapTex = Texture::createFromPixels(device, pixels.data(),
                                                   W, H, VK_FORMAT_R8G8B8A8_UNORM);
    }

    // ── 2. Flow map (64×64) ───────────────────────────────────────────────
    // A mostly eastward (positive U) flow with gentle swirling variation.
    {
        constexpr uint32_t W = 64, H = 64;
        std::vector<uint32_t> pixels(W * H);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float u = static_cast<float>(x) / W;
                float v = static_cast<float>(y) / H;
                float fx = 0.60f + 0.12f * std::sin(v * glm::two_pi<float>() * 1.5f);
                float fy = 0.50f + 0.08f * std::cos(u * glm::two_pi<float>() * 2.0f);
                pixels[y * W + x] = packRGBA(fx, fy, 0.5f);
            }
        }
        m_flowMapTex = Texture::createFromPixels(device, pixels.data(),
                                                 W, H, VK_FORMAT_R8G8B8A8_UNORM);
    }

    // ── 3. Foam / noise texture (64×64) ──────────────────────────────────
    // Value-noise-like pattern mixing two sine octaves for organic foam patches.
    {
        constexpr uint32_t W = 64, H = 64;
        std::vector<uint32_t> pixels(W * H);
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                float u = static_cast<float>(x) / W;
                float v = static_cast<float>(y) / H;
                float n  = std::sin(u * glm::two_pi<float>() * 5.0f
                                    + std::cos(v * glm::two_pi<float>() * 7.0f)) * 0.5f + 0.5f;
                float n2 = std::sin(u * glm::two_pi<float>() * 11.0f
                                    + std::cos(v * glm::two_pi<float>() * 3.0f)) * 0.5f + 0.5f;
                float foam = n * 0.65f + n2 * 0.35f;
                pixels[y * W + x] = packRGBA(foam, foam, foam);
            }
        }
        m_foamTex = Texture::createFromPixels(device, pixels.data(),
                                              W, H, VK_FORMAT_R8G8B8A8_SRGB);
    }

    // ── Register in the bindless array ────────────────────────────────────
    m_normalMapIdx = static_cast<int>(bindless.registerTexture(
        m_normalMapTex.getImageView(), m_normalMapTex.getSampler()));
    m_flowMapIdx   = static_cast<int>(bindless.registerTexture(
        m_flowMapTex.getImageView(), m_flowMapTex.getSampler()));
    m_foamTexIdx   = static_cast<int>(bindless.registerTexture(
        m_foamTex.getImageView(), m_foamTex.getSampler()));

    spdlog::info("WaterRenderer: textures at bindless slots {},{},{}", m_normalMapIdx, m_flowMapIdx, m_foamTexIdx);
}

// ── Pipeline ──────────────────────────────────────────────────────────────────

void WaterRenderer::createPipeline(const RenderFormats& formats,
                                   VkDescriptorSetLayout mainLayout,
                                   VkDescriptorSetLayout bindlessLayout) {
    VkDevice dev = m_device->getDevice();


    VkShaderModule vert = makeShaderModule(dev, readFile(std::string(SHADER_DIR) + "water.vert.spv"));
    VkShaderModule frag = makeShaderModule(dev, readFile(std::string(SHADER_DIR) + "water.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_VERTEX_BIT, vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                  VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    // WaterVertex layout: vec3 position (location=0), vec2 uv (location=1)
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(WaterVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(WaterVertex, position);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(WaterVertex, uv);

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_BACK_BIT;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test ON (reads scene depth), depth write OFF (transparent surface)
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER;

    // Attachment 0 (HDR color): standard alpha blend
    VkPipelineColorBlendAttachmentState blendHDR{};
    blendHDR.blendEnable         = VK_TRUE;
    blendHDR.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendHDR.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendHDR.colorBlendOp        = VK_BLEND_OP_ADD;
    blendHDR.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendHDR.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendHDR.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendHDR.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Attachment 1 (charDepth): no writes from water
    VkPipelineColorBlendAttachmentState blendDepth{};
    blendDepth.colorWriteMask = 0;

    VkPipelineColorBlendAttachmentState blends[2] = { blendHDR, blendDepth };
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2;
    cb.pAttachments    = blends;

    // Push constant: WaterPC (92 bytes), vertex + fragment
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size       = sizeof(WaterPC);

    VkDescriptorSetLayout setLayouts[2] = { mainLayout, bindlessLayout };
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_pipelineLayout), "water pipeline layout");

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vps;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    pci.pNext               = &fmtCI;
    pci.layout              = m_pipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline),
             "water pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    spdlog::info("WaterRenderer: pipeline created");
}

// ── Public interface ──────────────────────────────────────────────────────────

void WaterRenderer::init(const Device& device,
                         const RenderFormats& formats,
                         VkDescriptorSetLayout mainLayout,
                         VkDescriptorSetLayout bindlessLayout,
                         BindlessDescriptors& bindless) {
    m_device = &device;
    createMesh();
    createTextures(device, bindless);
    createPipeline(formats, mainLayout, bindlessLayout);
}

void WaterRenderer::render(VkCommandBuffer cmd, VkDescriptorSet mainSet,
                           VkDescriptorSet bindlessSet,
                           float time, const glm::mat4& model) {
    WaterPC pc{};
    pc.model               = model;
    pc.time                = time;
    pc.flowSpeed           = flowSpeed;
    pc.distortionStrength  = distortionStrength;
    pc.foamScale           = foamScale;
    pc.normalMapIdx        = m_normalMapIdx;
    pc.flowMapIdx          = m_flowMapIdx;
    pc.foamTexIdx          = m_foamTexIdx;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkDescriptorSet sets[2] = { mainSet, bindlessSet };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WaterPC), &pc);

    VkBuffer     vb  = m_vertexBuffer.getBuffer();
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

void WaterRenderer::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);             m_pipeline       = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }

    m_normalMapTex = Texture{};
    m_flowMapTex   = Texture{};
    m_foamTex      = Texture{};
    m_vertexBuffer.destroy();
    m_indexBuffer.destroy();
    m_device = nullptr;
}

} // namespace glory
