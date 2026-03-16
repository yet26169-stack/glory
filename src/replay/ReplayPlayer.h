#pragma once
/// Replays a previously recorded .rply file by feeding stored inputs into the
/// simulation loop.  The simulation must be seeded with the same RNG seed from
/// the replay header for bit-exact reproduction.

#include "replay/ReplayRecorder.h"  // ReplayHeader, ReplayTickRecord
#include "network/InputSync.h"

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

namespace glory {

class ReplayPlayer {
public:
    /// Load a replay file.  Returns false on error.
    bool load(const std::string& filePath);

    /// Get the header (RNG seed, player count, tick rate).
    const ReplayHeader& header() const { return m_header; }

    /// Get the input frame for the next tick.
    /// Returns false if no more ticks are available (replay ended).
    bool nextTick(CollectedInputFrame& out);

    /// Peek at the tick number of the next frame without consuming it.
    uint32_t peekNextTick() const;

    /// Reset playback to the beginning.
    void rewind();

    bool isLoaded()   const { return m_loaded; }
    bool isFinished() const { return m_cursor >= m_frames.size(); }
    uint32_t totalTicks()   const { return static_cast<uint32_t>(m_frames.size()); }
    uint32_t currentTick()  const { return m_cursor; }
    uint32_t finalChecksum() const { return m_finalChecksum; }

private:
    ReplayHeader m_header{};
    std::vector<ReplayTickRecord> m_frames;
    size_t   m_cursor        = 0;
    uint32_t m_finalChecksum = 0;
    bool     m_loaded        = false;
};

} // namespace glory
