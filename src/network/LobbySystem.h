#pragma once
/// LobbySystem: manages pre-game lobby, hero selection, loading sync.
/// Server-authoritative: validates hero picks, prevents duplicates,
/// broadcasts confirmations, waits for all-loaded before starting.

#include "network/NetProtocol.h"
#include "network/Transport.h"

#include <array>
#include <functional>
#include <string>

namespace glory {

static constexpr uint8_t MAX_LOBBY_PLAYERS = 10;

enum class LobbyPhase : uint8_t {
    WAITING_FOR_PLAYERS,  // server waiting for all clients to connect
    HERO_SELECT,          // hero pick phase
    ALL_LOCKED,           // all heroes chosen, server sent ALL_READY
    LOADING,              // building scene
    ALL_LOADED,           // everyone loaded, server sent START_GAME
    IN_GAME               // gameplay running
};

struct LobbySlot {
    bool        connected   = false;
    bool        heroLocked  = false;
    bool        loaded      = false;
    uint32_t    peerId      = 0;        // ENet peer index
    std::string heroId;                  // locked hero ID
};

class LobbySystem {
public:
    void initServer(uint8_t playerCount);
    void initClient();

    /// Process network events during lobby phase. Call each frame.
    void processEvents(Transport& transport, const std::vector<NetworkEvent>& events);

    /// Server: check if all players connected
    bool allPlayersConnected() const;

    /// Server: check if all heroes locked
    bool allHeroesLocked() const;

    /// Server: check if all players loaded
    bool allPlayersLoaded() const;

    /// Client: request a hero pick
    void requestHeroPick(Transport& transport, const std::string& heroId);

    /// Client: notify server that loading is complete
    void notifyLoaded(Transport& transport);

    /// Server: broadcast ALL_READY to start loading
    void broadcastAllReady(Transport& transport);

    /// Server: broadcast START_GAME
    void broadcastStartGame(Transport& transport, uint32_t startTick);

    // ── Accessors ────────────────────────────────────────────────────────
    LobbyPhase phase() const { return m_phase; }
    uint8_t localPlayerId() const { return m_localPlayerId; }
    uint8_t playerCount() const { return m_playerCount; }
    bool isServer() const { return m_isServer; }

    const LobbySlot& getSlot(uint8_t idx) const { return m_slots[idx]; }
    const std::string& getHeroId(uint8_t playerId) const { return m_slots[playerId].heroId; }

    // Callbacks
    std::function<void(uint8_t playerId, const std::string& heroId)> onHeroConfirmed;
    std::function<void()> onAllReady;       // all heroes picked → start loading
    std::function<void()> onStartGame;      // all loaded → transition to IN_GAME
    std::function<void(uint8_t playerId)> onPlayerDisconnected;
    std::function<void(uint8_t playerId)> onPlayerReconnected;

private:
    bool     m_isServer     = false;
    uint8_t  m_localPlayerId = 0;
    uint8_t  m_playerCount   = 2;
    LobbyPhase m_phase       = LobbyPhase::WAITING_FOR_PLAYERS;

    std::array<LobbySlot, MAX_LOBBY_PLAYERS> m_slots;

    // Server-side handlers
    void onClientConnect(Transport& transport, uint32_t peerId);
    void onClientDisconnect(Transport& transport, uint32_t peerId);
    void onHeroPickRequest(Transport& transport, uint32_t peerId, const MsgHeroPick& msg);
    void onLoadedNotify(uint32_t peerId, const MsgLoaded& msg);
    void onReconnectRequest(Transport& transport, uint32_t peerId, const MsgReconnectRequest& msg);

    // Client-side handlers
    void onAssignId(const MsgLobbyAssignId& msg);
    void onHeroConfirmedMsg(const MsgHeroConfirm& msg);
    void onAllReadyMsg();
    void onStartGameMsg(const MsgStartGame& msg);
    void onPeerDisconnect(const MsgPeerStatus& msg);
    void onPeerReconnect(const MsgPeerStatus& msg);

    // Helper: find slot by peerId
    int findSlotByPeer(uint32_t peerId) const;
    bool isHeroTaken(const std::string& heroId) const;
};

} // namespace glory
