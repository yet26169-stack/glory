#pragma once

#include "vfx/VFXTypes.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/ParticleSystem.h"
#include "renderer/Texture.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace glory {

class Device;

// ── VFXRenderer ────────────────────────────────────────────────────────────
// Owns the particle compute pipeline and billboard graphics pipeline.
// Manages a pool of up to MAX_CONCURRENT_EMITTERS active ParticleSystem objects.
//
// Integration into the main render loop (per-frame order):
//   1. vfxRenderer.processQueue(queue)   — flush spawns/destroys from game thread
//   2. vfxRenderer.update(dt)            — CPU emission bookkeeping
//   3. vfxRenderer.dispatchCompute(cmd)  — GPU compute simulate (outside render pass)
//   4. vfxRenderer.barrierComputeToGraphics(cmd) — pipeline barrier
//   5. (begin render pass)
//   6. vfxRenderer.render(cmd, viewProj, camRight, camUp)  — draw billboards
//
class VFXRenderer {
public:
    VFXRenderer(const Device& device, VkRenderPass renderPass);
    ~VFXRenderer();

    VFXRenderer(const VFXRenderer&)            = delete;
    VFXRenderer& operator=(const VFXRenderer&) = delete;

    // ── Per-frame interface ────────────────────────────────────────────────

    // Drain the SPSC queue and process Spawn/Destroy/Move events.
    void processQueue(VFXEventQueue& queue);

    // Update all active emitter CPU state (emission, lifetime).
    void update(float dt);

    // Dispatch compute shaders to simulate live particles (call OUTSIDE render pass).
    void dispatchCompute(VkCommandBuffer cmd);

    // Insert pipeline barrier: SSBO write (compute) → SSBO read (vertex shader).
    void barrierComputeToGraphics(VkCommandBuffer cmd);

    // Draw all alive particle effects as alpha-blended billboards.
    void render(VkCommandBuffer cmd,
                const glm::mat4& viewProj,
                const glm::vec3& camRight,
                const glm::vec3& camUp);

    // ── Registry ──────────────────────────────────────────────────────────

    // Register a custom emitter definition (e.g. loaded from JSON).
    void registerEmitter(EmitterDef def);

    // Load all *.json files in a directory as EmitterDefs.
    void loadEmitterDirectory(const std::string& path);

private:
    const Device& m_device;

    // ── Shared descriptor set layout (binding 0 = SSBO, binding 1 = sampler) ──
    VkDescriptorSetLayout  m_descLayout = VK_NULL_HANDLE;

    // ── One shared pool with MAX_CONCURRENT_EMITTERS sets pre-allocated ────
    VkDescriptorPool       m_descPool   = VK_NULL_HANDLE;

    // ── Compute pipeline (particle simulation) ─────────────────────────────
    VkPipelineLayout       m_computeLayout   = VK_NULL_HANDLE;
    VkPipeline             m_computePipeline = VK_NULL_HANDLE;

    struct SimPC {
        float    dt;
        float    gravity;
        uint32_t count;
        float    _pad;
    };

    // ── Graphics pipeline (billboard rendering) ────────────────────────────
    VkPipelineLayout       m_renderLayout    = VK_NULL_HANDLE;
    VkPipeline             m_renderPipeline  = VK_NULL_HANDLE;

    struct RenderPC {
        glm::mat4 viewProj;
        glm::vec4 camRight;
        glm::vec4 camUp;
    };
    static_assert(sizeof(RenderPC) <= 128, "RenderPC exceeds push constant limit");

    // ── Default particle atlas (white 1×1 fallback) ────────────────────────
    Texture                m_defaultAtlas;

    // ── Loaded atlas textures (keyed by EmitterDef::textureAtlas path) ─────
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_atlasCache;

    // ── Registered emitter definitions ────────────────────────────────────
    std::unordered_map<std::string, EmitterDef> m_emitterDefs;

    // ── Active effects ─────────────────────────────────────────────────────
    std::vector<ParticleSystem> m_effects;
    uint32_t                    m_nextHandle = 1;

    // ── Deferred deletion graveyard ────────────────────────────────────────
    // ParticleSystems moved here when dead; destroyed only after GPU is done.
    // Each entry holds the frame threshold after which destruction is safe.
    static constexpr int GRAVEYARD_DELAY = 3; // MAX_FRAMES_IN_FLIGHT(2) + 1
    struct GraveyardEntry {
        int            killFrame;
        ParticleSystem ps;
    };
    std::vector<GraveyardEntry> m_graveyard;
    int                         m_frameCount = 0;

    // ── Initialisation helpers ─────────────────────────────────────────────
    void createDescriptorLayoutAndPool();
    void createComputePipeline();
    void createRenderPipeline(VkRenderPass renderPass);

    // Returns existing or loaded atlas texture for a given path
    Texture* getOrLoadAtlas(const std::string& texturePath);

    // Process a single event
    void handleSpawn(VFXEvent& ev);
    void handleDestroy(const VFXEvent& ev);
    void handleMove(const VFXEvent& ev);

    static std::vector<char> readSPV(const std::string& path);
    VkShaderModule            createShaderModule(const std::vector<char>& code) const;
};

} // namespace glory
