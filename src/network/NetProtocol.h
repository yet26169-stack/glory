#pragma once
/// NetProtocol: Message types and wire-format structs for the Glory MOBA
/// networking layer. All messages are prefixed with a 1-byte MsgType header.
///
/// Channels: Input(0)=reliable, State(1)=unreliable, Chat(2)=reliable
/// The new Lobby channel piggybacks on Chat (reliable ordered).

#include <glm/glm.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace glory {

// ── Message type header (first byte of every packet) ────────────────────────
enum class MsgType : uint8_t {
    // Lobby phase (Chat channel, reliable)
    LOBBY_ASSIGN_ID       = 0x01,  // Server→Client: your playerId + playerCount
    LOBBY_HERO_PICK       = 0x02,  // Client→Server: I want this hero
    LOBBY_HERO_CONFIRM    = 0x03,  // Server→All: player X locked hero Y
    LOBBY_ALL_READY       = 0x04,  // Server→All: everyone picked, start loading
    LOBBY_LOADED          = 0x05,  // Client→Server: I finished loading
    LOBBY_START_GAME      = 0x06,  // Server→All: everyone loaded, begin IN_GAME

    // Gameplay phase (Input channel, reliable)
    GAME_INPUT            = 0x10,  // Player input packet (existing InputPacket)

    // State sync (State channel, unreliable)
    GAME_CHECKSUM         = 0x20,  // Checksum for desync detection
    GAME_FULL_SNAPSHOT    = 0x21,  // Full state snapshot (for reconnect)

    // Connection management (Chat channel, reliable)
    PEER_DISCONNECT       = 0x30,  // Server→All: player X disconnected
    PEER_RECONNECT        = 0x31,  // Server→All: player X reconnected
    RECONNECT_REQUEST     = 0x32,  // Client→Server: I'm player X, reconnecting
    RECONNECT_ACCEPT      = 0x33,  // Server→Client: welcome back, here's state
};

// ── Lobby messages ──────────────────────────────────────────────────────────

struct MsgLobbyAssignId {
    MsgType  type = MsgType::LOBBY_ASSIGN_ID;
    uint8_t  playerId;
    uint8_t  playerCount;
    uint8_t  _pad = 0;
};
static_assert(sizeof(MsgLobbyAssignId) == 4);

struct MsgHeroPick {
    MsgType  type = MsgType::LOBBY_HERO_PICK;
    uint8_t  playerId;
    uint8_t  _pad[2] = {};
    char     heroId[32] = {};  // null-terminated hero ID string
};

struct MsgHeroConfirm {
    MsgType  type = MsgType::LOBBY_HERO_CONFIRM;
    uint8_t  playerId;
    uint8_t  success;  // 1=locked, 0=rejected (duplicate)
    uint8_t  _pad = 0;
    char     heroId[32] = {};
};

struct MsgAllReady {
    MsgType  type = MsgType::LOBBY_ALL_READY;
    uint8_t  _pad[3] = {};
};

struct MsgLoaded {
    MsgType  type = MsgType::LOBBY_LOADED;
    uint8_t  playerId;
    uint8_t  _pad[2] = {};
};

struct MsgStartGame {
    MsgType  type = MsgType::LOBBY_START_GAME;
    uint8_t  _pad[3] = {};
    uint32_t startTick = 0;  // everyone starts simulation at this tick
};

// ── Gameplay messages ───────────────────────────────────────────────────────

struct MsgGameInput {
    MsgType   type = MsgType::GAME_INPUT;
    uint8_t   playerId;
    uint8_t   _pad[2] = {};
    uint32_t  tick;
    uint32_t  actionBitmask;
    glm::vec2 moveTarget;
    uint32_t  targetEntityId;
};
static_assert(sizeof(MsgGameInput) == 24);

struct MsgChecksum {
    MsgType  type = MsgType::GAME_CHECKSUM;
    uint8_t  _pad[3] = {};
    uint32_t tick;
    uint32_t checksum;
};

struct MsgFullSnapshot {
    MsgType  type = MsgType::GAME_FULL_SNAPSHOT;
    uint8_t  _pad[3] = {};
    uint32_t tick;
    uint32_t dataSize;
    // followed by dataSize bytes of snapshot data
};

// ── Connection management messages ──────────────────────────────────────────

struct MsgPeerStatus {
    MsgType  type;  // PEER_DISCONNECT or PEER_RECONNECT
    uint8_t  playerId;
    uint8_t  _pad[2] = {};
};

struct MsgReconnectRequest {
    MsgType  type = MsgType::RECONNECT_REQUEST;
    uint8_t  playerId;  // "I was player N"
    uint8_t  _pad[2] = {};
};

struct MsgReconnectAccept {
    MsgType  type = MsgType::RECONNECT_ACCEPT;
    uint8_t  playerId;
    uint8_t  playerCount;
    uint8_t  _pad = 0;
    uint32_t currentTick;
    // followed by hero assignments and full state snapshot
};

// ── Helper: read MsgType from raw buffer ────────────────────────────────────
inline MsgType peekMsgType(const uint8_t* data, size_t size) {
    if (size == 0) return static_cast<MsgType>(0xFF);
    return static_cast<MsgType>(data[0]);
}

} // namespace glory
