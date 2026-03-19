#include "replay/ReplayPlayer.h"
#include "replay/ReplayRecorder.h"

#include <cstring>
#include <spdlog/spdlog.h>

namespace glory {

// ═══ ReplayRecorder.cpp ═══

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

// ═══ ReplayPlayer.cpp ═══

bool ReplayPlayer::load(const std::string& filePath) {
    m_frames.clear();
    m_cursor        = 0;
    m_finalChecksum = 0;
    m_loaded        = false;

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("ReplayPlayer: failed to open '{}'", filePath);
        return false;
    }

    file.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
    if (m_header.magic != 0x52504C59) {
        spdlog::error("ReplayPlayer: invalid magic 0x{:08X} in '{}'", m_header.magic, filePath);
        return false;
    }
    if (m_header.version != 1) {
        spdlog::error("ReplayPlayer: unsupported version {} in '{}'", m_header.version, filePath);
        return false;
    }

    // Read frames until we hit the footer (totalTicks + checksum)
    while (file.peek() != EOF) {
        auto pos = file.tellg();
        
        // Peek if we have at least sizeof(ReplayTickRecord) + 8 (footer) left
        file.seekg(0, std::ios::end);
        auto endPos = file.tellg();
        file.seekg(pos);
        
        if (static_cast<size_t>(endPos - pos) <= 8) {
            break; // reached footer
        }

        ReplayTickRecord rec{};
        file.read(reinterpret_cast<char*>(&rec), sizeof(rec));
        if (!file) break;

        m_frames.push_back(rec);
    }

    // Read footer
    uint32_t totalTicks = 0;
    file.seekg(-static_cast<std::streamoff>(sizeof(uint32_t) * 2), std::ios::end);
    file.read(reinterpret_cast<char*>(&totalTicks), sizeof(totalTicks));
    file.read(reinterpret_cast<char*>(&m_finalChecksum), sizeof(m_finalChecksum));

    // Trim frames to match totalTicks (footer detection may have over-read)
    if (m_frames.size() > totalTicks) {
        m_frames.resize(totalTicks);
    }

    m_loaded = true;
    spdlog::info("ReplayPlayer: loaded '{}' — {} ticks, {} players, seed {}, checksum 0x{:08X}",
                 filePath, m_frames.size(), m_header.playerCount, m_header.rngSeed, m_finalChecksum);
    return true;
}

bool ReplayPlayer::nextTick(CollectedInputFrame& out) {
    if (!m_loaded || m_cursor >= m_frames.size()) return false;

    const auto& rec = m_frames[m_cursor];
    out.tick        = rec.tick;
    out.playerCount = rec.inputCount;
    for (uint8_t i = 0; i < rec.inputCount && i < MAX_PLAYERS; ++i)
        out.inputs[i] = rec.inputs[i];

    ++m_cursor;
    return true;
}

uint32_t ReplayPlayer::peekNextTick() const {
    if (!m_loaded || m_cursor >= m_frames.size()) return UINT32_MAX;
    return m_frames[m_cursor].tick;
}

void ReplayPlayer::rewind() {
    m_cursor = 0;
}

} // namespace glory
