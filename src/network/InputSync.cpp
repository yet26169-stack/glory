#include "network/InputSync.h"
#include <spdlog/spdlog.h>
#include <cstring>

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

    while (m_localHistory.size() > 256) {
        m_localHistory.pop_front();
    }
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
}

bool InputSynchronizer::hasAllInputs(uint32_t tick) const {
    // Check local
    bool localFound = false;
    for (const auto& f : m_localHistory) {
        if (f.tick == tick) { localFound = true; break; }
    }
    if (!localFound) return false;

    // Check all remotes
    for (uint8_t p = 0; p < m_playerCount; ++p) {
        if (p == m_localPlayerId) continue;
        bool found = false;
        for (const auto& f : m_remoteHistory[p]) {
            if (f.tick == tick) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

bool InputSynchronizer::getCollectedFrame(uint32_t tick, CollectedInputFrame& out) const {
    out.tick = tick;
    out.playerCount = m_playerCount;

    bool localFound = false;
    for (const auto& f : m_localHistory) {
        if (f.tick == tick) {
            out.inputs[m_localPlayerId] = f;
            localFound = true;
            break;
        }
    }
    if (!localFound) return false;

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

std::vector<uint8_t> InputSynchronizer::serializeInput(const InputPacket& pkt) {
    std::vector<uint8_t> buf(sizeof(InputPacket));
    std::memcpy(buf.data(), &pkt, sizeof(InputPacket));
    return buf;
}

InputPacket InputSynchronizer::deserializeInput(const uint8_t* data, size_t size) {
    InputPacket pkt{};
    if (size >= sizeof(InputPacket)) {
        std::memcpy(&pkt, data, sizeof(InputPacket));
    }
    return pkt;
}

void InputSynchronizer::setInputDelay(uint32_t delayTicks) {
    m_inputDelay = delayTicks;
    spdlog::info("[InputSync] setInputDelay: {} ticks", delayTicks);
}

uint32_t InputSynchronizer::getInputDelay() const {
    return m_inputDelay;
}

uint32_t InputSynchronizer::getBufferedAheadCount(uint32_t confirmedTick) const {
    uint32_t count = 0;
    for (const auto& f : m_localHistory) {
        if (f.tick > confirmedTick) ++count;
    }
    return count;
}

} // namespace glory
