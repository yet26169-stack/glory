#include "renderer/ExplosionRenderer.h"
#include "renderer/Device.h"
#include "renderer/Buffer.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace glory {

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
VkPipeline ExplosionRenderer::createPipeline(VkRenderPass       renderPass,
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
    gpCI.renderPass          = renderPass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline);

    vkDestroyShaderModule(m_dev, fmod, nullptr);
    vkDestroyShaderModule(m_dev, vmod, nullptr);

    return pipeline;
}

// ── init ──────────────────────────────────────────────────────────────────────
void ExplosionRenderer::init(const Device& device, VkRenderPass renderPass) {
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
    m_shockwavePipeline = createPipeline(renderPass,
        sd + "explosion_disk.vert.spv",
        sd + "explosion_shockwave.frag.spv",
        VK_CULL_MODE_NONE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    // Pass 2: Fireball sphere (both faces, additive)
    m_fireballPipeline = createPipeline(renderPass,
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

} // namespace glory
