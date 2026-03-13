#include "network/NetworkGameLoop.h"
#include <spdlog/spdlog.h>

namespace glory {

void NetworkGameLoop::init(NetworkRole role, uint8_t localPlayerId, uint8_t playerCount) {
    m_role = role;
    m_currentTick = 0;
    m_inputSync.init(localPlayerId, playerCount);

    switch (role) {
        case NetworkRole::Offline:
            spdlog::info("[NetworkGameLoop] init: Offline mode");
            break;
        case NetworkRole::Server:
            spdlog::info("[NetworkGameLoop] init: Server mode, players={}", playerCount);
            break;
        case NetworkRole::Client:
            spdlog::info("[NetworkGameLoop] init: Client mode, localPlayer={}", localPlayerId);
            break;
    }

    m_initialized = true;
}

void NetworkGameLoop::shutdown() {
    if (!m_initialized) return;

    spdlog::info("[NetworkGameLoop] shutdown");
    m_transport.disconnect();
    m_initialized = false;
}

void NetworkGameLoop::preSimulation(uint32_t currentTick) {
    if (!m_initialized) return;

    m_currentTick = currentTick;

    if (m_role == NetworkRole::Offline) return;

    // Poll transport for incoming packets
    auto events = m_transport.poll();
    for (const auto& event : events) {
        switch (event.type) {
            case NetworkEvent::Type::Connected:
                spdlog::info("[NetworkGameLoop] peer {} connected", event.peerId);
                break;
            case NetworkEvent::Type::Disconnected:
                spdlog::info("[NetworkGameLoop] peer {} disconnected", event.peerId);
                break;
            case NetworkEvent::Type::DataReceived:
                spdlog::trace("[NetworkGameLoop] data from peer {}: {} bytes on channel {}",
                              event.peerId, event.data.size(),
                              static_cast<uint8_t>(event.channel));
                break;
        }
    }
}

void NetworkGameLoop::postSimulation(uint32_t currentTick, entt::registry& reg) {
    if (!m_initialized) return;

    m_currentTick = currentTick;

    // Save state for rollback
    if (m_role != NetworkRole::Offline) {
        m_rollbackMgr.saveState(currentTick, reg);
    }

    spdlog::trace("[NetworkGameLoop] postSimulation: tick={}", currentTick);
}

bool NetworkGameLoop::shouldAdvanceTick() const {
    if (!m_initialized) return false;

    // Offline mode always advances
    if (m_role == NetworkRole::Offline) return true;

    // Stub: in a real implementation, check if all inputs are available
    // For now, always allow advancement
    return true;
}

} // namespace glory
