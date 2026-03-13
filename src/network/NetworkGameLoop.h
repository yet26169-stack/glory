#pragma once
#include "network/Transport.h"
#include "network/InputSync.h"
#include "network/StateSnapshot.h"

namespace glory {

enum class NetworkRole { Offline, Server, Client };

class NetworkGameLoop {
public:
    void init(NetworkRole role, uint8_t localPlayerId = 0, uint8_t playerCount = 1);
    void shutdown();

    // Called each frame before simulation tick
    void preSimulation(uint32_t currentTick);

    // Called each frame after simulation tick
    void postSimulation(uint32_t currentTick, entt::registry& reg);

    // Check if simulation should advance this frame
    bool shouldAdvanceTick() const;

    NetworkRole getRole() const { return m_role; }
    uint32_t getCurrentTick() const { return m_currentTick; }

private:
    NetworkRole m_role = NetworkRole::Offline;
    Transport m_transport;
    InputSynchronizer m_inputSync;
    RollbackManager m_rollbackMgr;
    uint32_t m_currentTick = 0;
    bool m_initialized = false;
};

} // namespace glory
