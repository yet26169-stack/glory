#include "replay/ReplayRecorder.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace glory {

bool ReplayRecorder::begin(const std::string& filePath, uint8_t playerCount, uint64_t rngSeed) {
    if (m_recording) end();

    m_file.open(filePath, std::ios::binary | std::ios::trunc);
    if (!m_file.is_open()) {
        spdlog::error("ReplayRecorder: failed to open '{}'", filePath);
        return false;
    }

    m_header.playerCount = playerCount;
    m_header.rngSeed     = rngSeed;
    m_header.tickRateHz   = 30;

    m_file.write(reinterpret_cast<const char*>(&m_header), sizeof(m_header));
    m_ticksRecorded = 0;
    m_recording     = true;

    spdlog::info("ReplayRecorder: started recording to '{}' ({} players, seed {})",
                 filePath, playerCount, rngSeed);
    return true;
}

void ReplayRecorder::recordTick(uint32_t tick, const CollectedInputFrame& frame) {
    if (!m_recording) return;

    ReplayTickRecord rec{};
    rec.tick       = tick;
    rec.inputCount = frame.playerCount;
    for (uint8_t i = 0; i < frame.playerCount && i < MAX_PLAYERS; ++i)
        rec.inputs[i] = frame.inputs[i];

    m_file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    ++m_ticksRecorded;
}

void ReplayRecorder::end(uint32_t finalChecksum) {
    if (!m_recording) return;

    // Write footer: total ticks + checksum
    m_file.write(reinterpret_cast<const char*>(&m_ticksRecorded), sizeof(m_ticksRecorded));
    m_file.write(reinterpret_cast<const char*>(&finalChecksum), sizeof(finalChecksum));

    m_file.flush();
    m_file.close();
    m_recording = false;

    spdlog::info("ReplayRecorder: finished — {} ticks recorded (checksum: 0x{:08X})",
                 m_ticksRecorded, finalChecksum);
}

} // namespace glory
