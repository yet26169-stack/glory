#pragma once
/// Records all player inputs per simulation tick for deterministic replay.
///
/// The replay file format is:
///   Header: { magic, version, tickRate, playerCount, rngSeed }
///   Frames: sequence of { tick, inputCount, InputFrame[inputCount] }
///   Footer: { totalTicks, finalChecksum }

#include "network/InputSync.h"
#include "core/DeterministicRNG.h"

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>

namespace glory {

struct ReplayHeader {
    uint32_t magic       = 0x52504C59; // "RPLY"
    uint32_t version     = 1;
    uint32_t tickRateHz  = 30;
    uint8_t  playerCount = 1;
    uint8_t  _pad[3]     = {};
    uint64_t rngSeed     = 1;
};

struct ReplayTickRecord {
    uint32_t tick       = 0;
    uint8_t  inputCount = 0;
    InputFrame inputs[MAX_PLAYERS] = {};
};

class ReplayRecorder {
public:
    /// Begin recording a new replay.
    bool begin(const std::string& filePath, uint8_t playerCount, uint64_t rngSeed);

    /// Record one tick's collected inputs.
    void recordTick(uint32_t tick, const CollectedInputFrame& frame);

    /// Finish and flush the replay file.  Writes the footer.
    void end(uint32_t finalChecksum = 0);

    bool isRecording() const { return m_recording; }
    uint32_t ticksRecorded() const { return m_ticksRecorded; }

private:
    std::ofstream m_file;
    bool          m_recording     = false;
    uint32_t      m_ticksRecorded = 0;
    ReplayHeader  m_header{};
};

} // namespace glory
