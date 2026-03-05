#pragma once
#include <cstdint>
#include "math/FixedPoint.h"

namespace glory {

/// Per-tick input from one player, sent to the authoritative server.
struct InputFrame {
    uint32_t  tick;
    uint16_t  playerID;
    uint8_t   buttons;        // bitmask: move | attack | ability1..6
    uint8_t   abilityIndex;   // which ability (0..5)
    SimFloat  targetX, targetZ; // world-space click (fixed-point)
    uint64_t  checksum;       // StateChecksum.hash at this tick (desync detection)
};

/// Server → clients: confirms tick N was processed with a given state hash.
struct TickConfirmation {
    uint32_t tick;
    uint64_t authoritativeChecksum;
};

/// Sent by a client when its local checksum diverges from the server's.
struct DesyncReport {
    uint32_t tick;
    uint16_t playerID;
    uint64_t clientChecksum;
    uint64_t serverChecksum;
};

/// Packet type tags for the Transport layer.
enum class PacketType : uint8_t {
    InputFrame       = 1,
    TickConfirmation = 2,
    DesyncReport     = 3,
    SnapshotData     = 4,
};

} // namespace glory
