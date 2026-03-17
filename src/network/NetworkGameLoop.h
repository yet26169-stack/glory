#pragma once
#include "network/Transport.h"
#include "network/InputSync.h"
#include "network/StateSnapshot.h"
#include "network/LobbySystem.h"
#include "network/NetProtocol.h"

namespace glory {

enum class NetworkRole { Offline, Server, Client };

class NetworkGameLoop {
public:
    void init(NetworkRole role, uint8_t localPlayerId = 0, uint8_t playerCount = 1);
    void initTransport(const std::string& host, uint16_t port);
    void shutdown();

    /// Poll network events and route to lobby or gameplay systems.
    /// Call every frame regardless of game state.
    void pollAndRoute();

    // Called each frame before simulation tick (IN_GAME only)
    void preSimulation(uint32_t currentTick);

    // Called each frame after simulation tick
    void postSimulation(uint32_t currentTick, entt::registry& reg);

    // Check if simulation should advance this frame
    bool shouldAdvanceTick() const;

    // Record local player input for the upcoming tick
    void submitLocalInput(const InputFrame& frame);

    // Send a full state snapshot to a specific peer (for reconnection)
    void sendFullSnapshot(uint32_t peerId, uint32_t tick, const entt::registry& reg);

    NetworkRole getRole() const { return m_role; }
    uint32_t getCurrentTick() const { return m_currentTick; }
    Transport& transport() { return m_transport; }
    InputSynchronizer& inputSync() { return m_inputSync; }
    RollbackManager& rollback() { return m_rollbackMgr; }
    LobbySystem& lobby() { return m_lobby; }

    /// Mark a peer as disconnected — their hero stands still
    void markPeerDisconnected(uint8_t playerId);

    /// Mark a peer as reconnected
    void markPeerReconnected(uint8_t playerId);

    bool isPeerConnected(uint8_t playerId) const;

private:
    void sendLocalInput(uint32_t tick);
    void sendChecksum(uint32_t tick);
    void processGameplayData(const NetworkEvent& ev);
    void handleDesync(uint32_t tick, entt::registry& reg);

    NetworkRole m_role = NetworkRole::Offline;
    Transport m_transport;
    InputSynchronizer m_inputSync;
    RollbackManager m_rollbackMgr;
    LobbySystem m_lobby;
    uint32_t m_currentTick = 0;
    uint32_t m_confirmedTick = 0;
    bool m_initialized = false;
    uint16_t m_port = 7777;

    // Per-player connection status for disconnect handling
    std::array<bool, MAX_PLAYERS> m_peerConnected{};

    static constexpr uint32_t CHECKSUM_INTERVAL = 10;
};

} // namespace glory
