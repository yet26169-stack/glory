#include "renderer/Device.h"
#include "renderer/VkCheck.h"
#include "vfx/MeshEffectRenderer.h"
#include "vfx/TrailRenderer.h"
#include "vfx/VFXRenderer.h"
#include "core/Profiler.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace glory {

// ─── Shared shader-loading helper ───────────────────────────────────────────

static std::vector<char> readFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open file: " + filepath);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

// ═══ VFXRenderer.cpp ═══

// ── Constructor ──────────────────────────────────────────────────────────────
VFXRenderer::VFXRenderer(const Device& device, const RenderFormats& formats)
    : m_device(device)
{
    createDescriptorLayoutAndPool();
    createComputePipeline();
    createCompactPipeline();
    createSortPipeline();
    createRenderPipeline(formats);

    // Default white-pixel atlas (fallback for effects without a texture).
    // White (1,1,1,1) is the neutral multiply identity — it preserves the
    // particle's computed colour.  Magenta would kill the green channel,
    // making green / yellow / cyan VFX appear black.
    m_defaultAtlas = Texture::createWhiteDefault(device);

    m_effects.reserve(MAX_CONCURRENT_EMITTERS);
    spdlog::info("VFXRenderer initialised (max {} concurrent emitters)",
                 MAX_CONCURRENT_EMITTERS);
}

// ── Destructor ────────────────────────────────────────────────────────────────
VFXRenderer::~VFXRenderer() {
    VkDevice dev = m_device.getDevice();

    // Wait for all GPU work to finish before destroying buffers that may
    // still be referenced by in-flight descriptor sets / command buffers.
    vkDeviceWaitIdle(dev);

    // Release descriptor sets before destroying ParticleSystems (and their buffers)
    for (auto& ps : m_effects)
        ps->releaseDescriptorSet(dev, m_descPool);
    for (auto& g : m_graveyard)
        g.ps->releaseDescriptorSet(dev, m_descPool);

    m_effects.clear();
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
    if (m_sortLocalPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_sortLocalPipeline, nullptr);
    if (m_sortPipeline    != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_sortPipeline, nullptr);
    if (m_sortLayout      != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_sortLayout, nullptr);
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

// ── createSortPipeline ────────────────────────────────────────────────────────
void VFXRenderer::createSortPipeline() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(SortPC);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_sortLayout);

    auto code = readSPV(std::string(SHADER_DIR) + "particle_sort.comp.spv");
    VkShaderModule mod = createShaderModule(code);

    VkComputePipelineCreateInfo pipeCI{};
    pipeCI.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.layout       = m_sortLayout;
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = mod;
    pipeCI.stage.pName  = "main";

    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_sortPipeline);
    vkDestroyShaderModule(dev, mod, nullptr);

    // Local bitonic sort (shared memory)
    auto localCode = readSPV(std::string(SHADER_DIR) + "particle_sort_local.comp.spv");
    VkShaderModule localMod = createShaderModule(localCode);
    VkComputePipelineCreateInfo localCI{};
    localCI.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    localCI.layout       = m_sortLayout;
    localCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    localCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    localCI.stage.module = localMod;
    localCI.stage.pName  = "main";
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &localCI, nullptr, &m_sortLocalPipeline);
    vkDestroyShaderModule(dev, localMod, nullptr);
}

// ── createRenderPipeline ──────────────────────────────────────────────────────
void VFXRenderer::createRenderPipeline(const RenderFormats& formats) {
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
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    gpCI.pNext               = &fmtCI;
    gpCI.renderPass          = VK_NULL_HANDLE;

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
    GLORY_ZONE_N("VFXUpdate");
    m_currentDt = dt;
    ++m_frameCount;

    for (auto& ps : m_effects) ps->update(dt);

    // ── Sub-emitter spawning (death events → child effects) ─────────────
    // Collect death events from all active effects that have sub-emitters,
    // then spawn child effects. We batch the spawns into a temporary list
    // to avoid mutating m_effects while iterating.
    struct PendingSubSpawn {
        std::string defId;
        glm::vec3   position;
        glm::vec3   direction;
    };
    std::vector<PendingSubSpawn> subSpawns;

    static thread_local std::mt19937 subRng{ std::random_device{}() };
    static thread_local std::uniform_real_distribution<float> probDist(0.0f, 1.0f);

    for (auto& ps : m_effects) {
        const EmitterDef* def = ps->emitterDef();
        if (!def || def->subEmitters.empty()) continue;

        auto deaths = ps->scanDeaths();
        if (deaths.empty()) continue;

        for (const auto& se : def->subEmitters) {
            if (se.trigger != SubEmitterDef::Trigger::OnDeath) continue;
            if (se.emitterRef.empty()) continue;

            for (const auto& evt : deaths) {
                if (se.probability < 1.0f && probDist(subRng) > se.probability)
                    continue;

                glm::vec3 dir = (se.inheritVelocity && glm::length(evt.velocity) > 0.001f)
                                  ? glm::normalize(evt.velocity)
                                  : glm::vec3(0.0f, 1.0f, 0.0f);

                subSpawns.push_back({ se.emitterRef, evt.position, dir });
            }
        }
    }

    for (auto& ss : subSpawns) {
        VFXEvent ev;
        ev.type = VFXEventType::Spawn;
        std::strncpy(ev.effectID, ss.defId.c_str(), sizeof(ev.effectID) - 1);
        ev.effectID[sizeof(ev.effectID) - 1] = '\0';
        ev.position  = ss.position;
        ev.direction = ss.direction;
        ev.scale     = 1.0f;
        ev.lifetime  = 0.0f; // use definition default
        handleSpawn(ev);
    }

    // Move dead effects to graveyard — DO NOT destroy immediately.
    auto it = m_effects.begin();
    while (it != m_effects.end()) {
        if (!(*it)->isAlive() || (*it)->handle() == 0) {
            m_graveyard.push_back({ m_renderFrame + GRAVEYARD_DELAY, std::move(*it) });
            it = m_effects.erase(it);
        } else {
            ++it;
        }
    }

}

void VFXRenderer::flushGraveyard() {
    ++m_renderFrame;
    VkDevice dev = m_device.getDevice();
    m_graveyard.erase(
        std::remove_if(m_graveyard.begin(), m_graveyard.end(),
                       [this, dev](GraveyardEntry& g){
                           if (g.killFrame <= m_renderFrame) {
                               g.ps->releaseDescriptorSet(dev, m_descPool);
                               return true; // erase triggers ~ParticleSystem → buffer destroy
                           }
                           return false;
                       }),
        m_graveyard.end());
}

// ── dispatchCompute ───────────────────────────────────────────────────────────
void VFXRenderer::dispatchCompute(VkCommandBuffer cmd) {
    GLORY_ZONE_N("VFXCompute");
    if (m_effects.empty()) return;

    for (auto& ps : m_effects) {
        if (!ps->hasDescriptorSet()) continue;

        // 1. Clear indirect buffer (vertexCount = 0, instanceCount = 1, others 0)
        VkDrawIndirectCommand clearCmd{};
        clearCmd.instanceCount = 1;
        vkCmdUpdateBuffer(cmd, ps->indirectBuffer(), 0, sizeof(VkDrawIndirectCommand), &clearCmd);
    }

    // Barrier to ensure updateBuffer finishes before compute
    VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                            VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    // 2. Simulation pass (with LOD: skip distant emitters on some frames)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    for (auto& ps : m_effects) {
        if (!ps->hasDescriptorSet()) continue;

        // LOD: compute distance-based skip mask
        float dist = glm::length(ps->worldPosition() - m_cameraPos);
        uint32_t lodMask = 0; // every frame
        if      (dist > 80.0f) lodMask = 3; // every 4th frame
        else if (dist > 40.0f) lodMask = 1; // every 2nd frame
        if ((m_frameCount & lodMask) != 0) continue;

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

    // Barrier between simulation and sort/compaction
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    // 3. Bitonic sort pass (back-to-front for correct alpha blending)
    for (auto& ps : m_effects) {
        if (!ps->hasDescriptorSet()) continue;
        if (ps->blendMode() == EmitterDef::BlendMode::Additive) continue; // additive doesn't need sorting

        // LOD: skip sort for emitters that were also skipped in simulation
        float dist = glm::length(ps->worldPosition() - m_cameraPos);
        uint32_t lodMask = 0;
        if      (dist > 80.0f) lodMask = 3;
        else if (dist > 40.0f) lodMask = 1;
        if ((m_frameCount & lodMask) != 0) continue;

        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_sortLayout, 0, 1, &ds, 0, nullptr);
        dispatchSort(cmd, ps->maxParticles());
    }

    // Barrier between sort and compaction
    vkCmdPipelineBarrier2(cmd, &depInfo);

    // 4. Compaction pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_compactPipeline);
    for (auto& ps : m_effects) {
        if (!ps->hasDescriptorSet()) continue;

        // LOD: skip compaction for emitters that were also skipped in simulation
        float dist = glm::length(ps->worldPosition() - m_cameraPos);
        uint32_t lodMask = 0;
        if      (dist > 80.0f) lodMask = 3;
        else if (dist > 40.0f) lodMask = 1;
        if ((m_frameCount & lodMask) != 0) continue;

        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_compactLayout, 0, 1, &ds, 0, nullptr);

        CompactPC cpc;
        cpc.totalCount = ps->maxParticles();
        vkCmdPushConstants(cmd, m_compactLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CompactPC), &cpc);

        const uint32_t groups = (ps->maxParticles() + 63) / 64;
        vkCmdDispatch(cmd, groups, 1, 1);
    }

    // Barrier: compaction compute writes → draw indirect / vertex shader reads
    VkMemoryBarrier2 compactBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    compactBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    compactBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    compactBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    compactBarrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                   VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo compactDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    compactDep.memoryBarrierCount = 1;
    compactDep.pMemoryBarriers    = &compactBarrier;
    vkCmdPipelineBarrier2(cmd, &compactDep);
}

// ── dispatchSort (bitonic merge sort — shared-memory optimized) ──────────────
void VFXRenderer::dispatchSort(VkCommandBuffer cmd, uint32_t maxParticles) {
    if (maxParticles <= 16) return;  // Too few particles to notice sort order

    uint32_t n = nextPowerOf2(maxParticles);
    constexpr uint32_t BLOCK_SIZE = 128;  // must match local shader
    const uint32_t groups = (n / 2 + 63) / 64;

    // Reusable barrier between dispatches
    VkMemoryBarrier2 bar{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    bar.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bar.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bar.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bar.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &bar;

    for (uint32_t k = 2; k <= n; k <<= 1) {
        // Global passes: j >= BLOCK_SIZE need cross-workgroup barrier
        uint32_t j = k >> 1;
        for (; j >= BLOCK_SIZE; j >>= 1) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortPipeline);
            SortPC pc{};
            pc.cameraPos = glm::vec4(m_cameraPos, 0.0f);
            pc.count = n;
            pc.k     = k;
            pc.j     = j;
            pc._pad  = 0;
            vkCmdPushConstants(cmd, m_sortLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(SortPC), &pc);
            vkCmdDispatch(cmd, groups, 1, 1);
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // Local pass: all j < BLOCK_SIZE in one dispatch via shared memory
        // j is now min(k/2, BLOCK_SIZE/2) = min(k/2, 64)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortLocalPipeline);
        SortPC pc{};
        pc.cameraPos = glm::vec4(m_cameraPos, 0.0f);
        pc.count = n;
        pc.k     = k;
        pc.j     = j;      // startJ for local shader
        pc._pad  = 0;
        vkCmdPushConstants(cmd, m_sortLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(SortPC), &pc);
        vkCmdDispatch(cmd, groups, 1, 1);
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

uint32_t VFXRenderer::nextPowerOf2(uint32_t n) const {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

// ── dispatchComputeAsync ─────────────────────────────────────────────────────
void VFXRenderer::dispatchComputeAsync(VkCommandBuffer computeCmd,
                                        uint32_t srcQueueFamily,
                                        uint32_t dstQueueFamily)
{
    if (m_effects.empty()) return;

    // Record same sim+compact+sort work but on the async compute command buffer.
    // The indirect buffer clears, sim, sort, and compact are recorded identically.
    dispatchCompute(computeCmd);

    // If queue families differ, release ownership of particle buffers to graphics queue
    if (srcQueueFamily != dstQueueFamily) {
        std::vector<VkBufferMemoryBarrier2> releases;
        releases.reserve(m_effects.size() * 2);

        for (auto& ps : m_effects) {
            if (!ps->hasDescriptorSet()) continue;

            VkBufferMemoryBarrier2 bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            bar.srcStageMask       = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bar.srcAccessMask      = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bar.dstStageMask       = VK_PIPELINE_STAGE_2_NONE;
            bar.dstAccessMask      = VK_ACCESS_2_NONE;
            bar.srcQueueFamilyIndex = srcQueueFamily;
            bar.dstQueueFamilyIndex = dstQueueFamily;

            // Release SSBO
            bar.buffer = ps->ssbo();
            bar.offset = 0;
            bar.size   = VK_WHOLE_SIZE;
            releases.push_back(bar);

            // Release indirect buffer
            bar.buffer = ps->indirectBuffer();
            releases.push_back(bar);
        }

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(releases.size());
        dep.pBufferMemoryBarriers    = releases.data();
        vkCmdPipelineBarrier2(computeCmd, &dep);
    }
}

// ── acquireFromCompute ───────────────────────────────────────────────────────
void VFXRenderer::acquireFromCompute(VkCommandBuffer graphicsCmd,
                                      uint32_t srcQueueFamily,
                                      uint32_t dstQueueFamily)
{
    if (m_effects.empty()) return;
    if (srcQueueFamily == dstQueueFamily) {
        // Same queue family — just a regular memory barrier
        barrierComputeToGraphics(graphicsCmd);
        return;
    }

    std::vector<VkBufferMemoryBarrier2> acquires;
    acquires.reserve(m_effects.size() * 2);

    for (auto& ps : m_effects) {
        if (!ps->hasDescriptorSet()) continue;

        VkBufferMemoryBarrier2 bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bar.srcStageMask       = VK_PIPELINE_STAGE_2_NONE;
        bar.srcAccessMask      = VK_ACCESS_2_NONE;
        bar.dstStageMask       = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        bar.dstAccessMask      = VK_ACCESS_2_SHADER_READ_BIT |
                                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        bar.srcQueueFamilyIndex = srcQueueFamily;
        bar.dstQueueFamilyIndex = dstQueueFamily;

        // Acquire SSBO
        bar.buffer = ps->ssbo();
        bar.offset = 0;
        bar.size   = VK_WHOLE_SIZE;
        acquires.push_back(bar);

        // Acquire indirect buffer
        bar.buffer = ps->indirectBuffer();
        acquires.push_back(bar);
    }

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(acquires.size());
    dep.pBufferMemoryBarriers    = acquires.data();
    vkCmdPipelineBarrier2(graphicsCmd, &dep);
}

// ── barrierComputeToGraphics ──────────────────────────────────────────────────
// Same-queue-family barrier that synchronises both the particle SSBO (read by
// the vertex shader) and the indirect draw buffer (consumed by vkCmdDrawIndirect).
// acquireFromCompute() is the async-queue equivalent (queue-family ownership
// transfer) and must be kept in sync with the dstStageMask / dstAccessMask here.
void VFXRenderer::barrierComputeToGraphics(VkCommandBuffer cmd) {
    if (m_effects.empty()) return;

    VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);
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
    GLORY_ZONE_N("VFXRender");
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
        if (!ps->hasDescriptorSet()) continue;
        if (ps->blendMode() != EmitterDef::BlendMode::Alpha) continue;
        
        VkDescriptorSet ds = ps->descSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_renderLayout, 0, 1, &ds, 0, nullptr);
        vkCmdDrawIndirect(cmd, ps->indirectBuffer(), 0, 1, sizeof(VkDrawIndirectCommand));
    }

    // Pass 2: Additive particles
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_additivePipeline);
    for (const auto& ps : m_effects) {
        if (!ps->hasDescriptorSet()) continue;
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

void VFXRenderer::spawnDirect(VFXEvent& ev) {
    handleSpawn(ev);
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

            // Force parameters
            def.forceType     = j.value("forceType", 0u);
            def.forceStrength = j.value("forceStrength", 1.0f);
            if (j.contains("attractorPos")) {
                auto& a = j["attractorPos"];
                def.attractorPos = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
            }
            if (j.contains("forces") && j["forces"].is_array()) {
                // Multi-force: build bitmask from forces array
                uint32_t mask = 0;
                for (const auto& fj : j["forces"]) {
                    std::string ft = fj.value("type", "gravity");
                    if      (ft == "gravity")    mask |= 1u;
                    else if (ft == "radial")     mask |= 2u;
                    else if (ft == "vortex")     mask |= 4u;
                    else if (ft == "turbulence") mask |= 8u;
                }
                def.forceParams_bitmask = mask;
            } else {
                def.forceParams_bitmask = 1u << def.forceType;
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

// ═══ TrailRenderer.cpp ═══

TrailRenderer::TrailRenderer(const Device& device, const RenderFormats& formats)
    : m_device(device), m_formats(formats) 
{
    m_whiteTexture = std::make_unique<Texture>(Texture::createDefault(device));
    createDescriptorLayout();
    createPipelines();

    // Pool for trail descriptor sets
    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_ACTIVE_TRAILS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_ACTIVE_TRAILS }
    };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets = MAX_ACTIVE_TRAILS;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VK_CHECK(vkCreateDescriptorPool(device.getDevice(), &poolCI, nullptr, &m_descPool), "Create Trail desc pool");
}

TrailRenderer::~TrailRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);

    // Free descriptor sets from active trails before destroying pool
    for (auto& inst : m_activeTrails) {
        if (inst->descSet != VK_NULL_HANDLE)
            vkFreeDescriptorSets(dev, m_descPool, 1, &inst->descSet);
    }
    // Free descriptor sets from graveyard entries
    for (auto& g : m_graveyard) {
        if (g.inst->descSet != VK_NULL_HANDLE)
            vkFreeDescriptorSets(dev, m_descPool, 1, &g.inst->descSet);
    }

    m_activeTrails.clear();
    m_graveyard.clear();

    if (m_alphaPipeline) vkDestroyPipeline(dev, m_alphaPipeline, nullptr);
    if (m_additivePipeline) vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

void TrailRenderer::registerTrail(const TrailDef& def) {
    m_defs[def.id] = def;
}

uint32_t TrailRenderer::spawn(const std::string& trailDefId, glm::vec3 startPos) {
    auto it = m_defs.find(trailDefId);
    if (it == m_defs.end()) {
        spdlog::warn("TrailRenderer: unknown def '{}'", trailDefId);
        return INVALID_TRAIL_HANDLE;
    }

    auto inst = std::make_unique<TrailInstance>();
    inst->handle = m_nextHandle++;
    inst->def = &it->second;
    inst->lastHeadPos = startPos;
    inst->texture = m_whiteTexture.get(); // fallback

    // Create SSBO
    inst->ssbo = Buffer(m_device.getAllocator(),
                        sizeof(GpuTrailPoint) * MAX_TRAIL_POINTS,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    VkResult allocResult = vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &inst->descSet);
    if (allocResult == VK_ERROR_OUT_OF_POOL_MEMORY) {
        spdlog::warn("TrailRenderer: descriptor pool exhausted, dropping trail spawn");
        return INVALID_TRAIL_HANDLE;
    }
    VK_CHECK(allocResult, "Alloc Trail desc set");

    // Write descriptor set
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = inst->ssbo.getBuffer();
    bufInfo.offset = 0;
    bufInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = inst->texture->getImageView();
    imgInfo.sampler = inst->texture->getSampler();

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = inst->descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = inst->descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 2, writes, 0, nullptr);

    // Initial point
    GpuTrailPoint p{};
    p.posWidth = glm::vec4(startPos, inst->def->widthStart);
    p.colorAge = inst->def->colorStart;
    p.colorAge.a = 0.0f;
    inst->points.push_back(p);

    m_activeTrails.push_back(std::move(inst));
    return m_nextHandle - 1;
}

void TrailRenderer::updateHead(uint32_t handle, glm::vec3 newHeadPos) {
    for (auto& inst : m_activeTrails) {
        if (inst->handle == handle) {
            inst->lastHeadPos = newHeadPos;
            if (!inst->points.empty()) {
                inst->points.front().posWidth = glm::vec4(newHeadPos, inst->def->widthStart);
            }
            return;
        }
    }
}

void TrailRenderer::detach(uint32_t handle) {
    for (auto& inst : m_activeTrails) {
        if (inst->handle == handle) {
            inst->detached = true;
            return;
        }
    }
}

void TrailRenderer::update(float dt) {
    ++m_frameCount;

    for (auto it = m_activeTrails.begin(); it != m_activeTrails.end(); ) {
        auto& inst = **it;
        
        // Update ages
        for (auto& p : inst.points) {
            p.colorAge.a += inst.def->fadeSpeed * dt;
        }

        // Remove old points
        while (!inst.points.empty() && inst.points.back().colorAge.a >= 1.0f) {
            inst.points.pop_back();
        }

        // Emit new points if not detached
        if (!inst.detached) {
            inst.emitAccum += dt;
            if (inst.emitAccum >= inst.def->emitInterval) {
                inst.emitAccum = 0.0f;
                GpuTrailPoint p{};
                p.posWidth = glm::vec4(inst.lastHeadPos, inst.def->widthStart);
                p.colorAge = inst.def->colorStart;
                p.colorAge.a = 0.0f;
                inst.points.push_front(p);
                
                if (inst.points.size() > MAX_TRAIL_POINTS) {
                    inst.points.pop_back();
                }
            }
        }

        // Update widths and colors for all points (linear interpolation based on age)
        for (auto& p : inst.points) {
            float t = p.colorAge.a;
            p.posWidth.w = glm::mix(inst.def->widthStart, inst.def->widthEnd, t);
            glm::vec4 c = glm::mix(inst.def->colorStart, inst.def->colorEnd, t);
            p.colorAge.r = c.r; p.colorAge.g = c.g; p.colorAge.b = c.b;
        }

        if (inst.points.empty() && inst.detached) {
            // Move to graveyard instead of immediate destruction
            m_graveyard.push_back({ m_renderFrame + GRAVEYARD_DELAY, std::move(*it) });
            it = m_activeTrails.erase(it);
        } else {
            updateInstanceBuffer(inst);
            ++it;
        }
    }

}

void TrailRenderer::flushGraveyard() {
    ++m_renderFrame;
    VkDevice dev = m_device.getDevice();
    m_graveyard.erase(
        std::remove_if(m_graveyard.begin(), m_graveyard.end(),
                       [this, dev](TrailGraveyardEntry& g) {
                           if (g.killFrame <= m_renderFrame) {
                               vkFreeDescriptorSets(dev, m_descPool, 1, &g.inst->descSet);
                               return true; // erase triggers ~TrailInstance → buffer destroy
                           }
                           return false;
                       }),
        m_graveyard.end());
}

void TrailRenderer::updateInstanceBuffer(TrailInstance& inst) {
    if (inst.points.empty()) return;
    void* data = inst.ssbo.map();
    size_t count = std::min((size_t)MAX_TRAIL_POINTS, inst.points.size());
    std::copy(inst.points.begin(), inst.points.begin() + count, (GpuTrailPoint*)data);
    inst.ssbo.unmap();
}

void TrailRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                           const glm::vec3& camRight, const glm::vec3& camUp) {
    if (m_activeTrails.empty()) return;

    struct TrailPC {
        glm::mat4 viewProj;
        glm::vec4 camRight;
        glm::vec4 camUp;
        uint32_t  pointCount;
        uint32_t  headIndex;
        float widthStart;
        float widthEnd;
    } pc;

    pc.viewProj = viewProj;
    pc.camRight = glm::vec4(camRight, 0.0f);
    pc.camUp    = glm::vec4(camUp, 0.0f);

    // Alpha pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_alphaPipeline);
    for (auto& inst : m_activeTrails) {
        if (inst->def->additive || inst->points.size() < 2) continue;
        
        pc.pointCount = static_cast<uint32_t>(inst->points.size());
        pc.headIndex  = 0;
        pc.widthStart = inst->def->widthStart;
        pc.widthEnd   = inst->def->widthEnd;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TrailPC), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &inst->descSet, 0, nullptr);
        vkCmdDraw(cmd, (pc.pointCount - 1) * 6, 1, 0, 0);
    }

    // Additive pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_additivePipeline);
    for (auto& inst : m_activeTrails) {
        if (!inst->def->additive || inst->points.size() < 2) continue;
        
        pc.pointCount = static_cast<uint32_t>(inst->points.size());
        pc.headIndex  = 0;
        pc.widthStart = inst->def->widthStart;
        pc.widthEnd   = inst->def->widthEnd;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TrailPC), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &inst->descSet, 0, nullptr);
        vkCmdDraw(cmd, (pc.pointCount - 1) * 6, 1, 0, 0);
    }
}

void TrailRenderer::createDescriptorLayout() {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 2;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Trail Layout");
}

void TrailRenderer::createPipelines() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::vec4)*2 + sizeof(uint32_t)*2 + sizeof(float)*2;

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Trail Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "trail_ribbon.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "trail_ribbon.frag.spv");

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

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1; vpCI.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsCI.polygonMode = VK_POLYGON_MODE_FILL; rsCI.lineWidth = 1.0f; rsCI.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable = VK_TRUE; dsCI.depthWriteEnable = VK_FALSE; dsCI.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendAttachmentState trailBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments = trailBlends;

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

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_alphaPipeline), "Alpha Trail Pipe");

    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // Additive
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_additivePipeline), "Additive Trail Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule TrailRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}

// ═══ MeshEffectRenderer.cpp ═══

MeshEffectRenderer::MeshEffectRenderer(const Device& device, const RenderFormats& formats)
    : m_device(device), m_formats(formats) {}

MeshEffectRenderer::~MeshEffectRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    for (auto& pair : m_pipelines) {
        vkDestroyPipeline(dev, pair.second.pipeline, nullptr);
        vkDestroyPipelineLayout(dev, pair.second.layout, nullptr);
    }
}

void MeshEffectRenderer::registerDef(const MeshEffectDef& def) {
    m_defs[def.id] = def;
}

void MeshEffectRenderer::spawn(const std::string& defId, glm::vec3 position,
                               glm::vec3 direction, float scale) {
    auto it = m_defs.find(defId);
    if (it == m_defs.end()) {
        spdlog::warn("MeshEffectRenderer: unknown def '{}'", defId);
        return;
    }

    const auto& def = it->second;
    MeshEffectInstance inst;
    inst.handle = m_nextHandle++;
    inst.def = &def;
    inst.position = position;
    inst.direction = direction;
    inst.scaleBase = scale;
    
    // Auto-align rotation to direction if it's not zero
    if (glm::length(direction) > 0.001f) {
        inst.currentRotation = std::atan2(direction.x, direction.z);
    }

    m_active.push_back(inst);
}

void MeshEffectRenderer::update(float dt) {
    for (auto it = m_active.begin(); it != m_active.end(); ) {
        it->elapsed += dt;
        it->currentRotation += it->def->rotationSpeed * dt;

        if (it->elapsed >= it->def->duration) {
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
}

void MeshEffectRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime) {
    for (const auto& inst : m_active) {
        const auto& def = *inst.def;
        PipelineKey key{ def.vertShader, def.fragShader, def.additive };
        if (m_pipelines.find(key) == m_pipelines.end()) {
            createPipeline(key);
        }
        
        auto& ps = m_pipelines[key];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ps.pipeline);

        float t = inst.elapsed / def.duration;
        float scale = glm::mix(def.scaleStart, def.scaleEnd, t) * inst.scaleBase;
        float alpha = glm::mix(def.alphaStart, def.alphaEnd, t);
        glm::vec4 color = glm::mix(def.colorStart, def.colorEnd, t);

        MeshEffectPC pc;
        pc.viewProj = viewProj;
        pc.color = color;
        pc.data = { alpha, inst.elapsed, appTime, scale };
        pc.posRot = { inst.position, inst.currentRotation };
        pc.dir = { inst.direction, 0.0f };

        vkCmdPushConstants(cmd, ps.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshEffectPC), &pc);

        MeshData* md = getOrLoadMesh(def.meshPath);
        if (md && md->model) {
            md->model->draw(cmd);
        }
    }
}

void MeshEffectRenderer::createPipeline(const PipelineKey& key) {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size = sizeof(MeshEffectPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &layout), "Create MeshEffect Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + key.vert);
    auto fragCode = readFile(std::string(SHADER_DIR) + key.frag);

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
    dsCI.depthTestEnable = VK_TRUE; dsCI.depthWriteEnable = VK_FALSE; dsCI.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = key.additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendAttachmentState meshFxBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments = meshFxBlends;

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
    pipeCI.layout = layout;
    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();
    pipeCI.pNext     = &fmtCI;
    pipeCI.renderPass = VK_NULL_HANDLE;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &pipeline), "Create MeshEffect Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);

    m_pipelines[key] = { layout, pipeline };
}

MeshEffectRenderer::MeshData* MeshEffectRenderer::getOrLoadMesh(const std::string& path) {
    if (path.empty()) return nullptr;
    if (m_meshes.count(path)) return &m_meshes[path];

    try {
        auto model = std::make_unique<Model>(Model::loadFromGLB(m_device, m_device.getAllocator(), path));
        m_meshes[path] = { std::move(model) };
        return &m_meshes[path];
    } catch (const std::exception& e) {
        spdlog::error("Failed to load VFX mesh: {} - {}", path, e.what());
        return nullptr;
    }
}

VkShaderModule MeshEffectRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module), "Failed shader module");
    return module;
}

} // namespace glory
