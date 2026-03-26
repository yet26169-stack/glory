#include "vfx/ParticleSystem.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include <cstring>
#include <algorithm>

namespace glory {

// ── Constructor ──────────────────────────────────────────────────────────────
ParticleSystem::ParticleSystem(const Device&          device,
                               const EmitterDef&      def,
                               VkDescriptorSetLayout  descLayout,
                               VkDescriptorPool       sharedPool,
                               Texture*               atlas,
                               VkImageView            depthView,
                               VkSampler              depthSampler,
                               glm::vec3              worldPosition,
                               glm::vec3              direction,
                               float                  scale,
                               float                  overrideLifetime,
                               uint32_t               handle)
    : m_device     (&device)
    , m_def        (&def)
    , m_atlas      (atlas)
    , m_position   (worldPosition)
    , m_direction  (glm::length(direction) > 0.001f ? glm::normalize(direction) : glm::vec3(0,1,0))
    , m_scale      (scale)
    , m_duration   (overrideLifetime > 0.f ? overrideLifetime : def.duration)
    , m_looping    (def.looping)
    , m_handle     (handle)
    , m_rng        (std::random_device{}())
{
    m_maxParticles = std::min(def.maxParticles, MAX_PARTICLES_PER_EMITTER);

    const VkDeviceSize bufSize = sizeof(GpuParticle) * m_maxParticles;

    // Allocate SSBO with CPU_TO_GPU so we can write new particles from CPU
    // while the compute shader reads+writes the buffer on the GPU.
    m_ssboBuffer = Buffer(device.getAllocator(),
                          bufSize,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VMA_MEMORY_USAGE_CPU_TO_GPU);

    m_particles = reinterpret_cast<GpuParticle*>(m_ssboBuffer.map());

    // Zero all particles (marks them as inactive via params.w = 0)
    std::memset(m_particles, 0, bufSize);

    // Create UBO for emitter parameters
    m_emitterBuffer = Buffer(device.getAllocator(),
                             sizeof(EmitterParams),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_emitterParams = reinterpret_cast<EmitterParams*>(m_emitterBuffer.map());

    // Create Indirect Draw Buffer
    m_indirectBuffer = Buffer(device.getAllocator(),
                              sizeof(VkDrawIndirectCommand),
                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VMA_MEMORY_USAGE_GPU_ONLY);

    // Fill parameters
    m_emitterParams->wind_dt = {def.windDirection * def.windStrength, 0.0f}; // dt set in update/render
    m_emitterParams->phys    = {def.gravity, def.drag, def.alphaCurve, static_cast<float>(m_maxParticles)};
    m_emitterParams->size    = {def.sizeMin, def.sizeMax, def.sizeEnd, def.softFadeDistance};
    float bitmask = static_cast<float>(def.forceParams_bitmask ? def.forceParams_bitmask : (1u << def.forceType));
    m_emitterParams->forceParams  = {static_cast<float>(def.forceType), def.forceStrength, bitmask, 0.0f};
    m_emitterParams->attractorPos = {def.attractorPos.x, def.attractorPos.y, def.attractorPos.z, 0.0f};
    // atlasParams.z: prefer flipbookFPS if set; fallback to legacy atlasFrameRate
    const float effectiveFPS = (def.flipbookFPS != 0.0f) ? def.flipbookFPS : def.atlasFrameRate;
    m_emitterParams->atlasParams  = {static_cast<float>(def.atlasRows),
                                     static_cast<float>(def.atlasCols),
                                     effectiveFPS,
                                     def.atlasLoopFrames ? 1.0f : 0.0f};

    const uint32_t keyCount = std::min(static_cast<uint32_t>(def.colorOverLifetime.size()), 8u);
    m_emitterParams->colorKeyCount = keyCount;
    for (uint32_t k = 0; k < keyCount; ++k) {
        m_emitterParams->colorKeys[k].color = def.colorOverLifetime[k].color;
        m_emitterParams->colorKeys[k].time  = def.colorOverLifetime[k].time;
    }

    // Allocate descriptor set from the shared pool
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = sharedPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &descLayout;
    VkResult allocResult = vkAllocateDescriptorSets(device.getDevice(), &allocInfo, &m_descSet);
    if (allocResult != VK_SUCCESS) {
        spdlog::warn("[VFX] Descriptor set allocation failed (VkResult {}), "
                     "effect handle {} will be skipped", static_cast<int>(allocResult), handle);
        m_descSet = VK_NULL_HANDLE;
        return;
    }

    writeToDescriptorSet(descLayout, atlas, depthView, depthSampler);
}

// ── Destructor ────────────────────────────────────────────────────────────────
ParticleSystem::~ParticleSystem() {
    destroy();
}

void ParticleSystem::destroy() {
    if (m_particles && m_ssboBuffer.getBuffer() != VK_NULL_HANDLE) {
        m_ssboBuffer.unmap();
        m_particles = nullptr;
    }
    if (m_emitterParams && m_emitterBuffer.getBuffer() != VK_NULL_HANDLE) {
        m_emitterBuffer.unmap();
        m_emitterParams = nullptr;
    }
    // Descriptor set is freed when the pool is reset; no individual free needed.
    m_descSet = VK_NULL_HANDLE;
}

// ── Move semantics ────────────────────────────────────────────────────────────
ParticleSystem::ParticleSystem(ParticleSystem&& o) noexcept
    : m_device      (o.m_device)
    , m_def         (o.m_def)
    , m_ssboBuffer  (std::move(o.m_ssboBuffer))
    , m_particles   (o.m_particles)
    , m_maxParticles(o.m_maxParticles)
    , m_emitterBuffer(std::move(o.m_emitterBuffer))
    , m_emitterParams(o.m_emitterParams)
    , m_indirectBuffer(std::move(o.m_indirectBuffer))
    , m_descSet     (o.m_descSet)
    , m_position    (o.m_position)
    , m_direction   (o.m_direction)
    , m_scale       (o.m_scale)
    , m_timeAlive   (o.m_timeAlive)
    , m_duration    (o.m_duration)
    , m_emitAccum   (o.m_emitAccum)
    , m_looping     (o.m_looping)
    , m_stopped     (o.m_stopped)
    , m_burst       (o.m_burst)
    , m_handle      (o.m_handle)
    , m_rng         (std::move(o.m_rng))
{
    o.m_particles = nullptr;
    o.m_emitterParams = nullptr;
    o.m_descSet   = VK_NULL_HANDLE;
}

ParticleSystem& ParticleSystem::operator=(ParticleSystem&& o) noexcept {
    if (this != &o) {
        destroy();
        m_device       = o.m_device;
        m_def          = o.m_def;
        m_ssboBuffer   = std::move(o.m_ssboBuffer);
        m_particles    = o.m_particles;
        m_maxParticles = o.m_maxParticles;
        m_emitterBuffer= std::move(o.m_emitterBuffer);
        m_emitterParams= o.m_emitterParams;
        m_indirectBuffer = std::move(o.m_indirectBuffer);
        m_descSet      = o.m_descSet;
        m_position     = o.m_position;
        m_direction    = o.m_direction;
        m_scale        = o.m_scale;
        m_timeAlive    = o.m_timeAlive;
        m_duration     = o.m_duration;
        m_emitAccum    = o.m_emitAccum;
        m_looping      = o.m_looping;
        m_stopped      = o.m_stopped;
        m_burst        = o.m_burst;
        m_handle       = o.m_handle;
        m_rng          = std::move(o.m_rng);
        o.m_particles  = nullptr;
        o.m_emitterParams = nullptr;
        o.m_descSet    = VK_NULL_HANDLE;
    }
    return *this;
}

// ── update ─────────────────────────────────────────────────────────────────
void ParticleSystem::update(float dt) {
    if (!m_particles || !m_def) return;

    if (m_emitterParams) {
        m_emitterParams->wind_dt.w = dt;
    }

    m_timeAlive += dt;

    const bool expired = !m_looping && (m_timeAlive >= m_duration);

    if (!m_stopped && !expired) {
        // Burst on very first frame
        if (!m_burst && m_def->burstCount > 0.0f) {
            m_burst = true;
            for (int b = 0; b < static_cast<int>(m_def->burstCount); ++b) {
                spawnParticle();
            }
        }

        // Continuous emission
        m_emitAccum += m_def->emitRate * dt;
        while (m_emitAccum >= 1.0f) {
            m_emitAccum -= 1.0f;
            spawnParticle();
        }
    }
}

// ── isAlive ───────────────────────────────────────────────────────────────
bool ParticleSystem::isAlive() const {
    if (!m_particles) return false;

    // Emitter is alive if it's still emitting OR any particle is still active
    const bool stillEmitting = m_looping ||
                               (!m_stopped && m_timeAlive < m_duration);
    if (stillEmitting) return true;

    for (uint32_t i = 0; i < m_maxParticles; ++i) {
        if (m_particles[i].params.w > 0.5f) return true;
    }
    return false;
}

// ── scanDeaths ────────────────────────────────────────────────────────────
std::vector<ParticleDeathEvent> ParticleSystem::scanDeaths() {
    std::vector<ParticleDeathEvent> events;
    if (!m_particles || !m_def || m_def->subEmitters.empty())
        return events;

    // Lazily initialise the alive bitset on first call
    if (m_wasAlive.size() != m_maxParticles)
        m_wasAlive.assign(m_maxParticles, false);

    for (uint32_t i = 0; i < m_maxParticles; ++i) {
        const bool aliveNow = m_particles[i].params.w >= 0.1f;
        if (m_wasAlive[i] && !aliveNow) {
            // Particle i just died — record its last known position/velocity
            events.push_back({
                glm::vec3(m_particles[i].posLife),
                glm::vec3(m_particles[i].velAge)
            });
        }
        m_wasAlive[i] = aliveNow;
    }
    return events;
}

// ── spawnParticle ─────────────────────────────────────────────────────────
void ParticleSystem::spawnParticle() {
    // Find a dead slot (simple linear scan; fast for small emitters)
    for (uint32_t i = 0; i < m_maxParticles; ++i) {
        if (m_particles[i].params.w < 0.5f) {
            GpuParticle& p = m_particles[i];

            auto frand = [&](float lo, float hi) -> float {
                std::uniform_real_distribution<float> d(lo, hi);
                return d(m_rng);
            };

            // Randomise velocity inside a cone around m_direction
            const float angleRad = glm::radians(m_def->spreadAngle);
            const float theta    = frand(0.0f, glm::two_pi<float>());
            const float phi      = frand(0.0f, angleRad);
            const float sinPhi   = std::sin(phi);
            const float cosPhi   = std::cos(phi);

            // Build orthonormal basis around m_direction
            glm::vec3 up    = (std::abs(m_direction.x) < 0.9f) ?
                              glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            glm::vec3 right = glm::normalize(glm::cross(m_direction, up));
            up              = glm::cross(right, m_direction);

            glm::vec3 vel = (m_direction * cosPhi
                           + right        * sinPhi * std::cos(theta)
                           + up           * sinPhi * std::sin(theta))
                           * frand(m_def->initialSpeedMin, m_def->initialSpeedMax)
                           * m_scale;

            const float life = frand(m_def->lifetimeMin, m_def->lifetimeMax);
            const float size = frand(m_def->sizeMin, m_def->sizeMax) * m_scale;
            const float rot  = frand(0.0f, glm::two_pi<float>());
            const float angVel = glm::radians(frand(m_def->rotationSpeedMin, m_def->rotationSpeedMax));

            // Choose spawn colour: first key or white if not defined
            glm::vec4 col{1.0f};
            if (!m_def->colorOverLifetime.empty()) {
                col = m_def->colorOverLifetime.front().color;
            }

            p.posLife = {m_position.x, m_position.y, m_position.z, life};
            p.velAge  = {vel.x, vel.y, vel.z, 0.0f};
            p.color   = col;
            
            // Packed params.w: atlasFrame (integer part) + active bias (0.5)
            float startFrame = 0.0f;
            if (m_def->flipbookRandomStart && m_def->atlasFrameCount > 1) {
                std::uniform_int_distribution<uint32_t> frameDist(0, m_def->atlasFrameCount - 1);
                startFrame = static_cast<float>(frameDist(m_rng));
            }
            const float activePacked = startFrame + 0.5f;
            p.params  = {size, rot, angVel, activePacked};

            return; // only spawn one particle per call
        }
    }
}

// ── updateDepthBuffer ──────────────────────────────────────────────────
void ParticleSystem::updateDepthBuffer(VkDescriptorSetLayout layout, VkImageView depthView, VkSampler sampler) {
    if (m_descSet == VK_NULL_HANDLE || !m_def) return;
    writeToDescriptorSet(layout, m_atlas, depthView, sampler);
}

// ── writeToDescriptorSet ─────────────────────────────────────────────────
void ParticleSystem::writeToDescriptorSet(VkDescriptorSetLayout /*layout*/,
                                          Texture* atlas,
                                          VkImageView depthView,
                                          VkSampler depthSampler) {
    if (m_descSet == VK_NULL_HANDLE) return;

    VkDevice dev = m_device->getDevice();

    // Binding 0 — storage buffer (SSBO)
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = m_ssboBuffer.getBuffer();
    bufInfo.offset = 0;
    bufInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = m_emitterBuffer.getBuffer();
    uboInfo.offset = 0;
    uboInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[5]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo     = &bufInfo;

    // Binding 1 — combined image sampler (atlas)
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = atlas->getImageView();
    imgInfo.sampler     = atlas->getSampler();

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &imgInfo;

    // Binding 2 — Emitter UBO
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_descSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo     = &uboInfo;

    // Binding 3 — Depth Buffer
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthInfo.imageView   = depthView;
    depthInfo.sampler     = depthSampler;

    writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet          = m_descSet;
    writes[3].dstBinding      = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo      = &depthInfo;

    // Binding 4 — Indirect Buffer
    VkDescriptorBufferInfo indirectInfo{};
    indirectInfo.buffer = m_indirectBuffer.getBuffer();
    indirectInfo.offset = 0;
    indirectInfo.range  = VK_WHOLE_SIZE;

    writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet          = m_descSet;
    writes[4].dstBinding      = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo     = &indirectInfo;

    vkUpdateDescriptorSets(dev, 5, writes, 0, nullptr);
}

} // namespace glory
