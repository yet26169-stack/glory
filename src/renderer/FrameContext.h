#pragma once

#include "renderer/Frustum.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace glory {

// Forward declarations for subsystem pointers carried by FrameContext
class Device;
class Descriptors;
class BindlessDescriptors;
class Pipeline;
class HDRFramebuffer;
class Swapchain;
class ShadowPass;
class BloomPass;
class ToneMapPass;
class InkingPass;
class FogOfWarRenderer;
class OutlineRenderer;
class WaterRenderer;
class DistortionRenderer;
class ShieldBubbleRenderer;
class ConeAbilityRenderer;
class ExplosionRenderer;
class SpriteEffectRenderer;
class ClickIndicatorRenderer;
class GroundDecalRenderer;
class MegaBuffer;
class HiZPass;
class VFXRenderer;
class TrailRenderer;
class MeshEffectRenderer;
class DebugRenderer;
class GpuTimer;
class Scene;
class ThreadPool;
class ThreadedCommandPoolManager;
class AsyncComputeManager;
class IsometricCamera;
struct InstanceData;

struct FrameContext {
    // ── Per-frame identification ──
    uint32_t    frameIndex  = 0;   // current frame-in-flight (0 or 1)
    uint32_t    imageIndex  = 0;   // swapchain image index
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkExtent2D  extent      = {};
    float       aspect      = 1.0f;
    float       dt          = 0.0f;
    float       gameTime    = 0.0f;

    // ── Camera ──
    glm::mat4   view        = glm::mat4(1.0f);
    glm::mat4   proj        = glm::mat4(1.0f);
    glm::mat4   viewProj    = glm::mat4(1.0f);
    glm::vec3   cameraPos   = glm::vec3(0.0f);
    float       nearPlane   = 0.1f;
    float       farPlane    = 300.0f;
    Frustum     frustum;

    // ── Subsystem pointers (non-owning, set by Renderer each frame) ──
    const Device*           device          = nullptr;
    Descriptors*            descriptors     = nullptr;
    BindlessDescriptors*    bindless        = nullptr;
    Pipeline*               pipeline        = nullptr;
    HDRFramebuffer*         hdrFB           = nullptr;
    Swapchain*              swapchain       = nullptr;
    ShadowPass*             shadowPass      = nullptr;
    BloomPass*              bloom           = nullptr;
    ToneMapPass*            toneMap         = nullptr;
    InkingPass*             inkingPass      = nullptr;
    FogOfWarRenderer*       fogOfWar        = nullptr;
    OutlineRenderer*        outlineRenderer = nullptr;
    WaterRenderer*          waterRenderer   = nullptr;
    DistortionRenderer*     distortionRenderer = nullptr;
    ShieldBubbleRenderer*   shieldBubble    = nullptr;
    ConeAbilityRenderer*    coneEffect      = nullptr;
    ExplosionRenderer*      explosionRenderer = nullptr;
    SpriteEffectRenderer*   spriteEffects   = nullptr;
    ClickIndicatorRenderer* clickIndicator  = nullptr;
    GroundDecalRenderer*    groundDecals    = nullptr;
    MegaBuffer*             megaBuffer      = nullptr;
    HiZPass*                hizPass         = nullptr;
    VFXRenderer*            vfxRenderer     = nullptr;
    TrailRenderer*          trailRenderer   = nullptr;
    MeshEffectRenderer*     meshEffects     = nullptr;
    DebugRenderer*          debugRenderer   = nullptr;
    GpuTimer*               gpuTimer        = nullptr;
    Scene*                  scene           = nullptr;
    ThreadPool*             threadPool      = nullptr;
    ThreadedCommandPoolManager* cmdPools    = nullptr;
    AsyncComputeManager*    asyncCompute    = nullptr;

    // ── Renderer state (avoids coupling passes to Renderer) ──
    VkPipeline              skinnedPipeline       = VK_NULL_HANDLE;
    VkPipelineLayout        skinnedPipelineLayout = VK_NULL_HANDLE;
    VkPipeline              gridPipeline          = VK_NULL_HANDLE;
    VkPipelineLayout        gridPipelineLayout    = VK_NULL_HANDLE;
    VkBuffer                instanceBuffer        = VK_NULL_HANDLE;
    InstanceData*           instanceMapped         = nullptr;
    void*                   sceneMapped            = nullptr;
    float                   renderAlpha            = 1.0f;
    bool                    wireframe              = false;
    bool                    showGrid               = false;
    bool                    fogEnabled             = true;
    bool                    isLauncher             = false;
};

} // namespace glory
