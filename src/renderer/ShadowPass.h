#pragma once

#include "renderer/Device.h"
#include "renderer/Image.h"
#include "renderer/Buffer.h"
#include "renderer/Frustum.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace glory {

class Descriptors;
class IsometricCamera;

// Cascaded Shadow Map pass — renders the scene from the light's perspective
// into a depth-only atlas (one tile per cascade).
//
// Atlas layout: SHADOW_MAP_SIZE × SHADOW_MAP_SIZE per cascade, laid out
// horizontally in a single 2D image (CASCADE_COUNT * SHADOW_MAP_SIZE wide).
class ShadowPass {
public:
    static constexpr uint32_t CASCADE_COUNT   = 3;
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;
    static constexpr VkFormat DEPTH_FORMAT    = VK_FORMAT_D32_SFLOAT;

    // Practical split scheme lambda (0 = uniform, 1 = logarithmic)
    static constexpr float SPLIT_LAMBDA = 0.75f;

    ShadowPass() = default;
    void init(const Device& device, VkDescriptorSetLayout mainLayout);
    void destroy();

    // Compute cascade matrices from the camera's view/projection.
    // lightDir: normalized world-space direction the light shines FROM (e.g. sun direction).
    void updateCascades(const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& lightDir,
                        float nearClip, float farClip);

    // Record shadow depth rendering commands into cmd.
    // Caller must have already bound vertex/index/instance buffers.
    // staticDrawFn: called per cascade with (cmd, cascadeIndex) — draws static meshes
    // skinnedDrawFn: called per cascade with (cmd, cascadeIndex) — draws skinned meshes
    using DrawFn = std::function<void(VkCommandBuffer, uint32_t cascadeIndex)>;
    void recordCommands(VkCommandBuffer cmd,
                        DrawFn staticDrawFn,
                        DrawFn skinnedDrawFn);

    // Bind the shadow atlas to descriptor binding 3 for sampling in the main pass.
    void bindToDescriptors(Descriptors& descriptors);

    // Per-cascade data for the main-pass shader
    struct CascadeData {
        glm::mat4 lightViewProj;
        float splitDepth; // view-space far distance for this cascade
    };
    const std::array<CascadeData, CASCADE_COUNT>& getCascades() const { return m_cascades; }

    VkImageView getAtlasView() const { return m_atlasView; }
    VkSampler   getSampler()   const { return m_sampler; }

    // Pipeline accessors for external draw calls
    VkPipeline       getStaticPipeline()  const { return m_staticPipeline; }
    VkPipeline       getSkinnedPipeline() const { return m_skinnedPipeline; }
    VkPipelineLayout getPipelineLayout()  const { return m_pipelineLayout; }

private:
    const Device* m_device = nullptr;

    // Shadow atlas: CASCADE_COUNT * SHADOW_MAP_SIZE wide, SHADOW_MAP_SIZE tall
    VkImage       m_atlasImage = VK_NULL_HANDLE;
    VmaAllocation m_atlasAlloc = VK_NULL_HANDLE;
    VkImageView   m_atlasView  = VK_NULL_HANDLE;
    VkSampler     m_sampler    = VK_NULL_HANDLE;

    // Depth-only render pass + framebuffer
    VkRenderPass  m_renderPass  = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;

    // Pipelines (depth-only, with push constants for lightViewProj)
    VkPipelineLayout m_pipelineLayout    = VK_NULL_HANDLE;
    VkPipeline       m_staticPipeline    = VK_NULL_HANDLE;
    VkPipeline       m_skinnedPipeline   = VK_NULL_HANDLE;

    // Cascade data
    std::array<CascadeData, CASCADE_COUNT> m_cascades{};

    void createAtlasImage();
    void createSampler();
    void createRenderPass();
    void createFramebuffer();
    void createPipelines(VkDescriptorSetLayout mainLayout);
};

} // namespace glory
