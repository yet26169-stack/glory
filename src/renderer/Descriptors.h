#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <vector>
#include "renderer/Buffer.h"

namespace glory {

class Device;

// binding 0 — vertex stage (per-frame shared data; per-entity model/normal in instance buffer)
struct UniformBufferObject {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::mat4 lightSpaceMatrix;    // cascade 0 (backwards compat)
    alignas(16) glm::mat4 lightSpaceMatrix1;   // cascade 1
    alignas(16) glm::mat4 lightSpaceMatrix2;   // cascade 2
    alignas(16) glm::vec4 cascadeSplits;       // x,y,z = split depths (view-space far dist)
};

// binding 2 — fragment stage
struct PointLightData {
    alignas(16) glm::vec3 position;
    alignas(16) glm::vec3 color;
};

static constexpr uint32_t MAX_LIGHTS = 4;

struct LightUBO {
    PointLightData lights[MAX_LIGHTS];
    alignas(16) glm::vec3 viewPos;
    alignas(4)  int       lightCount       = 0;
    alignas(4)  float     ambientStrength  = 0.15f;
    alignas(4)  float     specularStrength = 0.5f;
    alignas(4)  float     shininess        = 32.0f;
    alignas(16) glm::vec3 fogColor{0.6f, 0.65f, 0.75f};
    alignas(4)  float     fogDensity       = 0.03f;
    alignas(4)  float     fogStart         = 5.0f;
    alignas(4)  float     fogEnd           = 50.0f;

    // ── Toon / stylized shading (LoL/SC2 style) ───────────────────────────
    alignas(16) glm::vec3 rimColor{0.4f, 0.7f, 1.0f};   // team-colour rim (blue team default)
    alignas(4)  float     rimIntensity      = 0.6f;       // rim brightness multiplier
    alignas(4)  float     appTime           = 0.0f;       // elapsed game time for animation
    alignas(4)  float     toonRampSharpness = 0.45f;      // 0=soft LoL look, 1=hard cel-shade
    alignas(4)  float     shadowWarmth      = 0.3f;       // warm/cool shadow tinting weight
    alignas(4)  float     shadowBiasScale   = 1.5f;       // normal-offset bias multiplier (tune to taste)
    alignas(16) glm::vec3 shadowTint{0.15f, 0.12f, 0.25f}; // cool purple shadow tint (LoL style)
    alignas(8) glm::vec2 fowMapMin{0.0f, 0.0f};      // world-space FoW min bounds
    alignas(8) glm::vec2 fowMapMax{200.0f, 200.0f};  // world-space FoW max bounds
};

// ── std140 layout verification ────────────────────────────────────────────────
// Each static_assert confirms C++ offsets match GLSL std140 offsets exactly.
// PointLightData: 32 bytes (vec3@0 + 4-pad + vec3@16 + 4-pad)
static_assert(sizeof(PointLightData)                  ==  32, "PointLightData must be 32 bytes");
static_assert(offsetof(PointLightData, position)      ==   0, "std140: position@0");
static_assert(offsetof(PointLightData, color)         ==  16, "std140: color@16");
// LightUBO: lights[4] at 0–127, then packed fields
static_assert(offsetof(LightUBO, lights)              ==   0, "std140: lights@0");
static_assert(offsetof(LightUBO, viewPos)             == 128, "std140: viewPos@128");
static_assert(offsetof(LightUBO, lightCount)          == 140, "std140: lightCount@140 (packs after vec3)");
static_assert(offsetof(LightUBO, ambientStrength)     == 144, "std140: ambientStrength@144");
static_assert(offsetof(LightUBO, specularStrength)    == 148, "std140: specularStrength@148");
static_assert(offsetof(LightUBO, shininess)           == 152, "std140: shininess@152");
static_assert(offsetof(LightUBO, fogColor)            == 160, "std140: fogColor@160");
static_assert(offsetof(LightUBO, fogDensity)          == 172, "std140: fogDensity@172 (packs after vec3)");
static_assert(offsetof(LightUBO, fogStart)            == 176, "std140: fogStart@176");
static_assert(offsetof(LightUBO, fogEnd)              == 180, "std140: fogEnd@180");
static_assert(offsetof(LightUBO, rimColor)            == 192, "std140: rimColor@192");
static_assert(offsetof(LightUBO, rimIntensity)        == 204, "std140: rimIntensity@204 (packs after vec3)");
static_assert(offsetof(LightUBO, appTime)             == 208, "std140: appTime@208");
static_assert(offsetof(LightUBO, toonRampSharpness)   == 212, "std140: toonRampSharpness@212");
static_assert(offsetof(LightUBO, shadowWarmth)        == 216, "std140: shadowWarmth@216");
static_assert(offsetof(LightUBO, shadowBiasScale)     == 220, "std140: shadowBiasScale@220");
static_assert(offsetof(LightUBO, shadowTint)          == 224, "std140: shadowTint@224");
static_assert(offsetof(LightUBO, fowMapMin)           == 240, "std140: fowMapMin@240");
static_assert(offsetof(LightUBO, fowMapMax)           == 248, "std140: fowMapMax@248");
static_assert(sizeof(LightUBO)                        == 256, "LightUBO must be 256 bytes");

class Descriptors {
public:
    Descriptors(const Device& device, uint32_t frameCount);
    ~Descriptors();

    Descriptors(const Descriptors&)            = delete;
    Descriptors& operator=(const Descriptors&) = delete;

    void cleanup();

    void updateUniformBuffer(uint32_t frameIndex, const UniformBufferObject& ubo);
    void updateLightBuffer(uint32_t frameIndex, const LightUBO& light);
    // Legacy: writes all matrices to slot 0 (single-character shortcut)
    void updateBoneBuffer(uint32_t frameIndex, const std::vector<glm::mat4>& matrices);
    // Ring-buffer API: write bone matrices for a specific character slot (0..MAX_SKINNED_CHARS-1).
    // Returns the bone SSBO byte offset to pass to the vertex shader.
    // NOTE: does NOT flush — call flushBones() once after all slots are written.
    uint32_t writeBoneSlot(uint32_t frameIndex, uint32_t slotIndex,
                           const std::vector<glm::mat4>& matrices);
    // Flush the entire bone SSBO for the given frame to make all per-frame
    // bone writes visible to the GPU in a single coherency operation.
    void flushBones(uint32_t frameIndex);
    void updateShadowMap(VkImageView depthView, VkSampler shadowSampler);
    // binding 5: 1-D toon ramp texture (256×1 R8G8B8A8_UNORM gradient)
    void writeToonRamp(VkImageView rampView, VkSampler rampSampler);
    // binding 6: FoW visibility texture (512×512 R8_UNORM, compute output)
    void writeFogOfWar(VkImageView fowView, VkSampler fowSampler);
    // binding 7: per-frame scene object SSBO (GPU-driven indirect rendering)
    void writeSceneBuffer(uint32_t frameIndex, VkBuffer buffer, VkDeviceSize range);

    VkDescriptorSetLayout getLayout() const { return m_layout; }
    VkDescriptorPool      getPool()   const { return m_pool; }
    VkDescriptorSet       getSet(uint32_t frameIndex) const { return m_sets[frameIndex]; }
    // Returns the bone ring-buffer SSBO for a given frame (used by compute skinning)
    VkBuffer              getBoneBuffer(uint32_t frameIndex) const {
        return m_boneBuffers[frameIndex].getBuffer();
    }

    static constexpr uint32_t MAX_BONES = 256;
    static constexpr uint32_t MAX_SKINNED_CHARS = 128;

private:
    const Device& m_device;
    uint32_t m_frameCount = 0;

    VkDescriptorSetLayout        m_layout = VK_NULL_HANDLE;
    VkDescriptorPool             m_pool   = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;

    // Per-frame uniform buffers (host-visible, persistently mapped via Buffer class)
    std::vector<Buffer> m_uniformBuffers;
    std::vector<Buffer> m_lightBuffers;

    // Per-frame bone matrix SSBOs (binding 4, GPU skinning)
    std::vector<Buffer> m_boneBuffers;

    bool m_cleaned = false;

    void createLayout();
    void createUniformBuffers(uint32_t frameCount);
    void createPool(uint32_t frameCount);
    void createSets(uint32_t frameCount);
};

} // namespace glory
