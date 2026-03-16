#pragma once
#include "core/ThreadPool.h"
#include "renderer/ThreadedCommandPool.h"
#include <functional>
#include <latch>
#include <vector>
#include <algorithm>

namespace glory {

// Threshold: only parallelize if entity count exceeds this
static constexpr uint32_t PARALLEL_THRESHOLD = 64;

struct ParallelRecordResult {
    std::vector<VkCommandBuffer> secondaryBuffers;
};

// Records work across multiple threads using secondary command buffers.
// Falls back to single-threaded recording below PARALLEL_THRESHOLD.
class ParallelRecorder {
public:
    using RecordFn = std::function<void(VkCommandBuffer cmd, uint32_t startIdx, uint32_t endIdx)>;

    // Record entityCount entities across available threads.
    // recordFn is called per-thread with (secondaryCmd, startIndex, endIndex).
    // Returns secondary buffers to execute via vkCmdExecuteCommands.
    static ParallelRecordResult record(
        ThreadPool& pool,
        ThreadedCommandPoolManager& cmdPools,
        uint32_t frameIndex,
        const RenderFormats& formats,
        uint32_t entityCount,
        RecordFn recordFn)
    {
        ParallelRecordResult result;
        uint32_t threadCount = cmdPools.threadCount();

        if (entityCount == 0 || threadCount == 0) return result;

        // Partition entities across threads
        uint32_t entitiesPerThread = (entityCount + threadCount - 1) / threadCount;
        uint32_t actualThreads = (entityCount + entitiesPerThread - 1) / entitiesPerThread;
        actualThreads = std::min(actualThreads, threadCount);

        result.secondaryBuffers.resize(actualThreads);

        std::latch done(actualThreads);

        for (uint32_t t = 0; t < actualThreads; ++t) {
            uint32_t start = t * entitiesPerThread;
            uint32_t end = std::min(start + entitiesPerThread, entityCount);

            pool.submit([&cmdPools, &formats, &recordFn, &result, &done,
                         t, start, end, frameIndex]() {
                auto& res = cmdPools.getResources(t);
                VkCommandBuffer cmd = res.begin(frameIndex, formats);
                recordFn(cmd, start, end);
                ThreadCommandResources::end(cmd);
                result.secondaryBuffers[t] = cmd;
                done.count_down();
            });
        }

        done.wait();
        return result;
    }
};

} // namespace glory
