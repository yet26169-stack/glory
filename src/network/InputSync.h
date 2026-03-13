#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <deque>

namespace glory {

// Compact input frame — 8 bytes total
struct InputFrame {
    uint32_t tick;
    uint16_t moveAngle;    // quantized 0-65535 → 0-360°
    uint8_t  buttons;      // Q=0x01, W=0x02, E=0x04, R=0x08, Attack=0x10, Move=0x20
    uint8_t  targetSlot;   // target entity slot (0-255)
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

    // Get latest local input for prediction
    const InputFrame& getLatestLocalInput() const;

    // Input delay (in ticks) for lockstep
    void setInputDelay(uint32_t delayTicks);
    uint32_t getInputDelay() const;

private:
    uint8_t m_localPlayerId = 0;
    uint8_t m_playerCount = 1;
    uint32_t m_inputDelay = 2;  // 2 ticks default (~66ms at 30Hz)
    std::deque<InputFrame> m_localHistory;
    std::array<std::deque<InputFrame>, MAX_PLAYERS> m_remoteHistory;
    InputFrame m_latestLocal{};
};

} // namespace glory
