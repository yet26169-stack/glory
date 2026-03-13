#include "renderer/ConeAbilityRenderer.h"
#include "renderer/Device.h"
#include "renderer/Buffer.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace glory {

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
VkPipeline ConeAbilityRenderer::createPipeline(VkRenderPass        renderPass,
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
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable         = VK_TRUE;
    cbA.srcColorBlendFactor = srcFactor;
    cbA.dstColorBlendFactor = dstFactor;
    cbA.colorBlendOp        = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.alphaBlendOp        = VK_BLEND_OP_ADD;
    cbA.colorWriteMask      = 0xF;

    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &cbA;

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
    gpCI.renderPass          = renderPass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline);

    vkDestroyShaderModule(m_dev, fmod, nullptr);
    vkDestroyShaderModule(m_dev, vmod, nullptr);

    return pipeline;
}

// ── init ──────────────────────────────────────────────────────────────────────
void ConeAbilityRenderer::init(const Device& device, VkRenderPass renderPass) {
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
    m_energyPipeline = createPipeline(renderPass,
        sd + "cone_ability.vert.spv", sd + "cone_energy.frag.spv",
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);

    // Pass 2: Surface grid overlay (both faces, additive)
    m_gridPipeline = createPipeline(renderPass,
        sd + "cone_ability.vert.spv", sd + "cone_grid.frag.spv",
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE);

    // Pass 3: Lightning arcs (line strip, fully additive)
    m_lightningPipeline = createPipeline(renderPass,
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

} // namespace glory
