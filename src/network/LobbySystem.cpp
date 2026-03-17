#include "network/LobbySystem.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace glory {

// ── Init ────────────────────────────────────────────────────────────────────

void LobbySystem::initServer(uint8_t playerCount) {
    m_isServer    = true;
    m_localPlayerId = 0;
    m_playerCount = playerCount;
    m_phase       = LobbyPhase::WAITING_FOR_PLAYERS;

    // Server is always slot 0 and connected
    m_slots[0].connected = true;
    m_slots[0].peerId    = 0;

    spdlog::info("[Lobby] Server init, waiting for {} players", playerCount);
}

void LobbySystem::initClient() {
    m_isServer = false;
    m_phase    = LobbyPhase::WAITING_FOR_PLAYERS;
    spdlog::info("[Lobby] Client init, waiting for server assignment");
}

// ── Process events ──────────────────────────────────────────────────────────

void LobbySystem::processEvents(Transport& transport,
                                 const std::vector<NetworkEvent>& events) {
    for (auto& ev : events) {
        switch (ev.type) {
        case NetworkEvent::Type::Connected:
            if (m_isServer) {
                onClientConnect(transport, ev.peerId);
            }
            break;

        case NetworkEvent::Type::Disconnected:
            if (m_isServer) {
                onClientDisconnect(transport, ev.peerId);
            }
            break;

        case NetworkEvent::Type::DataReceived:
            if (ev.data.empty()) break;

            MsgType msgType = peekMsgType(ev.data.data(), ev.data.size());

            if (m_isServer) {
                switch (msgType) {
                case MsgType::LOBBY_HERO_PICK:
                    if (ev.data.size() >= sizeof(MsgHeroPick)) {
                        MsgHeroPick msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onHeroPickRequest(transport, ev.peerId, msg);
                    }
                    break;
                case MsgType::LOBBY_LOADED:
                    if (ev.data.size() >= sizeof(MsgLoaded)) {
                        MsgLoaded msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onLoadedNotify(ev.peerId, msg);
                    }
                    break;
                case MsgType::RECONNECT_REQUEST:
                    if (ev.data.size() >= sizeof(MsgReconnectRequest)) {
                        MsgReconnectRequest msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onReconnectRequest(transport, ev.peerId, msg);
                    }
                    break;
                default:
                    break;
                }
            } else {
                // Client handlers
                switch (msgType) {
                case MsgType::LOBBY_ASSIGN_ID:
                    if (ev.data.size() >= sizeof(MsgLobbyAssignId)) {
                        MsgLobbyAssignId msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onAssignId(msg);
                    }
                    break;
                case MsgType::LOBBY_HERO_CONFIRM:
                    if (ev.data.size() >= sizeof(MsgHeroConfirm)) {
                        MsgHeroConfirm msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onHeroConfirmedMsg(msg);
                    }
                    break;
                case MsgType::LOBBY_ALL_READY:
                    onAllReadyMsg();
                    break;
                case MsgType::LOBBY_START_GAME:
                    if (ev.data.size() >= sizeof(MsgStartGame)) {
                        MsgStartGame msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onStartGameMsg(msg);
                    }
                    break;
                case MsgType::PEER_DISCONNECT:
                    if (ev.data.size() >= sizeof(MsgPeerStatus)) {
                        MsgPeerStatus msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onPeerDisconnect(msg);
                    }
                    break;
                case MsgType::PEER_RECONNECT:
                    if (ev.data.size() >= sizeof(MsgPeerStatus)) {
                        MsgPeerStatus msg;
                        std::memcpy(&msg, ev.data.data(), sizeof(msg));
                        onPeerReconnect(msg);
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        }
    }
}

// ── Server-side handlers ────────────────────────────────────────────────────

void LobbySystem::onClientConnect(Transport& transport, uint32_t peerId) {
    // Assign the next free slot
    for (uint8_t i = 1; i < m_playerCount; ++i) {
        if (!m_slots[i].connected) {
            m_slots[i].connected = true;
            m_slots[i].peerId    = peerId;

            // Send assignment to the connecting client
            MsgLobbyAssignId assign;
            assign.playerId    = i;
            assign.playerCount = m_playerCount;
            transport.send(peerId, &assign, sizeof(assign),
                          NetworkChannel::Chat, true);

            spdlog::info("[Lobby] Assigned player {} to peer {}", i, peerId);

            // Check if all connected
            if (allPlayersConnected() &&
                m_phase == LobbyPhase::WAITING_FOR_PLAYERS) {
                m_phase = LobbyPhase::HERO_SELECT;
                spdlog::info("[Lobby] All {} players connected → HERO_SELECT", m_playerCount);
            }
            return;
        }
    }
    spdlog::warn("[Lobby] No free slots for peer {}", peerId);
}

void LobbySystem::onClientDisconnect(Transport& transport, uint32_t peerId) {
    int slot = findSlotByPeer(peerId);
    if (slot < 0) return;

    m_slots[slot].connected = false;
    spdlog::info("[Lobby] Player {} disconnected (peer {})", slot, peerId);

    // Broadcast disconnect to all clients
    MsgPeerStatus msg;
    msg.type     = MsgType::PEER_DISCONNECT;
    msg.playerId = static_cast<uint8_t>(slot);
    transport.broadcast(&msg, sizeof(msg), NetworkChannel::Chat, true);

    if (onPlayerDisconnected) onPlayerDisconnected(static_cast<uint8_t>(slot));
}

void LobbySystem::onHeroPickRequest(Transport& transport, uint32_t peerId,
                                     const MsgHeroPick& msg) {
    int slot = findSlotByPeer(peerId);
    if (slot < 0) return;

    std::string heroId(msg.heroId, strnlen(msg.heroId, sizeof(msg.heroId)));

    MsgHeroConfirm confirm;
    confirm.playerId = static_cast<uint8_t>(slot);
    std::strncpy(confirm.heroId, heroId.c_str(), sizeof(confirm.heroId) - 1);

    if (isHeroTaken(heroId)) {
        confirm.success = 0;
        spdlog::info("[Lobby] Player {} pick rejected (hero '{}' taken)", slot, heroId);
    } else {
        confirm.success = 1;
        m_slots[slot].heroLocked = true;
        m_slots[slot].heroId     = heroId;
        spdlog::info("[Lobby] Player {} locked hero '{}'", slot, heroId);
    }

    // Broadcast confirmation to all (including the requester)
    transport.broadcast(&confirm, sizeof(confirm), NetworkChannel::Chat, true);

    if (onHeroConfirmed)
        onHeroConfirmed(static_cast<uint8_t>(slot), heroId);

    // Check if all heroes locked
    if (allHeroesLocked() && m_phase == LobbyPhase::HERO_SELECT) {
        m_phase = LobbyPhase::ALL_LOCKED;
        broadcastAllReady(transport);
    }
}

void LobbySystem::onLoadedNotify(uint32_t peerId, const MsgLoaded& msg) {
    int slot = findSlotByPeer(peerId);
    if (slot < 0) return;

    m_slots[slot].loaded = true;
    spdlog::info("[Lobby] Player {} finished loading", slot);

    if (allPlayersLoaded() && m_phase == LobbyPhase::LOADING) {
        m_phase = LobbyPhase::ALL_LOADED;
        spdlog::info("[Lobby] All players loaded");
    }
}

void LobbySystem::onReconnectRequest(Transport& transport, uint32_t peerId,
                                      const MsgReconnectRequest& msg) {
    uint8_t playerId = msg.playerId;
    if (playerId >= m_playerCount) return;

    // Re-assign the slot to this peer
    m_slots[playerId].connected = true;
    m_slots[playerId].peerId    = peerId;

    spdlog::info("[Lobby] Player {} reconnected via peer {}", playerId, peerId);

    // Send accept with current state
    MsgReconnectAccept accept;
    accept.playerId    = playerId;
    accept.playerCount = m_playerCount;
    accept.currentTick = 0; // will be filled by NetworkGameLoop
    transport.send(peerId, &accept, sizeof(accept), NetworkChannel::Chat, true);

    // Broadcast reconnect to all
    MsgPeerStatus status;
    status.type     = MsgType::PEER_RECONNECT;
    status.playerId = playerId;
    transport.broadcast(&status, sizeof(status), NetworkChannel::Chat, true);

    if (onPlayerReconnected) onPlayerReconnected(playerId);
}

// ── Client-side handlers ────────────────────────────────────────────────────

void LobbySystem::onAssignId(const MsgLobbyAssignId& msg) {
    m_localPlayerId = msg.playerId;
    m_playerCount   = msg.playerCount;
    m_phase         = LobbyPhase::HERO_SELECT;
    spdlog::info("[Lobby] Assigned as player {} ({} total)", msg.playerId, msg.playerCount);
}

void LobbySystem::onHeroConfirmedMsg(const MsgHeroConfirm& msg) {
    std::string heroId(msg.heroId, strnlen(msg.heroId, sizeof(msg.heroId)));

    if (msg.success) {
        m_slots[msg.playerId].heroLocked = true;
        m_slots[msg.playerId].heroId     = heroId;
        spdlog::info("[Lobby] Player {} locked hero '{}'", msg.playerId, heroId);
    } else {
        spdlog::info("[Lobby] Player {} pick rejected for '{}'", msg.playerId, heroId);
    }

    if (onHeroConfirmed) onHeroConfirmed(msg.playerId, heroId);
}

void LobbySystem::onAllReadyMsg() {
    m_phase = LobbyPhase::ALL_LOCKED;
    spdlog::info("[Lobby] All heroes locked — start loading");
    if (onAllReady) onAllReady();
}

void LobbySystem::onStartGameMsg(const MsgStartGame& /*msg*/) {
    m_phase = LobbyPhase::IN_GAME;
    spdlog::info("[Lobby] START_GAME received — entering gameplay");
    if (onStartGame) onStartGame();
}

void LobbySystem::onPeerDisconnect(const MsgPeerStatus& msg) {
    m_slots[msg.playerId].connected = false;
    spdlog::info("[Lobby] Player {} disconnected", msg.playerId);
    if (onPlayerDisconnected) onPlayerDisconnected(msg.playerId);
}

void LobbySystem::onPeerReconnect(const MsgPeerStatus& msg) {
    m_slots[msg.playerId].connected = true;
    spdlog::info("[Lobby] Player {} reconnected", msg.playerId);
    if (onPlayerReconnected) onPlayerReconnected(msg.playerId);
}

// ── Client actions ──────────────────────────────────────────────────────────

void LobbySystem::requestHeroPick(Transport& transport,
                                   const std::string& heroId) {
    MsgHeroPick msg;
    msg.playerId = m_localPlayerId;
    std::strncpy(msg.heroId, heroId.c_str(), sizeof(msg.heroId) - 1);
    transport.broadcast(&msg, sizeof(msg), NetworkChannel::Chat, true);
    spdlog::info("[Lobby] Requesting hero '{}'", heroId);
}

void LobbySystem::notifyLoaded(Transport& transport) {
    MsgLoaded msg;
    msg.playerId = m_localPlayerId;
    transport.broadcast(&msg, sizeof(msg), NetworkChannel::Chat, true);
    spdlog::info("[Lobby] Notified server: loading complete");
}

void LobbySystem::broadcastAllReady(Transport& transport) {
    MsgAllReady msg;
    transport.broadcast(&msg, sizeof(msg), NetworkChannel::Chat, true);
    m_phase = LobbyPhase::LOADING;
    spdlog::info("[Lobby] Broadcast ALL_READY → LOADING");
    if (onAllReady) onAllReady();
}

void LobbySystem::broadcastStartGame(Transport& transport, uint32_t startTick) {
    MsgStartGame msg;
    msg.startTick = startTick;
    transport.broadcast(&msg, sizeof(msg), NetworkChannel::Chat, true);
    m_phase = LobbyPhase::IN_GAME;
    spdlog::info("[Lobby] Broadcast START_GAME (tick {})", startTick);
    if (onStartGame) onStartGame();
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool LobbySystem::allPlayersConnected() const {
    for (uint8_t i = 0; i < m_playerCount; ++i)
        if (!m_slots[i].connected) return false;
    return true;
}

bool LobbySystem::allHeroesLocked() const {
    for (uint8_t i = 0; i < m_playerCount; ++i)
        if (!m_slots[i].heroLocked) return false;
    return true;
}

bool LobbySystem::allPlayersLoaded() const {
    for (uint8_t i = 0; i < m_playerCount; ++i)
        if (!m_slots[i].loaded) return false;
    return true;
}

int LobbySystem::findSlotByPeer(uint32_t peerId) const {
    for (uint8_t i = 0; i < m_playerCount; ++i)
        if (m_slots[i].connected && m_slots[i].peerId == peerId)
            return i;
    return -1;
}

bool LobbySystem::isHeroTaken(const std::string& heroId) const {
    for (uint8_t i = 0; i < m_playerCount; ++i)
        if (m_slots[i].heroLocked && m_slots[i].heroId == heroId)
            return true;
    return false;
}

// ── Server: handle own hero pick (slot 0) ───────────────────────────────────
// Called by the server's local UI — doesn't go through network.

} // namespace glory
