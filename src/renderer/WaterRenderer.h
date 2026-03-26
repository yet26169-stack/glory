#pragma once

#include "renderer/Device.h"
#include "renderer/Buffer.h"
#include "renderer/Texture.h"
#include "renderer/BindlessDescriptors.h"
#include "renderer/RenderFormats.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <memory>

namespace glory {

// Flow-map driven water surface renderer (LoL/SC2 style).
//
// Uses two-phase UV distortion driven by a flow map so the water animates
// without visible seams when the phase wraps.  Alpha-blended over the scene;
// depth test ON / depth write OFF so it reads scene depth but doesn't block
// objects behind it.
class WaterRenderer {
public:
    struct WaterPC {
        glm::mat4 model;             // offset  0 (64 bytes)
        float     time;              // offset 64
        float     flowSpeed;         // offset 68
        float     distortionStrength;// offset 72
        float     foamScale;         // offset 76
        int       normalMapIdx;      // offset 80
        int       flowMapIdx;        // offset 84
        int       foamTexIdx;        // offset 88
        int       ssrTexIdx;         // offset 92  (-1 = no SSR)
    };
    static_assert(sizeof(WaterPC) == 96, "WaterPC must be 96 bytes");

    // renderPass   — main HDR render pass (3 attachments, stencil format)
    // mainLayout   — descriptor set layout for the main frame descriptors
    // bindless     — used to register water textures in the bindless array
    void init(const Device&       device,
              const RenderFormats& formats,
              VkDescriptorSetLayout mainLayout,
              VkDescriptorSetLayout bindlessLayout,
              BindlessDescriptors& bindless);

    // cmd      — active command buffer (must be inside the main render pass)
    // mainSet  — descriptor set for the current frame
    // time     — elapsed time in seconds (for animation)
    // model    — world transform of the water plane
    void render(VkCommandBuffer    cmd,
                VkDescriptorSet    mainSet,
                VkDescriptorSet    bindlessSet,
                float              time,
                const glm::mat4&   model);

    // Tunable parameters — set before render() each frame or leave at defaults
    float flowSpeed            = 0.25f;
    float distortionStrength   = 0.07f;
    float foamScale            = 3.5f;

    // SSR texture bindless index (set each frame by Renderer; -1 = disabled)
    int ssrBindlessIdx         = -1;

    void destroy();

private:
    struct WaterVertex {
        glm::vec3 position;
        glm::vec2 uv;
    };

    const Device* m_device = nullptr;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    // Grid mesh
    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount = 0;

    // Procedural water textures (owned by WaterRenderer)
    Texture  m_normalMapTex;
    Texture  m_flowMapTex;
    Texture  m_foamTex;

    // Bindless texture indices (written into the main descriptor array at init)
    int m_normalMapIdx = 0;
    int m_flowMapIdx   = 0;
    int m_foamTexIdx   = 0;

    void createMesh();
    void createTextures(const Device& device,
                        BindlessDescriptors& bindless);
    void createPipeline(const RenderFormats& formats,
                        VkDescriptorSetLayout mainLayout,
                        VkDescriptorSetLayout bindlessLayout);
};

} // namespace glory
