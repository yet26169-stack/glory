#pragma once
#include "network/Transport.h"
#include "network/InputSync.h"
#include "network/StateSnapshot.h"

namespace glory {

enum class NetworkRole { Offline, Server, Client };

class NetworkGameLoop {
public:
    void init(NetworkRole role, uint8_t localPlayerId = 0, uint8_t playerCount = 1);
    void initTransport(const std::string& host, uint16_t port);
    void shutdown();

    // Called each frame before simulation tick
    void preSimulation(uint32_t currentTick);

    // Called each frame after simulation tick
    void postSimulation(uint32_t currentTick, entt::registry& reg);

    // Check if simulation should advance this frame
    bool shouldAdvanceTick() const;

    // Record local player input for the upcoming tick
    void submitLocalInput(const InputFrame& frame);

    NetworkRole getRole() const { return m_role; }
    uint32_t getCurrentTick() const { return m_currentTick; }
    Transport& transport() { return m_transport; }
    InputSynchronizer& inputSync() { return m_inputSync; }
    RollbackManager& rollback() { return m_rollbackMgr; }

private:
    void sendLocalInput(uint32_t tick);
    void sendChecksum(uint32_t tick);
    void processIncoming();
    void handleDesync(uint32_t tick, entt::registry& reg);

    NetworkRole m_role = NetworkRole::Offline;
    Transport m_transport;
    InputSynchronizer m_inputSync;
    RollbackManager m_rollbackMgr;
    uint32_t m_currentTick = 0;
    uint32_t m_confirmedTick = 0;  // latest tick with all inputs confirmed
    bool m_initialized = false;
    uint16_t m_port = 7777;

    static constexpr uint32_t CHECKSUM_INTERVAL = 10;  // send checksum every N ticks
};

} // namespace glory
