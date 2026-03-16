#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayer.h"
#include "animation/Skeleton.h"
#include "camera/Camera.h"
#include "renderer/Buffer.h"
#include "renderer/ClickIndicatorRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "renderer/DistortionRenderer.h"
#include "renderer/InkingPass.h"
#include "renderer/FogOfWarRenderer.h"
#include "renderer/OutlineRenderer.h"
#include "renderer/WaterRenderer.h"
#include "fog/FogSystem.h"
#include "renderer/ShieldBubbleRenderer.h"
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "renderer/Context.h"
#include "renderer/Descriptors.h"
#include "renderer/BindlessDescriptors.h"
#include "renderer/Device.h"
#include "renderer/Pipeline.h"
#include "renderer/RenderFormats.h"
#include "renderer/RenderGraph.h"
#include "renderer/Swapchain.h"
#include "renderer/HDRFramebuffer.h"
#include "renderer/BloomPass.h"
#include "renderer/ToneMapPass.h"
#include "renderer/Sync.h"
#include "renderer/Texture.h"
#include "renderer/ShadowPass.h"
#include "renderer/HiZPass.h"
#include "renderer/MegaBuffer.h"
#include "renderer/AsyncComputeManager.h"
#include "renderer/GpuTimer.h"
#include "renderer/ParallelRecorder.h"
#include "renderer/ThreadedCommandPool.h"
#include "input/InputManager.h"
#include "scene/Scene.h"
#include "terrain/IsometricCamera.h"
#include "nav/DebugRenderer.h"
#include "vfx/VFXRenderer.h"
#include "vfx/VFXDefinitionLoader.h"
#include "vfx/VFXFactory.h"
#include "vfx/TrailRenderer.h"
#include "vfx/MeshEffectRenderer.h"
#include "vfx/VFXEventQueue.h"
#include "ability/AbilitySystem.h"
#include "ability/ProjectileSystem.h"
#include "combat/CombatComponents.h"
#include "combat/CombatSystem.h"
#include "combat/GpuCollisionSystem.h"
#include "hud/HUD.h"
#include "hud/PerfOverlay.h"
#include "map/MapTypes.h"

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace glory {

class Window;

class Renderer {
public:
    explicit Renderer(Window& window);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    void simulateStep(float dt);
    void renderFrame(float alpha);
    void waitIdle();

private:
    Window& m_window;

    // ── Vulkan core ───────────────────────────────────────────────────────
    std::unique_ptr<Context>     m_context;
    std::unique_ptr<Device>      m_device;
    std::unique_ptr<Swapchain>   m_swapchain;
    std::unique_ptr<Sync>        m_sync;
    std::unique_ptr<Descriptors> m_descriptors;
    std::unique_ptr<BindlessDescriptors> m_bindless;
    std::unique_ptr<Pipeline>    m_pipeline;   // forward pass, owns renderpass + FBs
    std::unique_ptr<HDRFramebuffer> m_hdrFB;
    std::unique_ptr<BloomPass>      m_bloom;
    std::unique_ptr<ToneMapPass>    m_toneMap;
    DebugRenderer                m_debugRenderer;

    // ── Rendering extras ──────────────────────────────────────────────────
    std::unique_ptr<ClickIndicatorRenderer> m_clickIndicatorRenderer;
    std::unique_ptr<GroundDecalRenderer>    m_groundDecalRenderer;
    std::unique_ptr<DistortionRenderer>     m_distortionRenderer;
    std::unique_ptr<InkingPass>             m_inkingPass;
    std::unique_ptr<FogOfWarRenderer>       m_fogOfWar;
    std::unique_ptr<OutlineRenderer>        m_outlineRenderer;
    std::unique_ptr<WaterRenderer>          m_waterRenderer;
    FogSystem                               m_fogSystem;
    std::unique_ptr<ShieldBubbleRenderer>   m_shieldBubble;
    std::unique_ptr<ConeAbilityRenderer>    m_coneEffect;
    std::unique_ptr<ExplosionRenderer>      m_explosionRenderer;
    std::unique_ptr<SpriteEffectRenderer>   m_spriteEffectRenderer;
    uint32_t m_spriteEffectConeW      = 0;
    uint32_t m_spriteEffectExplosionE = 0;

    // ── W-ability cone effect state ───────────────────────────────────────
    static constexpr float CONE_DURATION   = 0.9f;   // 0.6s wave + 0.3s fade
    static constexpr float CONE_HALF_ANGLE = 0.698f;  // ~40 degrees in radians
    static constexpr float CONE_RANGE      = 15.0f;
    float     m_coneEffectTimer = 0.0f;
    glm::vec3 m_coneDirection   = {0.0f, 0.0f, 1.0f};
    glm::vec3 m_coneApex        = {0.0f, 0.0f, 0.0f};  // latched at cast time
    Texture m_dummyShadow; // 1×1 white — bound to shadow-map slot so calcShadow()=1
    Texture m_toonRamp;   // 256×1 R8G8B8A8_UNORM gradient — bound to toon-ramp slot (binding 5)
    ShadowPass m_shadowPass;

    // ── GPU-driven indirect rendering ─────────────────────────────────────
    struct GpuObjectData {
        glm::mat4 model;
        glm::mat4 normalMatrix;
        glm::vec4 aabbMin;        // xyz = world-space min
        glm::vec4 aabbMax;        // xyz = world-space max
        glm::vec4 tint;
        glm::vec4 params;         // x=shininess, y=metallic, z=roughness, w=emissive
        glm::vec4 texIndices;     // x=diffuseIdx, y=normalIdx
        uint32_t  meshVertexOffset;
        uint32_t  meshIndexOffset;
        uint32_t  meshIndexCount;
        uint32_t  _pad;
    }; // 224 bytes, matches shader ObjectData
    std::unique_ptr<MegaBuffer>  m_megaBuffer;
    HiZPass                      m_hizPass;
    GpuCuller                    m_gpuCuller;
    std::vector<Buffer>          m_sceneBuffers;     // per-frame GpuObjectData[] SSBO
    std::vector<void*>           m_sceneMapped;      // CPU-mapped pointers
    // Per-model, per-submesh MeshHandle for mega-buffer offsets
    std::vector<std::vector<MeshHandle>> m_meshHandles;

    // ── VFX system ────────────────────────────────────────────────────────
    std::unique_ptr<VFXEventQueue> m_vfxQueue;        // SPSC bridge game→render (AbilitySystem)
    std::unique_ptr<VFXEventQueue> m_combatVfxQueue;  // SPSC bridge CombatSystem→render
    std::unique_ptr<VFXRenderer>   m_vfxRenderer;     // GPU particle pipeline
    VFXDefinitionLoader            m_vfxDefLoader;    // JSON definition registry + hot-reload
    VFXFactory                     m_vfxFactory;      // spawn VFX by definition name
    std::unique_ptr<TrailRenderer> m_trailRenderer;   // connected ribbon trails
    std::unique_ptr<MeshEffectRenderer> m_meshEffectRenderer; // geometric mesh VFX
    AsyncComputeManager            m_asyncCompute;    // async compute queue for particles
    ThreadPool                     m_threadPool;      // worker threads for parallel CB recording
    ThreadedCommandPoolManager     m_cmdPools;        // per-thread Vulkan command pools
    std::unique_ptr<AbilitySystem> m_abilitySystem;   // ability state machine
    std::unique_ptr<ProjectileSystem> m_projectileSystem; // moves projectile entities
    std::unique_ptr<CombatSystem>  m_combatSystem;    // auto-attack / shield / trick
    GpuCollisionSystem             m_gpuCollision;    // GPU spatial hash + broadphase

    // ── Scene ─────────────────────────────────────────────────────────────
    Scene            m_scene;
    Camera           m_camera;     // placeholder: feeds InputManager (capture disabled)
    IsometricCamera  m_isoCam;
    std::unique_ptr<InputManager> m_input;

    // ── Frame state ───────────────────────────────────────────────────────
    uint32_t  m_currentFrame  = 0;
    float     m_lastFrameTime = 0.0f;
    float     m_gameTime      = 0.0f;
    float     m_currentDt     = 0.0f;   // dt for the current sim step (set in simulateStep)
    float     m_renderAlpha   = 0.0f;   // interpolation factor for current render frame
    bool      m_showGrid      = false;
    bool      m_wireframe     = false;
    bool      m_showDebugUI   = false;   // Tab-toggled ImGui debug overlay
    bool      m_fogEnabled    = true;    // Fog on/off (toggle from debug UI)
    entt::entity m_playerEntity  = entt::null;
    entt::entity m_hoveredEntity = entt::null;

    struct ClickAnim {
        glm::vec3 position{};
        float     lifetime = 0.0f;
        float     maxLife  = 0.25f;
    };
    std::optional<ClickAnim> m_clickAnim;

    // ── Unit System state ─────────────────────────────────────────────────
    SelectionState m_selection;
    uint32_t m_minionMeshIndex = 0;
    uint32_t m_minionTexIndex = 0;
    uint32_t m_flatNormIndex  = 0;   // shared flat normal map texture index
    Skeleton m_minionSkeleton;
    std::vector<std::vector<SkinVertex>> m_minionSkinVertices;
    std::vector<std::vector<Vertex>> m_minionBindPoseVertices;
    std::vector<AnimationClip> m_minionClips;
    float m_spawnTimer = 0.0f;

    // ── Per-frame CPU→GPU instance buffer (InstanceData per draw call) ────
    static constexpr uint32_t MAX_INSTANCES = 512;
    std::vector<Buffer>  m_instanceBuffers;
    std::vector<void*>   m_instanceMapped;

    // ── Pipelines ─────────────────────────────────────────────────────────
    VkPipelineLayout m_gridPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline       m_gridPipeline          = VK_NULL_HANDLE;
    VkPipelineLayout m_skinnedPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_skinnedPipeline       = VK_NULL_HANDLE;

    // ── ImGui ─────────────────────────────────────────────────────────────
    VkDescriptorPool m_imguiPool = VK_NULL_HANDLE;

    // ── HUD / Minimap ────────────────────────────────────────────────────
    HUD     m_hud;
    MapData m_mapData;

    // ── GPU profiling / perf overlay ─────────────────────────────────────
    std::unique_ptr<GpuTimer> m_gpuTimer;
    PerfOverlay               m_perfOverlay;
    std::vector<GpuTimingResult> m_lastGpuResults;
    float m_lastGpuTotalMs = 0.0f;

    // ── Render graph ─────────────────────────────────────────────────────
    RenderGraph m_renderGraph;

    // Per-pass recording methods (called from render graph execute lambdas)
    void recordShadowPass(VkCommandBuffer cmd, const FrameContext& ctx);
    void recordGBufferPass(VkCommandBuffer cmd, const FrameContext& ctx);
    void recordTransparentVFXPass(VkCommandBuffer cmd, const FrameContext& ctx);
    void recordTonemapPass(VkCommandBuffer cmd, const FrameContext& ctx);
    FrameContext buildFrameContext(VkCommandBuffer cmd, uint32_t imageIndex);

    // ── Helpers ───────────────────────────────────────────────────────────
    void buildScene();
    void recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, float dt);
    void createInstanceBuffers();
    void destroyInstanceBuffers();
    void createGridPipeline();
    void destroyGridPipeline();
    void createSkinnedPipeline();
    void destroySkinnedPipeline();
    glm::vec3 screenToWorld(float mx, float my) const; // unproject to Y=0 plane
    glm::vec2 worldToScreen(const glm::vec3& worldPos) const; // project world to screen pixels

    void spawnTestEnemy();
    entt::entity pickEntityUnderCursor();

    enum class AppState { Launcher, TestMode };
    AppState m_state = AppState::Launcher;

    void drawLauncherUI();
};

} // namespace glory
