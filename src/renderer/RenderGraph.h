#pragma once

#include "renderer/FrameContext.h"

#include <vulkan/vulkan.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <concepts>

namespace glory {

// ── Resource identifier (hash of resource name for fast comparison) ──────────
using ResourceId = uint32_t;

constexpr ResourceId makeResourceId(std::string_view name) noexcept {
    // FNV-1a 32-bit hash
    uint32_t hash = 2166136261u;
    for (char c : name) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

// Well-known resource IDs (used by passes to declare dependencies)
namespace res {
    inline constexpr ResourceId ShadowMap      = makeResourceId("ShadowMap");
    inline constexpr ResourceId HdrColor       = makeResourceId("HdrColor");
    inline constexpr ResourceId HdrDepth       = makeResourceId("HdrDepth");
    inline constexpr ResourceId CharDepth      = makeResourceId("CharDepth");
    inline constexpr ResourceId FowMask        = makeResourceId("FowMask");
    inline constexpr ResourceId HiZPyramid     = makeResourceId("HiZPyramid");
    inline constexpr ResourceId IndirectBuffer = makeResourceId("IndirectBuffer");
    inline constexpr ResourceId ParticleBuffer = makeResourceId("ParticleBuffer");
    inline constexpr ResourceId BloomMips      = makeResourceId("BloomMips");
    inline constexpr ResourceId SwapchainImage = makeResourceId("SwapchainImage");
    inline constexpr ResourceId SceneCopyColor = makeResourceId("SceneCopyColor");
}

// ── C++20 concept: any pass type must be executable with a command buffer + context
template<typename T>
concept Renderable = requires(T t, VkCommandBuffer cmd, const FrameContext& ctx) {
    { t.execute(cmd, ctx) };
    { t.resources() };
};

// ── Resource access declaration for barrier deduction ────────────────────────
enum class ResourceAccess : uint8_t {
    Read,
    Write,
};

struct ResourceDependency {
    ResourceId     id;
    ResourceAccess access;
    VkPipelineStageFlags2   stage      = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2          accessMask = VK_ACCESS_2_NONE;
    VkImageLayout           layout     = VK_IMAGE_LAYOUT_UNDEFINED; // for images
};

// ── Render pass node ─────────────────────────────────────────────────────────
struct RenderPassNode {
    std::string name;
    std::function<void(VkCommandBuffer, const FrameContext&)> execute;

    // Resource dependencies for automatic barrier insertion
    std::vector<ResourceDependency> dependencies;

    bool enabled = true;

    // Convenience: declare a read dependency
    RenderPassNode& reads(ResourceId id,
                          VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VkAccessFlags2 access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        dependencies.push_back({id, ResourceAccess::Read, stage, access, layout});
        return *this;
    }

    // Convenience: declare a write dependency
    RenderPassNode& writes(ResourceId id,
                           VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VkAccessFlags2 access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        dependencies.push_back({id, ResourceAccess::Write, stage, access, layout});
        return *this;
    }
};

// ── Render graph (DAG with topological sort + automatic barrier insertion) ───
class RenderGraph {
public:
    void addPass(RenderPassNode pass);

    // Topological sort via Kahn's algorithm + barrier deduction.
    // Must be called after all passes are added and before execute().
    void compile();

    // Execute all enabled passes in compiled order, inserting barriers.
    void execute(VkCommandBuffer cmd, const FrameContext& ctx) const;

    void clear();

    size_t passCount() const { return m_passes.size(); }

    // Enable/disable a pass by name
    void setPassEnabled(const std::string& name, bool enabled);

    // Access a pass by name (nullptr if not found)
    RenderPassNode* findPass(const std::string& name);

private:
    std::vector<RenderPassNode> m_passes;
    std::vector<uint32_t>       m_sortedOrder;  // indices into m_passes after compile()
    bool                        m_compiled = false;

    // Deduced barriers between consecutive passes
    struct BarrierBatch {
        uint32_t beforePassIndex; // execute this batch before pass at m_sortedOrder[beforePassIndex]
        std::vector<VkImageMemoryBarrier2>  imageBarriers;
        std::vector<VkBufferMemoryBarrier2> bufferBarriers;
        std::vector<VkMemoryBarrier2>       memoryBarriers;
    };
    std::vector<BarrierBatch> m_barriers;

    void deduceBarriers();
};

} // namespace glory
