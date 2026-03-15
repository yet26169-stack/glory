#pragma once

#include "renderer/FrameContext.h"

#include <vulkan/vulkan.h>
#include <functional>
#include <string>
#include <vector>

namespace glory {

// A single render pass node in the frame graph.
// Each pass records GPU commands into the provided command buffer.
struct RenderPassNode {
    std::string name;
    std::function<void(VkCommandBuffer, const FrameContext&)> execute;

    // Resource dependencies (names of outputs from other passes).
    // Currently informational — used for future automatic barrier insertion.
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;

    bool enabled = true;
};

// A lightweight frame graph that orders and executes render passes.
//
// Current implementation: linear execution in insertion order.
// Future: topological sort based on input/output dependencies,
//         automatic VkImageMemoryBarrier insertion between passes,
//         and parallel secondary command buffer recording.
class RenderGraph {
public:
    void addPass(RenderPassNode pass) {
        m_passes.push_back(std::move(pass));
    }

    // Placeholder for future dependency-driven compilation.
    void compile() {
        // Currently a no-op. Passes execute in insertion order.
        // Future: topological sort, barrier deduction, pass merging.
    }

    void execute(VkCommandBuffer cmd, const FrameContext& ctx) const {
        for (const auto& pass : m_passes) {
            if (pass.enabled) {
                pass.execute(cmd, ctx);
            }
        }
    }

    void clear() { m_passes.clear(); }

    size_t passCount() const { return m_passes.size(); }

    // Enable/disable a pass by name
    void setPassEnabled(const std::string& name, bool enabled) {
        for (auto& pass : m_passes) {
            if (pass.name == name) {
                pass.enabled = enabled;
                return;
            }
        }
    }

    // Access a pass by name (nullptr if not found)
    RenderPassNode* findPass(const std::string& name) {
        for (auto& pass : m_passes) {
            if (pass.name == name) return &pass;
        }
        return nullptr;
    }

private:
    std::vector<RenderPassNode> m_passes;
};

} // namespace glory
