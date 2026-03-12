#include "renderer/ShieldBubbleRenderer.h"
#include "renderer/Device.h"
#include "renderer/Buffer.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/constants.hpp>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstring>

namespace glory {

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

VkPipeline ShieldBubbleRenderer::createPipeline(VkRenderPass renderPass,
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
    ds.depthCompareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable          = VK_TRUE;
    cbA.srcColorBlendFactor  = srcColorFactor;
    cbA.dstColorBlendFactor  = dstColorFactor;
    cbA.colorBlendOp         = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor  = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor  = VK_BLEND_FACTOR_ZERO;
    cbA.alphaBlendOp         = VK_BLEND_OP_ADD;
    cbA.colorWriteMask       = 0xF;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cbA;

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
    gpCI.renderPass          = renderPass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline);

    vkDestroyShaderModule(m_dev, fmod, nullptr);
    vkDestroyShaderModule(m_dev, vmod, nullptr);

    return pipeline;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void ShieldBubbleRenderer::init(const Device& device, VkRenderPass renderPass) {
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
    m_backfacePipeline = createPipeline(renderPass,
        VK_CULL_MODE_FRONT_BIT,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    // Front-face pass: additive blend (Fresnel rim glows bright)
    m_frontfacePipeline = createPipeline(renderPass,
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

} // namespace glory
