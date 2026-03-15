#include "vfx/VFXRenderer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace glory {

// ── Constructor ──────────────────────────────────────────────────────────────
VFXRenderer::VFXRenderer(const Device& device, VkRenderPass renderPass)
    : m_device(device)
{
    createDescriptorLayoutAndPool();
    createComputePipeline();
    createCompactPipeline();
    createRenderPipeline(renderPass);

    // Default white-pixel atlas (fallback for effects without a texture)
    m_defaultAtlas = Texture::createDefault(device);

    m_effects.reserve(MAX_CONCURRENT_EMITTERS);
    spdlog::info("VFXRenderer initialised (max {} concurrent emitters)",
                 MAX_CONCURRENT_EMITTERS);
}

// ── Destructor ────────────────────────────────────────────────────────────────
VFXRenderer::~VFXRenderer() {
    VkDevice dev = m_device.getDevice();

    m_effects.clear();   // destroy ParticleSystems before releasing the pool
    m_graveyard.clear();

    m_atlasCache.clear();
    m_defaultAtlas = Texture{};

    if (m_alphaPipeline  != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_alphaPipeline,  nullptr);
    if (m_additivePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_renderLayout    != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_renderLayout, nullptr);
    if (m_compactPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_compactPipeline, nullptr);
    if (m_compactLayout   != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_compactLayout, nullptr);
    if (m_computePipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_computePipeline, nullptr);
    if (m_computeLayout   != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_computeLayout, nullptr);
    if (m_descPool        != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout      != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
}

// ── createDescriptorLayoutAndPool ────────────────────────────────────────────
void VFXRenderer::createDescriptorLayoutAndPool() {
    VkDevice dev = m_device.getDevice();

    // binding 0: SSBO (particle buffer) — compute writes, vertex reads
    // binding 1: combined image sampler (particle atlas) — fragment reads
    // binding 2: UBO (emitter parameters) — compute/vertex reads
    // binding 3: depth buffer — fragment reads (soft particles)
    // binding 4: indirect buffer — compute writes (compaction)
    VkDescriptorSetLayoutBinding bindings[5]{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 5;
    layoutCI.pBindings    = bindings;
    vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout);

    // Pool — allocate one set per possible concurrent emitter
    VkDescriptorPoolSize poolSizes[5]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = MAX_CONCURRENT_EMITTERS;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_CONCURRENT_EMITTERS;
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = MAX_CONCURRENT_EMITTERS;
    poolSizes[3].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[3].descriptorCount = MAX_CONCURRENT_EMITTERS;
    poolSizes[4].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[4].descriptorCount = MAX_CONCURRENT_EMITTERS;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = MAX_CONCURRENT_EMITTERS;
    poolCI.poolSizeCount = 5;
    poolCI.pPoolSizes    = poolSizes;
    // Allow individual free (needed when effects are destroyed out-of-order)
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);
}

// ── createComputePipeline ─────────────────────────────────────────────────────
void VFXRenderer::createComputePipeline() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(SimPC);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_computeLayout);

    auto code = readSPV(std::string(SHADER_DIR) + "particle_sim.comp.spv");
    VkShaderModule mod = createShaderModule(code);

    VkComputePipelineCreateInfo pipeCI{};
    pipeCI.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.layout       = m_computeLayout;
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = mod;
    pipeCI.stage.pName  = "main";

    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_computePipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
}

void VFXRenderer::createCompactPipeline() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(CompactPC);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_compactLayout);

    auto code = readSPV(std::string(SHADER_DIR) + "particle_compact.comp.spv");
    VkShaderModule mod = createShaderModule(code);

    VkComputePipelineCreateInfo pipeCI{};
    pipeCI.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.layout       = m_compactLayout;
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = mod;
    pipeCI.stage.pName  = "main";

    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_compactPipeline);
    vkDestroyShaderModule(dev, mod, nullptr);
}

// ── createRenderPipeline ──────────────────────────────────────────────────────
void VFXRenderer::createRenderPipeline(VkRenderPass renderPass) {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(RenderPC);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_renderLayout);

    auto vCode = readSPV(std::string(SHADER_DIR) + "particle.vert.spv");
    auto fCode = readSPV(std::string(SHADER_DIR) + "particle.frag.spv");
    VkShaderModule vMod = createShaderModule(vCode);
    VkShaderModule fMod = createShaderModule(fCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fMod;
    stages[1].pName  = "main";

    // No vertex buffer — positions come entirely from the SSBO
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    rs.cullMode    = VK_CULL_MODE_NONE;   // particles are two-sided
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test ON (particles behind opaque geometry are occluded),
    // depth write OFF (particles should not occlude each other or opaque objects).
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    // Standard alpha blending: src_alpha * src + (1 - src_alpha) * dst
    VkPipelineColorBlendAttachmentState cbA_alpha{};
    cbA_alpha.blendEnable         = VK_TRUE;
    cbA_alpha.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cbA_alpha.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbA_alpha.colorBlendOp        = VK_BLEND_OP_ADD;
    cbA_alpha.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA_alpha.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbA_alpha.alphaBlendOp        = VK_BLEND_OP_ADD;
    cbA_alpha.colorWriteMask      = 0xF;

    VkPipelineColorBlendAttachmentState vfxAlphaBlends[2] = {cbA_alpha, {}};
    VkPipelineColorBlendStateCreateInfo cb_alpha{};
    cb_alpha.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_alpha.attachmentCount = 2;
    cb_alpha.pAttachments    = vfxAlphaBlends;

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
    gpCI.pColorBlendState    = &cb_alpha;
    gpCI.pDynamicState       = &dy;
    gpCI.layout              = m_renderLayout;
    gpCI.renderPass          = renderPass;
    gpCI.subpass             = 0;

    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_alphaPipeline);

    // Additive blending: src_alpha * src + 1 * dst
    VkPipelineColorBlendAttachmentState cbA_additive = cbA_alpha;
    cbA_additive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;

    VkPipelineColorBlendAttachmentState vfxAddBlends[2] = {cbA_additive, {}};
    VkPipelineColorBlendStateCreateInfo cb_additive = cb_alpha;
    cb_additive.pAttachments = vfxAddBlends;
    
    gpCI.pColorBlendState = &cb_additive;
    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_additivePipeline);

    vkDestroyShaderModule(dev, fMod, nullptr);
    vkDestroyShaderModule(dev, vMod, nullptr);
}

// ── processQueue ──────────────────────────────────────────────────────────────
void VFXRenderer::processQueue(VFXEventQueue& queue) {
    VFXEvent ev;
    while (queue.pop(ev)) {
        switch (ev.type) {
            case VFXEventType::Spawn:   handleSpawn(ev);   break;
            case VFXEventType::Destroy: handleDestroy(ev); break;
            case VFXEventType::Move:    handleMove(ev);    break;
        }
    }
}

// ── update ────────────────────────────────────────────────────────────────────
void VFXRenderer::update(float dt) {
    m_currentDt = dt;
    ++m_frameCount;

    for (auto& ps : m_effects) ps->update(dt);

    // Move dead effects to graveyard — DO NOT destroy immediately.
    auto it = m_effects.begin();
    while (it != m_effects.end()) {
        if (!(*it)->isAlive() || (*it)->handle() == 0) {
            m_graveyard.push_back({ m_frameCount + GRAVEYARD_DELAY, std::move(*it) });
            it = m_effects.erase(it);
        } else {
            ++it;
        }
    }

    // Flush graveyard: only destroy entries whose GPU-safe frame has passed
    m_graveyard.erase(
        std::remove_if(m_graveyard.begin(), m_graveyard.end(),
                       [this](const GraveyardEntry& g){ return g.killFrame <= m_frameCount; }),
        m_graveyard.end());
}

// ── dispatchCompute ───────────────────────────────────────────────────────────
void VFXRenderer::dispatchCompute(VkCommandBuffer cmd) {
    if (m_effects.empty()) return;

    for (auto& ps : m_effects) {
        // 1. Clear indirect buffer (vertexCount = 0, instanceCount = 1, others 0)
        VkDrawIndirectCommand clearCmd{};
        clearCmd.instanceCount = 1;
        vkCmdUpdateBuffer(cmd, ps->indirectBuffer(), 0, sizeof(VkDrawIndirectCommand), &clearCmd);
    }

    // Barrier to ensure updateBuffer finishes before compute
    VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

    // 2. Simulation pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    for (auto& ps : m_effects) {
        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_computeLayout, 0, 1, &ds, 0, nullptr);

        SimPC pc{};
        pc.dt       = m_currentDt; 
        pc.gravity  = 4.0f;
        pc.count    = ps->maxParticles();
        pc._pad     = 0.f;

        vkCmdPushConstants(cmd, m_computeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(SimPC), &pc);

        const uint32_t groups = (ps->maxParticles() + 63) / 64;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Barrier between simulation and compaction
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

    // 3. Compaction pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compactPipeline);
    for (auto& ps : m_effects) {
        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_compactLayout, 0, 1, &ds, 0, nullptr);

        CompactPC cpc;
        cpc.totalCount = ps->maxParticles();
        vkCmdPushConstants(cmd, m_compactLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CompactPC), &cpc);

        const uint32_t groups = (ps->maxParticles() + 63) / 64;
        vkCmdDispatch(cmd, groups, 1, 1);
    }
}

// ── barrierComputeToGraphics ──────────────────────────────────────────────────
void VFXRenderer::barrierComputeToGraphics(VkCommandBuffer cmd) {
    if (m_effects.empty()) return;

    // One global memory barrier: wait for all compute SSBO writes to complete
    // before the vertex stage reads the SSBOs.
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0,
                         1, &barrier,
                         0, nullptr,
                         0, nullptr);
}

// ── render ────────────────────────────────────────────────────────────────────
void VFXRenderer::render(VkCommandBuffer cmd,
                          const glm::mat4& viewProj,
                          const glm::vec3& camRight,
                          const glm::vec3& camUp,
                          glm::vec2 screenSize,
                          float nearPlane,
                          float farPlane)
{
    if (m_effects.empty()) return;

    RenderPC pc{};
    pc.viewProj   = viewProj;
    pc.camRight   = glm::vec4(camRight, 0.0f);
    pc.camUp      = glm::vec4(camUp,    0.0f);
    pc.screenSize = screenSize;
    pc.nearPlane  = nearPlane;
    pc.farPlane   = farPlane;

    vkCmdPushConstants(cmd, m_renderLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(RenderPC), &pc);

    // Pass 1: Alpha-blended particles
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_alphaPipeline);
    for (const auto& ps : m_effects) {
        if (ps->blendMode() != EmitterDef::BlendMode::Alpha) continue;
        
        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_renderLayout, 0, 1, &ds, 0, nullptr);
        vkCmdDrawIndirect(cmd, ps->indirectBuffer(), 0, 1, sizeof(VkDrawIndirectCommand));
    }

    // Pass 2: Additive particles
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_additivePipeline);
    for (const auto& ps : m_effects) {
        if (ps->blendMode() != EmitterDef::BlendMode::Additive) continue;
        
        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_renderLayout, 0, 1, &ds, 0, nullptr);
        vkCmdDrawIndirect(cmd, ps->indirectBuffer(), 0, 1, sizeof(VkDrawIndirectCommand));
    }
}

void VFXRenderer::setDepthBuffer(VkImageView depthView, VkSampler sampler) {
    m_depthView = depthView;
    m_depthSampler = sampler;

    // Update all active effects immediately
    for (auto& ps : m_effects) {
        ps->updateDepthBuffer(m_descLayout, m_depthView, m_depthSampler);
    }
    // Graveyard systems also need correct binding if they might still be drawn
    // (though usually we partition and move to graveyard when they are done being updated/rendered on CPU)
    for (auto& g : m_graveyard) {
        g.ps->updateDepthBuffer(m_descLayout, m_depthView, m_depthSampler);
    }
}

// ── registerEmitter ───────────────────────────────────────────────────────────
void VFXRenderer::registerEmitter(EmitterDef def) {
    const std::string id = def.id;
    m_emitterDefs[id] = std::move(def);
}

// ── loadEmitterDirectory ──────────────────────────────────────────────────────
void VFXRenderer::loadEmitterDirectory(const std::string& dirPath) {
    namespace fs = std::filesystem;
    if (!fs::exists(dirPath)) {
        spdlog::warn("VFXRenderer: emitter directory '{}' not found", dirPath);
        return;
    }

    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.path().extension() != ".json") continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        try {
            nlohmann::json j;
            f >> j;

            EmitterDef def;
            def.id              = j.value("id",             entry.path().stem().string());
            def.textureAtlas    = j.value("textureAtlas",   "");
            def.atlasFrameCount = j.value("atlasFrameCount",1u);
            def.maxParticles    = j.value("maxParticles",   256u);
            def.emitRate        = j.value("emitRate",       40.0f);
            def.burstCount      = j.value("burstCount",     0.0f);
            def.looping         = j.value("looping",        false);
            def.duration        = j.value("duration",       1.5f);
            def.lifetimeMin     = j.value("lifetimeMin",    0.8f);
            def.lifetimeMax     = j.value("lifetimeMax",    1.6f);
            def.initialSpeedMin = j.value("initialSpeedMin",2.0f);
            def.initialSpeedMax = j.value("initialSpeedMax",6.0f);
            def.sizeMin         = j.value("sizeMin",        0.2f);
            def.sizeMax         = j.value("sizeMax",        0.6f);
            def.sizeEnd         = j.value("sizeEnd",        -1.0f);
            def.rotationSpeedMin= j.value("rotationSpeedMin", 0.0f);
            def.rotationSpeedMax= j.value("rotationSpeedMax", 0.0f);
            def.spreadAngle     = j.value("spreadAngle",    45.0f);
            def.gravity         = j.value("gravity",        4.0f);
            def.drag            = j.value("drag",           0.0f);
            def.alphaCurve      = j.value("alphaCurve",     1.0f);
            def.windStrength    = j.value("windStrength",   0.0f);

            std::string bm = j.value("blendMode", "alpha");
            if (bm == "additive") def.blendMode = EmitterDef::BlendMode::Additive;
            else                  def.blendMode = EmitterDef::BlendMode::Alpha;

            if (j.contains("windDirection")) {
                auto& w = j["windDirection"];
                def.windDirection = {w[0].get<float>(), w[1].get<float>(), w[2].get<float>()};
            }

            if (j.contains("colorOverLifetime")) {
                for (const auto& k : j["colorOverLifetime"]) {
                    ColorKey ck;
                    ck.time  = k["time"].get<float>();
                    auto& c  = k["color"];
                    ck.color = {c[0].get<float>(), c[1].get<float>(),
                                c[2].get<float>(), c[3].get<float>()};
                    def.colorOverLifetime.push_back(ck);
                }
            }

            const std::string emitterID = def.id;
            registerEmitter(std::move(def));
            spdlog::debug("VFXRenderer: loaded emitter '{}'", emitterID);
        } catch (const std::exception& e) {
            spdlog::warn("VFXRenderer: failed to parse '{}': {}",
                         entry.path().string(), e.what());
        }
    }
}

// ── handleSpawn ───────────────────────────────────────────────────────────────
void VFXRenderer::handleSpawn(VFXEvent& ev) {
    if (m_effects.size() >= MAX_CONCURRENT_EMITTERS) {
        spdlog::warn("VFXRenderer: max concurrent emitters reached, dropping '{}'",
                     ev.effectID);
        return;
    }

    // Find the definition for this effect ID
    auto it = m_emitterDefs.find(ev.effectID);
    if (it == m_emitterDefs.end()) {
        spdlog::warn("VFXRenderer: unknown effect '{}', spawning default", ev.effectID);
        // Build a minimal fallback definition
        EmitterDef fallback;
        fallback.id          = ev.effectID;
        fallback.maxParticles= 64;
        fallback.emitRate    = 30.0f;
        fallback.duration    = 1.0f;
        m_emitterDefs[ev.effectID] = fallback;
        it = m_emitterDefs.find(ev.effectID);
    }

    const EmitterDef& def = it->second;
    Texture* atlas = getOrLoadAtlas(def.textureAtlas);

    const uint32_t handle = (ev.handle != 0) ? ev.handle : m_nextHandle++;
    ev.handle = handle;  // write-back just in case

    m_effects.push_back(std::make_unique<ParticleSystem>(m_device, def, m_descLayout, m_descPool,
                           atlas, m_depthView, m_depthSampler,
                           ev.position, ev.direction,
                           ev.scale, ev.lifetime, handle));
}

// ── handleDestroy ─────────────────────────────────────────────────────────────
void VFXRenderer::handleDestroy(const VFXEvent& ev) {
    for (auto& ps : m_effects) {
        if (ps->handle() == ev.handle) {
            ps->stop();
            return;
        }
    }
}

// ── handleMove ────────────────────────────────────────────────────────────────
void VFXRenderer::handleMove(const VFXEvent& ev) {
    for (auto& ps : m_effects) {
        if (ps->handle() == ev.handle) {
            ps->moveTo(ev.position);
            return;
        }
    }
}

// ── getOrLoadAtlas ────────────────────────────────────────────────────────────
Texture* VFXRenderer::getOrLoadAtlas(const std::string& texturePath) {
    if (texturePath.empty()) return &m_defaultAtlas;

    auto it = m_atlasCache.find(texturePath);
    if (it != m_atlasCache.end()) return it->second.get();

    const std::string fullPath = std::string(ASSET_DIR) + texturePath;
    try {
        auto tex = std::make_unique<Texture>(m_device, fullPath);
        Texture* ptr = tex.get();
        m_atlasCache[texturePath] = std::move(tex);
        return ptr;
    } catch (const std::exception& e) {
        spdlog::warn("VFXRenderer: failed to load atlas '{}': {}", fullPath, e.what());
        return &m_defaultAtlas;
    }
}

// ── Shader helpers ────────────────────────────────────────────────────────────
std::vector<char> VFXRenderer::readSPV(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("VFXRenderer: cannot open shader: " + path);
    const size_t sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

VkShaderModule VFXRenderer::createShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &mod);
    return mod;
}

} // namespace glory
