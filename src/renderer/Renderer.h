#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayer.h"
#include "animation/Skeleton.h"
#include "camera/Camera.h"
#include "renderer/Buffer.h"
#include "renderer/ClickIndicatorRenderer.h"
#include "renderer/ShieldBubbleRenderer.h"
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "renderer/Context.h"
#include "renderer/Descriptors.h"
#include "renderer/Device.h"
#include "renderer/Pipeline.h"
#include "renderer/Swapchain.h"
#include "renderer/Sync.h"
#include "renderer/Texture.h"
#include "input/InputManager.h"
#include "scene/Scene.h"
#include "terrain/IsometricCamera.h"
#include "nav/DebugRenderer.h"
#include "vfx/VFXRenderer.h"
#include "vfx/VFXEventQueue.h"
#include "ability/AbilitySystem.h"
#include "ability/ProjectileSystem.h"
#include "combat/CombatComponents.h"
#include "combat/CombatSystem.h"

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

    void drawFrame();
    void waitIdle();

private:
    Window& m_window;

    // ── Vulkan core ───────────────────────────────────────────────────────
    std::unique_ptr<Context>     m_context;
    std::unique_ptr<Device>      m_device;
    std::unique_ptr<Swapchain>   m_swapchain;
    std::unique_ptr<Sync>        m_sync;
    std::unique_ptr<Descriptors> m_descriptors;
    std::unique_ptr<Pipeline>    m_pipeline;   // forward pass, owns renderpass + FBs
    DebugRenderer                m_debugRenderer;

    // ── Rendering extras ──────────────────────────────────────────────────
    std::unique_ptr<ClickIndicatorRenderer> m_clickIndicatorRenderer;
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

    // ── VFX system ────────────────────────────────────────────────────────
    std::unique_ptr<VFXEventQueue> m_vfxQueue;        // SPSC bridge game→render (AbilitySystem)
    std::unique_ptr<VFXEventQueue> m_combatVfxQueue;  // SPSC bridge CombatSystem→render
    std::unique_ptr<VFXRenderer>   m_vfxRenderer;     // GPU particle pipeline
    std::unique_ptr<AbilitySystem> m_abilitySystem;   // ability state machine
    std::unique_ptr<ProjectileSystem> m_projectileSystem; // moves projectile entities
    std::unique_ptr<CombatSystem>  m_combatSystem;    // auto-attack / shield / trick

    // ── Scene ─────────────────────────────────────────────────────────────
    Scene            m_scene;
    Camera           m_camera;     // placeholder: feeds InputManager (capture disabled)
    IsometricCamera  m_isoCam;
    std::unique_ptr<InputManager> m_input;

    // ── Frame state ───────────────────────────────────────────────────────
    uint32_t  m_currentFrame  = 0;
    float     m_lastFrameTime = 0.0f;
    float     m_gameTime      = 0.0f;
    float     m_currentDt     = 0.0f;   // dt for the current frame (set in drawFrame)
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
    static constexpr uint32_t MAX_INSTANCES = 256;
    std::vector<Buffer>  m_instanceBuffers;
    std::vector<void*>   m_instanceMapped;

    // ── Pipelines ─────────────────────────────────────────────────────────
    VkPipelineLayout m_gridPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline       m_gridPipeline          = VK_NULL_HANDLE;
    VkPipelineLayout m_skinnedPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_skinnedPipeline       = VK_NULL_HANDLE;

    // ── ImGui ─────────────────────────────────────────────────────────────
    VkDescriptorPool m_imguiPool = VK_NULL_HANDLE;

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
};

} // namespace glory
