#include "net/GameClient.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace glory {

bool GameClient::connect(const char* host, uint16_t port) {
    return m_transport.connectToServer(host, port);
}

void GameClient::disconnect() {
    m_transport.disconnect(m_playerID);
    m_transport.destroy();
}

void GameClient::update(float /*deltaTime*/, const InputFrame& localInput) {
    // Send local input to server
    m_transport.sendReliable(0 /* server peer */, PacketType::InputFrame,
                              reinterpret_cast<const uint8_t*>(&localInput),
                              sizeof(localInput));

    // Poll for server messages
    m_transport.poll([this](uint16_t peerID, PacketType type,
                             const uint8_t* data, size_t len) {
        onReceive(peerID, type, data, len);
    });
}

void GameClient::onReceive(uint16_t /*peerID*/, PacketType type,
                            const uint8_t* data, size_t len) {
    if (type == PacketType::TickConfirmation && len >= sizeof(TickConfirmation)) {
        TickConfirmation tc;
        std::memcpy(&tc, data, sizeof(TickConfirmation));
        spdlog::debug("Client: tick {} confirmed (server hash {:016x})",
                      tc.tick, tc.authoritativeChecksum);
        // TODO: compare with local checksum; trigger rollback on mismatch
    }
}

void GameClient::rollbackAndResimulate(uint32_t toTick) {
    const StateSnapshot* snap = m_snapshots.get(toTick);
    if (!snap) {
        spdlog::error("GameClient: no snapshot at tick {} for rollback", toTick);
        return;
    }
    // TODO: restore snapshot, re-run simulation ticks up to m_localTick
    spdlog::info("GameClient: rollback to tick {}", toTick);
}

} // namespace glory
