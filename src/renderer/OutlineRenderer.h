#pragma once

#include "renderer/Device.h"
#include "renderer/RenderFormats.h"
#include "renderer/StaticSkinnedMesh.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <memory>

namespace glory {

// Two-pass stencil outline renderer (LoL Stage 8).
//
// Pass 1 — Stencil Write:
//   Renders selected mesh into the stencil buffer (ref = 1, op = REPLACE).
//   Color writes are disabled; depth test ON so occluded geometry doesn't mark
//   stencil.
//
// Pass 2 — Outline Shell:
//   Renders the same mesh with normals inflated by outlineScale, front-face
//   culling (back-face shell), stencil NOT_EQUAL ref=1.  Only pixels belonging
//   to the outline fringe (not covered by the original silhouette) are drawn.
class OutlineRenderer {
public:
    ~OutlineRenderer() { destroy(); }
    // Push constant layout matching GLSL std430 order:
    //   boneBaseIndex @ offset  0 (4 bytes)
    //   outlineScale  @ offset  4 (4 bytes)
    //   [8 bytes implicit padding — std430 aligns vec4 to 16]
    //   outlineColor  @ offset 16 (16 bytes) → total 32 bytes
    struct OutlinePC {
        uint32_t  boneBaseIndex;  // offset  0
        float     outlineScale;   // offset  4
        float     _pad[2];        // offset  8  (explicit padding to guarantee vec4 at 16)
        glm::vec4 outlineColor;   // offset 16
    };
    static_assert(sizeof(OutlinePC) == 32, "OutlinePC must be 32 bytes");

    // renderPass must be the main HDR render pass (3-attachment, stencil format).
    // mainLayout is the VkDescriptorSetLayout used by the skinned pipeline so
    // the same descriptor sets can be re-bound.
    void init(const Device& device,
              const RenderFormats& formats,
              VkDescriptorSetLayout mainLayout);

    // Render one selected entity's outline.
    // Call this AFTER the skinned draw pass (within the same render pass).
    // cmd        — active command buffer
    // ds         — descriptor set bound for the main frame (skinned pipeline set 0)
    // instBuf    — instance buffer for this frame
    // instOffset — byte offset into instBuf for this entity's InstanceData
    // boneBase   — boneBaseIndex for this entity (pc.boneBaseIndex)
    // mesh       — the entity's StaticSkinnedMesh
    // outlineScale — world-space normal-inflation amount (e.g. 0.03)
    // outlineColor — RGBA; default blue-team colour (0.2, 0.5, 1.0, 1.0)
    void renderOutline(VkCommandBuffer cmd,
                       VkDescriptorSet ds,
                       VkBuffer        instBuf,
                       VkDeviceSize    instOffset,
                       uint32_t        boneBase,
                       const StaticSkinnedMesh& mesh,
                       float           outlineScale,
                       const glm::vec4& outlineColor);

    void destroy();

private:
    const Device* m_device = nullptr;

    VkPipelineLayout m_outlineLayout          = VK_NULL_HANDLE;
    VkPipeline       m_stencilWritePipeline   = VK_NULL_HANDLE;
    VkPipeline       m_outlineDrawPipeline    = VK_NULL_HANDLE;

    void createPipelineLayout(VkDescriptorSetLayout mainLayout);
    void createPipelines(const RenderFormats& formats);
};

} // namespace glory
