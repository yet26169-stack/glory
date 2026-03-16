#pragma once

#include "vfx/VFXTypes.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/ParticleSystem.h"
#include "renderer/Texture.h"
#include "renderer/RenderFormats.h"

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
    VFXRenderer(const Device& device, const RenderFormats& formats);
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

    // Dispatch compute on a separate command buffer (for async compute path).
    void dispatchComputeAsync(VkCommandBuffer computeCmd,
                              uint32_t srcQueueFamily,
                              uint32_t dstQueueFamily);

    // Acquire ownership of particle buffers on the graphics queue after async compute.
    void acquireFromCompute(VkCommandBuffer graphicsCmd,
                            uint32_t srcQueueFamily,
                            uint32_t dstQueueFamily);

    // Insert pipeline barrier: SSBO write (compute) → SSBO read (vertex shader).
    void barrierComputeToGraphics(VkCommandBuffer cmd);

    // Set camera position for LOD + sorting.
    void setCameraPosition(const glm::vec3& pos) { m_cameraPos = pos; }

    // Draw all alive particle effects as alpha-blended billboards.
    void render(VkCommandBuffer cmd,
                const glm::mat4& viewProj,
                const glm::vec3& camRight,
                const glm::vec3& camUp,
                glm::vec2 screenSize,
                float nearPlane,
                float farPlane);

    void setDepthBuffer(VkImageView depthView, VkSampler sampler);

    // ── Registry ──────────────────────────────────────────────────────────

    // Register a custom emitter definition (e.g. loaded from JSON).
    void registerEmitter(EmitterDef def);

    // Load all *.json files in a directory as EmitterDefs.
    void loadEmitterDirectory(const std::string& path);

    // Direct spawn (bypasses event queue — for VFXFactory use).
    // Fills ev.handle with the assigned handle.
    void spawnDirect(VFXEvent& ev);

private:
    const Device& m_device;

    // ── Shared descriptor set layout (binding 0 = SSBO, binding 1 = sampler) ──
    VkDescriptorSetLayout  m_descLayout = VK_NULL_HANDLE;

    // ── One shared pool with MAX_CONCURRENT_EMITTERS sets pre-allocated ────
    VkDescriptorPool       m_descPool   = VK_NULL_HANDLE;

    // ── Compute pipelines (simulation + compaction + sort) ───────────────────
    VkPipelineLayout       m_computeLayout   = VK_NULL_HANDLE;
    VkPipeline             m_computePipeline = VK_NULL_HANDLE;
    VkPipelineLayout       m_compactLayout   = VK_NULL_HANDLE;
    VkPipeline             m_compactPipeline = VK_NULL_HANDLE;
    VkPipelineLayout       m_sortLayout      = VK_NULL_HANDLE;
    VkPipeline             m_sortPipeline    = VK_NULL_HANDLE;


    struct SimPC {
        float    dt;
        float    gravity;
        uint32_t count;
        float    _pad;
    };

    // ── Graphics pipeline (billboard rendering) ────────────────────────────
    VkPipelineLayout       m_renderLayout    = VK_NULL_HANDLE;
    VkPipeline             m_alphaPipeline   = VK_NULL_HANDLE;
    VkPipeline             m_additivePipeline = VK_NULL_HANDLE;

    struct RenderPC {
        glm::mat4 viewProj;   // 64
        glm::vec4 camRight;   // 16
        glm::vec4 camUp;      // 16
        glm::vec2 screenSize; //  8
        float     nearPlane;  //  4
        float     farPlane;   //  4
    }; // Total: 112 bytes
    static_assert(sizeof(RenderPC) <= 128, "RenderPC exceeds push constant limit");

    struct CompactPC {
        uint32_t totalCount;
    };

    struct SortPC {
        glm::vec4 cameraPos;  // xyz = camera world pos
        uint32_t  count;      // particle count (rounded to power-of-2)
        uint32_t  k;          // bitonic outer loop param
        uint32_t  j;          // bitonic inner loop param
        uint32_t  _pad;
    };
    static_assert(sizeof(SortPC) <= 32, "SortPC too large");

    // ── Particle LOD: skip sim on distant emitters ─────────────────────────
    // lodSkipMask per emitter: 0=every frame, 1=every 2nd, 3=every 4th
    glm::vec3 m_cameraPos{0.0f};  // set each frame for sort + LOD

    // ── Default particle atlas (white 1×1 fallback) ────────────────────────
    Texture                m_defaultAtlas;

    // ── Loaded atlas textures (keyed by EmitterDef::textureAtlas path) ─────
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_atlasCache;

    // ── Registered emitter definitions ────────────────────────────────────
    std::unordered_map<std::string, EmitterDef> m_emitterDefs;

    // ── Active effects ─────────────────────────────────────────────────────
    std::vector<std::unique_ptr<ParticleSystem>> m_effects;
    uint32_t                    m_nextHandle = 1;
    float                       m_currentDt = 0.0f;

    VkImageView m_depthView = VK_NULL_HANDLE;
    VkSampler   m_depthSampler = VK_NULL_HANDLE;

    // ── Deferred deletion graveyard ────────────────────────────────────────
    // ParticleSystems moved here when dead; destroyed only after GPU is done.
    // Each entry holds the frame threshold after which destruction is safe.
    static constexpr int GRAVEYARD_DELAY = 20; // Extra padding for safety
    struct GraveyardEntry {
        int            killFrame;
        std::unique_ptr<ParticleSystem> ps;
    };
    std::vector<GraveyardEntry> m_graveyard;
    int                         m_frameCount = 0;

    // ── Initialisation helpers ─────────────────────────────────────────────
    void createDescriptorLayoutAndPool();
    void createComputePipeline();
    void createCompactPipeline();
    void createSortPipeline();
    void createRenderPipeline(const RenderFormats& formats);

    void dispatchSort(VkCommandBuffer cmd, uint32_t maxParticles);
    uint32_t nextPowerOf2(uint32_t n) const;

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
