#pragma once
#include "net/NetTypes.h"
#include "net/Transport.h"
#include "core/StateSnapshot.h"
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace glory {

/// Authoritative lock-step game server.
/// Waits for InputFrames from all peers, steps simulation, broadcasts
/// TickConfirmations and authoritative snapshots on desync.
class GameServer {
public:
    static constexpr int MAX_PLAYERS   = 16;
    static constexpr int INPUT_TIMEOUT_MS = 16; // max wait for late inputs

    bool start(uint16_t port);
    void shutdown();

    /// Call every frame. Returns true if a simulation tick was stepped.
    bool update(float deltaTime);

    Transport& getTransport() { return m_transport; }

private:
    Transport m_transport;
    SnapshotBuffer m_snapshots;
    uint32_t m_currentTick = 0;
    float    m_accumulator = 0.0f;

    struct PeerInput {
        InputFrame frame{};
        bool       received = false;
    };
    std::unordered_map<uint16_t, PeerInput> m_pendingInputs;

    void onReceive(uint16_t peerID, PacketType type,
                   const uint8_t* data, size_t len);
    void stepTick();
    void broadcastConfirmation(uint64_t checksum);
};

} // namespace glory
