#include "combat/GpuCollisionSystem.h"
#include "combat/CombatComponents.h"
#include "scene/Components.h"
#include "ability/AbilityComponents.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace glory {

// ── Shader utilities (same pattern as HiZPass / FogOfWarRenderer) ────────

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

// ══════════════════════════════════════════════════════════════════════════
// Initialization / Destruction
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::init(const Device& device) {
    m_device = &device;
    createBuffers();
    createDescriptors();
    createPipelines();
    spdlog::info("[GpuCollision] Initialized — table={} maxPerCell={} maxEntities={} maxPairs={}",
                 TABLE_SIZE, MAX_PER_CELL, MAX_ENTITIES, MAX_PAIRS);
}

void GpuCollisionSystem::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    vkDestroyPipeline(dev, m_hashPipeline, nullptr);
    vkDestroyPipeline(dev, m_broadphasePipeline, nullptr);
    vkDestroyPipelineLayout(dev, m_hashLayout, nullptr);
    vkDestroyPipelineLayout(dev, m_broadphaseLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_hashDescLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_broadphaseDescLayout, nullptr);
    vkDestroyDescriptorPool(dev, m_descPool, nullptr);

    for (auto& f : m_frames) {
        if (f.mappedEntities) { f.entityBuffer.unmap(); f.mappedEntities = nullptr; }
        f.entityBuffer.destroy();
        f.gridBuffer.destroy();
        f.pairBuffer.destroy();
        f.pairReadback.destroy();
    }

    m_device = nullptr;
    spdlog::info("[GpuCollision] Destroyed");
}

// ══════════════════════════════════════════════════════════════════════════
// Buffer Creation
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::createBuffers() {
    VmaAllocator alloc = m_device->getAllocator();

    VkDeviceSize entityBufSize = MAX_ENTITIES * sizeof(GpuEntity);
    // Grid: TABLE_SIZE counts + TABLE_SIZE * MAX_PER_CELL entity indices (all uint32)
    VkDeviceSize gridBufSize   = (TABLE_SIZE + TABLE_SIZE * MAX_PER_CELL) * sizeof(uint32_t);
    // Pairs: 4 bytes for atomicCounter + MAX_PAIRS * sizeof(GpuCollisionPair)
    VkDeviceSize pairBufSize   = sizeof(uint32_t) + MAX_PAIRS * sizeof(GpuCollisionPair);

    for (auto& f : m_frames) {
        // Entity upload buffer (CPU writes, GPU reads)
        f.entityBuffer = Buffer(alloc, entityBufSize,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VMA_MEMORY_USAGE_CPU_TO_GPU);
        f.mappedEntities = reinterpret_cast<GpuEntity*>(f.entityBuffer.map());

        // Hash grid (GPU-only, cleared each frame with vkCmdFillBuffer)
        f.gridBuffer = Buffer(alloc, gridBufSize,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VMA_MEMORY_USAGE_GPU_ONLY);

        // Collision pair output (GPU-only, read back via copy)
        f.pairBuffer = Buffer(alloc, pairBufSize,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VMA_MEMORY_USAGE_GPU_ONLY);

        // CPU-readable readback buffer
        f.pairReadback = Buffer(alloc, pairBufSize,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VMA_MEMORY_USAGE_GPU_TO_CPU);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Descriptor Sets
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::createDescriptors() {
    VkDevice dev = m_device->getDevice();

    // ── Hash shader layout: binding 0 = entities, binding 1 = grid ───────
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings    = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &m_hashDescLayout),
                 "Failed to create spatial hash descriptor layout");
    }

    // ── Broadphase layout: binding 0 = entities, binding 1 = grid, binding 2 = pairs
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings    = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &m_broadphaseDescLayout),
                 "Failed to create broadphase descriptor layout");
    }

    // ── Pool: 2 frames × (2 hash sets + 3 broadphase sets) = 10 SSBO descriptors
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES * 5};

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = MAX_FRAMES * 2; // 2 sets per frame (hash + broadphase)
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "Failed to create GPU collision descriptor pool");

    // ── Allocate and write descriptor sets per frame ──────────────────────
    for (auto& f : m_frames) {
        // Hash set
        {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool     = m_descPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_hashDescLayout;
            VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &f.hashDescSet),
                     "Failed to allocate hash descriptor set");

            VkDescriptorBufferInfo entityInfo{f.entityBuffer.getBuffer(), 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo gridInfo{f.gridBuffer.getBuffer(), 0, VK_WHOLE_SIZE};

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[0].dstSet          = f.hashDescSet;
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo     = &entityInfo;

            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[1].dstSet          = f.hashDescSet;
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo     = &gridInfo;

            vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        // Broadphase set
        {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool     = m_descPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_broadphaseDescLayout;
            VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &f.broadphaseDescSet),
                     "Failed to allocate broadphase descriptor set");

            VkDescriptorBufferInfo entityInfo{f.entityBuffer.getBuffer(), 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo gridInfo{f.gridBuffer.getBuffer(), 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo pairInfo{f.pairBuffer.getBuffer(), 0, VK_WHOLE_SIZE};

            std::array<VkWriteDescriptorSet, 3> writes{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[0].dstSet          = f.broadphaseDescSet;
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo     = &entityInfo;

            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[1].dstSet          = f.broadphaseDescSet;
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo     = &gridInfo;

            writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[2].dstSet          = f.broadphaseDescSet;
            writes[2].dstBinding      = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo     = &pairInfo;

            vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Pipeline Creation
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::createPipelines() {
    VkDevice dev = m_device->getDevice();
    std::string shaderDir = SHADER_DIR;

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(SpatialHashPC);

    // ── Hash pipeline ────────────────────────────────────────────────────
    {
        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount         = 1;
        plCI.pSetLayouts            = &m_hashDescLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges    = &pcRange;
        VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_hashLayout),
                 "Failed to create spatial hash pipeline layout");

        auto code = readFile(shaderDir + "/spatial_hash.comp.spv");
        VkShaderModule mod = createShaderModule(dev, code);

        VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeCI.stage.module = mod;
        pipeCI.stage.pName  = "main";
        pipeCI.layout       = m_hashLayout;
        VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_hashPipeline),
                 "Failed to create spatial hash compute pipeline");

        vkDestroyShaderModule(dev, mod, nullptr);
    }

    // ── Broadphase pipeline ──────────────────────────────────────────────
    {
        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount         = 1;
        plCI.pSetLayouts            = &m_broadphaseDescLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges    = &pcRange;
        VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_broadphaseLayout),
                 "Failed to create broadphase pipeline layout");

        auto code = readFile(shaderDir + "/collision_broadphase.comp.spv");
        VkShaderModule mod = createShaderModule(dev, code);

        VkComputePipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeCI.stage.module = mod;
        pipeCI.stage.pName  = "main";
        pipeCI.layout       = m_broadphaseLayout;
        VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_broadphasePipeline),
                 "Failed to create broadphase compute pipeline");

        vkDestroyShaderModule(dev, mod, nullptr);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Per-Frame Entity Upload
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::uploadEntities(entt::registry& reg, uint32_t frameIndex) {
    auto& f = m_frames[frameIndex];

    // Clear entity map for this upload
    m_entityMap.clear();
    m_entityFlags.clear();

    uint32_t count = 0;

    // Gather all entities with Transform + Team (combat participants)
    auto combatView = reg.view<TransformComponent, TeamComponent>();
    for (auto [entity, tc, team] : combatView.each()) {
        if (count >= MAX_ENTITIES) break;

        GpuEntity& ge = f.mappedEntities[count];
        ge.posX     = tc.position.x;
        ge.posY     = tc.position.y;
        ge.posZ     = tc.position.z;
        ge.entityId = static_cast<uint32_t>(entt::to_integral(entity));
        ge.team     = static_cast<uint32_t>(team.team);
        ge.flags    = 0;
        ge._pad     = 0;

        // Set collision radius based on entity type
        if (reg.all_of<CombatComponent>(entity)) {
            ge.radius = reg.get<CombatComponent>(entity).attackRange;
            ge.flags |= 0x2; // has stats / is combat participant
        } else {
            ge.radius = 2.0f; // default interaction radius
        }

        if (reg.all_of<StatsComponent>(entity)) {
            ge.flags |= 0x2;
        }

        m_entityMap.push_back(entity);
        m_entityFlags.push_back(ge.flags);
        count++;
    }

    // Gather projectiles (have ProjectileComponent but may not have TeamComponent through combatView)
    auto projView = reg.view<TransformComponent, ProjectileComponent>();
    for (auto [entity, tc, pc] : projView.each()) {
        if (count >= MAX_ENTITIES) break;

        // Skip if already added via combatView
        bool alreadyAdded = false;
        for (uint32_t i = 0; i < m_entityMap.size(); i++) {
            if (m_entityMap[i] == entity) { alreadyAdded = true; break; }
        }
        if (alreadyAdded) continue;

        GpuEntity& ge = f.mappedEntities[count];
        ge.posX     = tc.position.x;
        ge.posY     = tc.position.y;
        ge.posZ     = tc.position.z;
        ge.radius   = (pc.sourceDef && pc.sourceDef->projectile.width > 0.f)
                      ? pc.sourceDef->projectile.width * 0.5f
                      : 0.8f;
        ge.entityId = static_cast<uint32_t>(entt::to_integral(entity));

        // Determine team from caster
        auto casterEntt = static_cast<entt::entity>(pc.casterEntity);
        if (reg.valid(casterEntt) && reg.all_of<TeamComponent>(casterEntt))
            ge.team = static_cast<uint32_t>(reg.get<TeamComponent>(casterEntt).team);
        else
            ge.team = 2; // neutral

        ge.flags = 0x1; // projectile flag
        ge._pad  = 0;

        m_entityMap.push_back(entity);
        m_entityFlags.push_back(ge.flags);
        count++;
    }

    f.entityCount    = count;
    m_lastEntityCount = count;

    // Flush CPU writes to ensure GPU visibility
    f.entityBuffer.flush();
}

// ══════════════════════════════════════════════════════════════════════════
// Compute Dispatch
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::dispatch(VkCommandBuffer cmd, uint32_t frameIndex,
                                   uint32_t computeFamily, uint32_t graphicsFamily) {
    auto& f = m_frames[frameIndex];
    if (f.entityCount == 0) return;

    // ── 1. Clear grid cell counts to zero ────────────────────────────────
    vkCmdFillBuffer(cmd, f.gridBuffer.getBuffer(), 0,
                    TABLE_SIZE * sizeof(uint32_t), 0);

    // ── 2. Clear pair counter to zero ────────────────────────────────────
    vkCmdFillBuffer(cmd, f.pairBuffer.getBuffer(), 0,
                    sizeof(uint32_t), 0);

    // ── 3. Barrier: fill → compute read/write ────────────────────────────
    {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    SpatialHashPC pc{};
    pc.entityCount = f.entityCount;
    pc.cellSize    = CELL_SIZE;
    pc.invCellSize = 1.0f / CELL_SIZE;
    pc.tableSize   = TABLE_SIZE;
    pc.maxPerCell  = MAX_PER_CELL;
    pc.maxPairs    = MAX_PAIRS;

    uint32_t groupCount = (f.entityCount + 63) / 64;

    // ── 4. Dispatch spatial hash build ───────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_hashPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_hashLayout, 0, 1, &f.hashDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_hashLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(SpatialHashPC), &pc);
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // ── 5. Barrier: hash write → broadphase read ─────────────────────────
    {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ── 6. Dispatch broadphase collision ─────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_broadphasePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_broadphaseLayout, 0, 1, &f.broadphaseDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_broadphaseLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(SpatialHashPC), &pc);
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // ── 7. Barrier: compute write → transfer read ────────────────────────
    {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // ── 8. Copy pair results to CPU-readable buffer ──────────────────────
    VkBufferCopy copyRegion{};
    copyRegion.size = sizeof(uint32_t) + MAX_PAIRS * sizeof(GpuCollisionPair);
    vkCmdCopyBuffer(cmd, f.pairBuffer.getBuffer(), f.pairReadback.getBuffer(),
                    1, &copyRegion);
}

// ══════════════════════════════════════════════════════════════════════════
// Result Readback
// ══════════════════════════════════════════════════════════════════════════

void GpuCollisionSystem::readResults(uint32_t frameIndex) {
    auto& f = m_frames[frameIndex];
    m_results.clear();

    if (f.entityCount == 0) return;

    // Map readback buffer
    void* mapped = f.pairReadback.map();
    if (!mapped) return;

    uint32_t pairCount = *reinterpret_cast<uint32_t*>(mapped);
    pairCount = std::min(pairCount, MAX_PAIRS);

    auto* pairs = reinterpret_cast<GpuCollisionPair*>(
        static_cast<uint8_t*>(mapped) + sizeof(uint32_t));

    m_results.reserve(pairCount);
    for (uint32_t i = 0; i < pairCount; i++) {
        uint32_t a = pairs[i].entityA;
        uint32_t b = pairs[i].entityB;

        if (a >= m_entityMap.size() || b >= m_entityMap.size()) continue;

        CollisionResult r{};
        r.entityA = m_entityMap[a];
        r.entityB = m_entityMap[b];
        r.distSq  = pairs[i].distSq;
        m_results.push_back(r);
    }

    f.pairReadback.unmap();
}

// ══════════════════════════════════════════════════════════════════════════
// Query API
// ══════════════════════════════════════════════════════════════════════════

entt::entity GpuCollisionSystem::findNearestEnemy(entt::registry& reg,
                                                   entt::entity attacker,
                                                   float range) const {
    if (!reg.all_of<TransformComponent, TeamComponent>(attacker))
        return entt::null;

    auto attackerTeam = reg.get<TeamComponent>(attacker).team;
    float bestDistSq  = range * range;
    entt::entity best  = entt::null;

    for (const auto& pair : m_results) {
        entt::entity other = entt::null;

        if (pair.entityA == attacker)
            other = pair.entityB;
        else if (pair.entityB == attacker)
            other = pair.entityA;
        else
            continue;

        if (!reg.valid(other)) continue;
        if (!reg.all_of<TeamComponent>(other)) continue;
        if (reg.get<TeamComponent>(other).team == attackerTeam) continue;

        if (pair.distSq < bestDistSq) {
            bestDistSq = pair.distSq;
            best = other;
        }
    }

    // Fallback: if no GPU results available, do CPU scan
    if (best == entt::null && m_results.empty()) {
        auto& attackerPos = reg.get<TransformComponent>(attacker).position;
        bestDistSq = range * range;

        auto view = reg.view<TransformComponent, TeamComponent>();
        for (auto [entity, transform, team] : view.each()) {
            if (entity == attacker) continue;
            if (team.team == attackerTeam) continue;

            glm::vec2 a2d(attackerPos.x, attackerPos.z);
            glm::vec2 e2d(transform.position.x, transform.position.z);
            glm::vec2 diff = e2d - a2d;
            float distSq = diff.x * diff.x + diff.y * diff.y;
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                best = entity;
            }
        }
    }

    return best;
}

std::vector<GpuCollisionSystem::ProjectileHit> GpuCollisionSystem::getProjectileHits() const {
    std::vector<ProjectileHit> hits;

    for (const auto& pair : m_results) {
        // Check which entity is the projectile
        uint32_t idxA = UINT32_MAX, idxB = UINT32_MAX;
        for (uint32_t i = 0; i < m_entityMap.size(); i++) {
            if (m_entityMap[i] == pair.entityA) idxA = i;
            if (m_entityMap[i] == pair.entityB) idxB = i;
        }

        bool aIsProj = (idxA < m_entityFlags.size()) && (m_entityFlags[idxA] & 0x1);
        bool bIsProj = (idxB < m_entityFlags.size()) && (m_entityFlags[idxB] & 0x1);
        bool aHasStats = (idxA < m_entityFlags.size()) && (m_entityFlags[idxA] & 0x2);
        bool bHasStats = (idxB < m_entityFlags.size()) && (m_entityFlags[idxB] & 0x2);

        if (aIsProj && bHasStats) {
            hits.push_back({pair.entityA, pair.entityB, pair.distSq});
        } else if (bIsProj && aHasStats) {
            hits.push_back({pair.entityB, pair.entityA, pair.distSq});
        }
    }

    return hits;
}

} // namespace glory
