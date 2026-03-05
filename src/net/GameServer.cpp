#include "net/GameServer.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace glory {

bool GameServer::start(uint16_t port) {
    return m_transport.startServer(port, MAX_PLAYERS);
}

void GameServer::shutdown() {
    m_transport.destroy();
}

bool GameServer::update(float /*deltaTime*/) {
    m_transport.poll([this](uint16_t peerID, PacketType type,
                             const uint8_t* data, size_t len) {
        onReceive(peerID, type, data, len);
    });
    return false; // tick not stepped (stub)
}

void GameServer::onReceive(uint16_t peerID, PacketType type,
                            const uint8_t* data, size_t len) {
    if (type == PacketType::InputFrame && len >= sizeof(InputFrame)) {
        InputFrame frame;
        std::memcpy(&frame, data, sizeof(InputFrame));
        m_pendingInputs[peerID] = PeerInput{frame, true};
        spdlog::debug("Server: received input from peer {} tick {}", peerID, frame.tick);
    }
}

void GameServer::stepTick() {
    // TODO: apply all pending inputs, step simulation, broadcast TickConfirmation
    ++m_currentTick;
    m_pendingInputs.clear();
}

void GameServer::broadcastConfirmation(uint64_t checksum) {
    TickConfirmation tc{m_currentTick, checksum};
    m_transport.broadcast(PacketType::TickConfirmation,
                          reinterpret_cast<const uint8_t*>(&tc), sizeof(tc));
}

} // namespace glory
