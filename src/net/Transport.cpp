/// Transport stub — replace this file with a real ENet implementation.
/// See extern/enet/ setup instructions in GLORY_ENGINE_AAA_IMPLEMENTATION_PLAN.md.
#include "net/Transport.h"
#include <spdlog/spdlog.h>

namespace glory {

bool Transport::startServer(uint16_t port, int /*maxClients*/) {
    spdlog::warn("Transport::startServer — ENet not integrated yet (port {})", port);
    m_isServer  = true;
    m_connected = false;
    return false;
}

bool Transport::connectToServer(const char* host, uint16_t port) {
    spdlog::warn("Transport::connectToServer — ENet not integrated yet ({}:{})", host, port);
    m_connected = false;
    return false;
}

void Transport::poll(ReceiveCallback /*cb*/) {}

void Transport::sendReliable(uint16_t /*peerID*/, PacketType /*type*/,
                              const uint8_t* /*data*/, size_t /*len*/) {}

void Transport::broadcast(PacketType /*type*/, const uint8_t* /*data*/, size_t /*len*/) {}

void Transport::disconnect(uint16_t /*peerID*/) {}

void Transport::destroy() { m_connected = false; }

} // namespace glory
