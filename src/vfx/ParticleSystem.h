#pragma once

#include "vfx/VFXTypes.h"
#include "renderer/Buffer.h"
#include "renderer/Texture.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <random>

namespace glory {

class Device;

// ── ParticleSystem ─────────────────────────────────────────────────────────
// Manages a single active effect instance:
//   • A GPU-side SSBO holding up to EmitterDef::maxParticles GpuParticle entries
//   • CPU-side persistently-mapped pointer for spawning new particles
//   • Per-frame emission bookkeeping (accumulator, time, burst on first frame)
//   • The descriptor set used both by the compute and render pipelines
//
// The SSBO is allocated with VMA_MEMORY_USAGE_CPU_TO_GPU so that the CPU can
// write spawn data directly while the GPU compute shader reads & updates.
class ParticleSystem {
public:
    ParticleSystem() = default;
    ParticleSystem(const Device&          device,
                   const EmitterDef&      def,
                   VkDescriptorSetLayout  descLayout,
                   VkDescriptorPool       sharedPool,
                   Texture*               atlas,
                   glm::vec3              worldPosition,
                   glm::vec3              direction,
                   float                  scale,
                   float                  overrideLifetime,
                   uint32_t               handle);

    ~ParticleSystem();

    // Non-copyable, moveable
    ParticleSystem(const ParticleSystem&)            = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    ParticleSystem(ParticleSystem&&) noexcept;
    ParticleSystem& operator=(ParticleSystem&&) noexcept;

    // Called once per frame BEFORE the compute dispatch.
    // Writes newly-spawned particles into the persistently-mapped SSBO.
    void update(float dt);

    // Teleport the emitter to a new world position
    void moveTo(glm::vec3 newPos) { m_position = newPos; }

    // Force-stop this emitter (no more emissions; existing particles finish out)
    void stop() { m_stopped = true; }

    bool  isAlive()   const; // true while any particle is still active
    uint32_t handle() const { return m_handle; }

    // Accessors needed by VFXRenderer during dispatch / draw
    VkBuffer        ssbo()        const { return m_ssboBuffer.getBuffer(); }
    VkBuffer        emitterUbo()  const { return m_emitterBuffer.getBuffer(); }
    VkDescriptorSet descSet()     const { return m_descSet; }
    uint32_t        maxParticles()const { return m_maxParticles; }

private:
    const Device*      m_device     = nullptr;
    const EmitterDef*  m_def        = nullptr;

    // GPU SSBO + persistent CPU mapping
    Buffer             m_ssboBuffer;
    GpuParticle*       m_particles  = nullptr;    // mapped pointer into SSBO
    uint32_t           m_maxParticles = 0;

    // GPU UBO for emitter parameters
    Buffer             m_emitterBuffer;
    EmitterParams*     m_emitterParams = nullptr;

    // Descriptor set (shared layout: binding 0 = SSBO, binding 1 = atlas)
    VkDescriptorSet    m_descSet    = VK_NULL_HANDLE;

    // Emitter state
    glm::vec3          m_position;
    glm::vec3          m_direction;
    float              m_scale      = 1.0f;
    float              m_timeAlive  = 0.0f;
    float              m_duration   = 0.0f;   // effective duration (overrideLifetime or def)
    float              m_emitAccum  = 0.0f;   // sub-frame emission accumulator
    bool               m_looping    = false;
    bool               m_stopped    = false;
    bool               m_burst      = false;  // burst has been issued on first frame
    uint32_t           m_handle     = 0;

    std::mt19937       m_rng;

    void spawnParticle();
    void writeToDescriptorSet(VkDescriptorSetLayout layout,
                              Texture* atlas);
    void destroy();
};

} // namespace glory
