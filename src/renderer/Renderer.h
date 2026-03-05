#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayer.h"
#include "animation/Skeleton.h"
#include "camera/Camera.h"
#include "renderer/Buffer.h"
#include "renderer/ClickIndicatorRenderer.h"
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

    // ── Rendering extras ──────────────────────────────────────────────────
    std::unique_ptr<ClickIndicatorRenderer> m_clickIndicatorRenderer;
    Texture m_dummyShadow; // 1×1 white — bound to shadow-map slot so calcShadow()=1

    // ── Scene ─────────────────────────────────────────────────────────────
    Scene            m_scene;
    Camera           m_camera;     // placeholder: feeds InputManager (capture disabled)
    IsometricCamera  m_isoCam;
    std::unique_ptr<InputManager> m_input;

    // ── Frame state ───────────────────────────────────────────────────────
    uint32_t  m_currentFrame  = 0;
    float     m_lastFrameTime = 0.0f;
    float     m_gameTime      = 0.0f;
    bool      m_showGrid      = false;
    bool      m_wireframe     = false;
    entt::entity m_playerEntity = entt::null;

    struct ClickAnim {
        glm::vec3 position{};
        float     lifetime = 0.0f;
        float     maxLife  = 0.25f;
    };
    std::optional<ClickAnim> m_clickAnim;

    // ── Per-frame CPU→GPU instance buffer (InstanceData per draw call) ────
    static constexpr uint32_t MAX_INSTANCES = 256;
    std::vector<Buffer>  m_instanceBuffers;
    std::vector<void*>   m_instanceMapped;

    // ── Pipelines ─────────────────────────────────────────────────────────
    VkPipelineLayout m_gridPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline       m_gridPipeline          = VK_NULL_HANDLE;
    VkPipelineLayout m_skinnedPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_skinnedPipeline       = VK_NULL_HANDLE;

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
};

} // namespace glory
