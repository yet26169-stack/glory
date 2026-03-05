#pragma once
#include "net/NetTypes.h"
#include "net/Transport.h"
#include "core/StateSnapshot.h"
#include <cstdint>

namespace glory {

/// Client-side predict-rollback networking.
/// Predicts simulation forward with local input; rolls back and re-simulates
/// on TickConfirmation checksum mismatch.
class GameClient {
public:
    bool connect(const char* host, uint16_t port);
    void disconnect();

    /// Call every frame with the local input for this tick.
    void update(float deltaTime, const InputFrame& localInput);

    bool isConnected() const { return m_transport.isConnected(); }
    uint32_t currentTick() const { return m_localTick; }

    Transport& getTransport() { return m_transport; }

private:
    Transport      m_transport;
    SnapshotBuffer m_snapshots;
    uint32_t       m_localTick   = 0;
    uint16_t       m_playerID    = 0;

    void onReceive(uint16_t peerID, PacketType type,
                   const uint8_t* data, size_t len);
    void rollbackAndResimulate(uint32_t toTick);
};

} // namespace glory
