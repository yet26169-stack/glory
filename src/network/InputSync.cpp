#include "network/InputSync.h"
#include <spdlog/spdlog.h>

namespace glory {

void InputSynchronizer::init(uint8_t localPlayerId, uint8_t playerCount) {
    m_localPlayerId = localPlayerId;
    m_playerCount = playerCount;
    m_localHistory.clear();
    for (auto& dq : m_remoteHistory) dq.clear();
    m_latestLocal = {};
    spdlog::info("[InputSync] init: localPlayer={}, playerCount={}", localPlayerId, playerCount);
}

void InputSynchronizer::recordLocalInput(const InputFrame& frame) {
    m_latestLocal = frame;
    m_localHistory.push_back(frame);

    // Keep history bounded
    while (m_localHistory.size() > 256) {
        m_localHistory.pop_front();
    }

    spdlog::trace("[InputSync] recordLocalInput: tick={}, buttons=0x{:02X}",
                  frame.tick, frame.buttons);
}

void InputSynchronizer::receiveRemoteInput(uint8_t playerId, const InputFrame& frame) {
    if (playerId >= MAX_PLAYERS) {
        spdlog::warn("[InputSync] receiveRemoteInput: invalid playerId={}", playerId);
        return;
    }

    m_remoteHistory[playerId].push_back(frame);

    while (m_remoteHistory[playerId].size() > 256) {
        m_remoteHistory[playerId].pop_front();
    }

    spdlog::trace("[InputSync] receiveRemoteInput: player={}, tick={}", playerId, frame.tick);
}

bool InputSynchronizer::getCollectedFrame(uint32_t tick, CollectedInputFrame& out) const {
    out.tick = tick;
    out.playerCount = m_playerCount;

    // Check local input
    bool localFound = false;
    for (const auto& f : m_localHistory) {
        if (f.tick == tick) {
            out.inputs[m_localPlayerId] = f;
            localFound = true;
            break;
        }
    }
    if (!localFound) return false;

    // Check remote inputs
    for (uint8_t p = 0; p < m_playerCount; ++p) {
        if (p == m_localPlayerId) continue;

        bool found = false;
        for (const auto& f : m_remoteHistory[p]) {
            if (f.tick == tick) {
                out.inputs[p] = f;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

const InputFrame& InputSynchronizer::getLatestLocalInput() const {
    return m_latestLocal;
}

void InputSynchronizer::setInputDelay(uint32_t delayTicks) {
    m_inputDelay = delayTicks;
    spdlog::info("[InputSync] setInputDelay: {} ticks", delayTicks);
}

uint32_t InputSynchronizer::getInputDelay() const {
    return m_inputDelay;
}

} // namespace glory
