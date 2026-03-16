#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <deque>
#include <glm/glm.hpp>

namespace glory {

// Compact input frame — 8 bytes, used internally
struct InputFrame {
    uint32_t tick;
    uint16_t moveAngle;    // quantized 0-65535 → 0-360°
    uint8_t  buttons;      // Q=0x01, W=0x02, E=0x04, R=0x08, Attack=0x10, Move=0x20
    uint8_t  targetSlot;   // target entity slot (0-255)
};

// Wire-format input packet sent over the network
struct InputPacket {
    uint32_t  tick;
    uint8_t   playerId;
    uint8_t   _pad[3];
    uint32_t  actionBitmask;    // ability/action flags
    glm::vec2 moveTarget;      // world-space move target
    uint32_t  targetEntityId;  // 0 = no target
};
static_assert(sizeof(InputPacket) == 24, "InputPacket size check");

// Checksum packet for desync detection (sent unreliable)
struct ChecksumPacket {
    uint32_t tick;
    uint32_t checksum;
};

static constexpr uint32_t MAX_PLAYERS = 10;

struct CollectedInputFrame {
    uint32_t tick;
    std::array<InputFrame, MAX_PLAYERS> inputs;
    uint8_t playerCount;
};

class InputSynchronizer {
public:
    void init(uint8_t localPlayerId, uint8_t playerCount);

    // Record local input for a tick
    void recordLocalInput(const InputFrame& frame);

    // Receive remote player's input
    void receiveRemoteInput(uint8_t playerId, const InputFrame& frame);

    // Get collected frame for a tick (returns false if not all inputs received)
    bool getCollectedFrame(uint32_t tick, CollectedInputFrame& out) const;

    // Check if all inputs for a tick are available
    bool hasAllInputs(uint32_t tick) const;

    // Get latest local input for prediction
    const InputFrame& getLatestLocalInput() const;

    // Serialize/deserialize for network
    static std::vector<uint8_t> serializeInput(const InputPacket& pkt);
    static InputPacket deserializeInput(const uint8_t* data, size_t size);

    // Input delay (in ticks) for lockstep
    void setInputDelay(uint32_t delayTicks);
    uint32_t getInputDelay() const;

    // Get the number of future ticks buffered locally ahead of confirmed
    uint32_t getBufferedAheadCount(uint32_t confirmedTick) const;

private:
    uint8_t m_localPlayerId = 0;
    uint8_t m_playerCount = 1;
    uint32_t m_inputDelay = 2;  // 2 ticks default (~66ms at 30Hz)
    std::deque<InputFrame> m_localHistory;
    std::array<std::deque<InputFrame>, MAX_PLAYERS> m_remoteHistory;
    InputFrame m_latestLocal{};
};

} // namespace glory
