#include "renderer/RenderGraph.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace glory {

void RenderGraph::addPass(RenderPassNode pass) {
    m_passes.push_back(std::move(pass));
    m_compiled = false;
}

void RenderGraph::compile() {
    const uint32_t N = static_cast<uint32_t>(m_passes.size());
    if (N == 0) { m_compiled = true; return; }

    // ── Build adjacency from resource dependencies ──────────────────────────
    // For each resource, track which pass last wrote it.
    // A pass that reads a resource depends on the last writer.
    std::unordered_map<ResourceId, uint32_t> lastWriter; // resourceId → pass index
    std::vector<std::vector<uint32_t>> adj(N);           // adj[i] = passes that depend on i
    std::vector<uint32_t> inDegree(N, 0);

    for (uint32_t i = 0; i < N; ++i) {
        const auto& pass = m_passes[i];
        for (const auto& dep : pass.dependencies) {
            if (dep.access == ResourceAccess::Read) {
                auto it = lastWriter.find(dep.id);
                if (it != lastWriter.end() && it->second != i) {
                    // i depends on the writer
                    adj[it->second].push_back(i);
                    ++inDegree[i];
                }
            }
        }
        // Update last writer for writes
        for (const auto& dep : pass.dependencies) {
            if (dep.access == ResourceAccess::Write) {
                lastWriter[dep.id] = i;
            }
        }
    }

    // ── Kahn's algorithm (topological sort) ─────────────────────────────────
    std::queue<uint32_t> ready;
    for (uint32_t i = 0; i < N; ++i) {
        if (inDegree[i] == 0) ready.push(i);
    }

    m_sortedOrder.clear();
    m_sortedOrder.reserve(N);

    while (!ready.empty()) {
        uint32_t cur = ready.front();
        ready.pop();
        m_sortedOrder.push_back(cur);

        for (uint32_t next : adj[cur]) {
            if (--inDegree[next] == 0) {
                ready.push(next);
            }
        }
    }

    if (m_sortedOrder.size() != N) {
        spdlog::error("[RenderGraph] Cycle detected! Falling back to insertion order.");
        m_sortedOrder.resize(N);
        for (uint32_t i = 0; i < N; ++i) m_sortedOrder[i] = i;
    }

    // ── Deduce barriers between passes ──────────────────────────────────────
    deduceBarriers();
    m_compiled = true;

    spdlog::info("[RenderGraph] Compiled {} passes, {} barrier batches",
                 N, m_barriers.size());
}

void RenderGraph::deduceBarriers() {
    m_barriers.clear();
    if (m_sortedOrder.size() < 2) return;

    // Track the last write info for each resource across the sorted order
    struct WriteInfo {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2        access;
        VkImageLayout         layout;
    };
    std::unordered_map<ResourceId, WriteInfo> lastWriteInfo;

    // Initialize from the first pass
    {
        const auto& first = m_passes[m_sortedOrder[0]];
        for (const auto& dep : first.dependencies) {
            if (dep.access == ResourceAccess::Write) {
                lastWriteInfo[dep.id] = {dep.stage, dep.accessMask, dep.layout};
            }
        }
    }

    for (uint32_t orderIdx = 1; orderIdx < static_cast<uint32_t>(m_sortedOrder.size()); ++orderIdx) {
        const auto& pass = m_passes[m_sortedOrder[orderIdx]];
        BarrierBatch batch;
        batch.beforePassIndex = orderIdx;

        for (const auto& dep : pass.dependencies) {
            if (dep.access == ResourceAccess::Read) {
                auto it = lastWriteInfo.find(dep.id);
                if (it != lastWriteInfo.end()) {
                    // Need a barrier: previous write → this read
                    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    mb.srcStageMask  = it->second.stage;
                    mb.srcAccessMask = it->second.access;
                    mb.dstStageMask  = dep.stage;
                    mb.dstAccessMask = dep.accessMask;
                    batch.memoryBarriers.push_back(mb);
                }
            }
        }

        // Update write tracking
        for (const auto& dep : pass.dependencies) {
            if (dep.access == ResourceAccess::Write) {
                lastWriteInfo[dep.id] = {dep.stage, dep.accessMask, dep.layout};
            }
        }

        if (!batch.memoryBarriers.empty() ||
            !batch.imageBarriers.empty() ||
            !batch.bufferBarriers.empty()) {
            m_barriers.push_back(std::move(batch));
        }
    }
}

void RenderGraph::execute(VkCommandBuffer cmd, const FrameContext& ctx) const {
    if (!m_compiled) {
        spdlog::warn("[RenderGraph] execute() called before compile()!");
        // Fall back to insertion order without barriers
        for (const auto& pass : m_passes) {
            if (pass.enabled) pass.execute(cmd, ctx);
        }
        return;
    }

    uint32_t barrierIdx = 0;

    for (uint32_t orderIdx = 0; orderIdx < static_cast<uint32_t>(m_sortedOrder.size()); ++orderIdx) {
        // Insert deduced barriers before this pass
        while (barrierIdx < m_barriers.size() &&
               m_barriers[barrierIdx].beforePassIndex == orderIdx) {
            const auto& batch = m_barriers[barrierIdx];
            VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            depInfo.memoryBarrierCount       = static_cast<uint32_t>(batch.memoryBarriers.size());
            depInfo.pMemoryBarriers          = batch.memoryBarriers.empty() ? nullptr : batch.memoryBarriers.data();
            depInfo.imageMemoryBarrierCount  = static_cast<uint32_t>(batch.imageBarriers.size());
            depInfo.pImageMemoryBarriers     = batch.imageBarriers.empty() ? nullptr : batch.imageBarriers.data();
            depInfo.bufferMemoryBarrierCount = static_cast<uint32_t>(batch.bufferBarriers.size());
            depInfo.pBufferMemoryBarriers    = batch.bufferBarriers.empty() ? nullptr : batch.bufferBarriers.data();
            vkCmdPipelineBarrier2(cmd, &depInfo);
            ++barrierIdx;
        }

        const auto& pass = m_passes[m_sortedOrder[orderIdx]];
        if (pass.enabled) {
            pass.execute(cmd, ctx);
        }
    }
}

void RenderGraph::clear() {
    m_passes.clear();
    m_sortedOrder.clear();
    m_barriers.clear();
    m_compiled = false;
}

void RenderGraph::setPassEnabled(const std::string& name, bool enabled) {
    for (auto& pass : m_passes) {
        if (pass.name == name) {
            pass.enabled = enabled;
            return;
        }
    }
}

RenderPassNode* RenderGraph::findPass(const std::string& name) {
    for (auto& pass : m_passes) {
        if (pass.name == name) return &pass;
    }
    return nullptr;
}

} // namespace glory
