#pragma once
#include "net/NetTypes.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace glory {

/// Reliable-UDP transport wrapper.
/// Production implementation requires ENet: add_subdirectory(extern/enet) and
/// target_link_libraries(glory PRIVATE enet). See Appendix C of the AAA plan.
class Transport {
public:
    using ReceiveCallback = std::function<void(uint16_t peerID, PacketType type,
                                               const uint8_t* data, size_t len)>;

    bool startServer(uint16_t port, int maxClients = 16);
    bool connectToServer(const char* host, uint16_t port);
    void poll(ReceiveCallback cb);
    void sendReliable(uint16_t peerID, PacketType type,
                      const uint8_t* data, size_t len);
    void broadcast(PacketType type, const uint8_t* data, size_t len);
    void disconnect(uint16_t peerID);
    void destroy();

    bool isConnected() const { return m_connected; }

private:
    bool     m_connected = false;
    bool     m_isServer  = false;
};

} // namespace glory
