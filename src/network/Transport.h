#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace glory {

enum class NetworkChannel : uint8_t {
    Input    = 0,  // reliable ordered
    State    = 1,  // unreliable sequenced
    Chat     = 2,  // reliable ordered
    COUNT    = 3
};

struct NetworkEvent {
    enum class Type { Connected, Disconnected, DataReceived };
    Type type;
    uint32_t peerId;
    std::vector<uint8_t> data;
    NetworkChannel channel;
};

class Transport {
public:
    void initServer(uint16_t port, uint32_t maxClients);
    void initClient(const std::string& host, uint16_t port);
    void send(uint32_t peerId, const void* data, size_t size, NetworkChannel channel, bool reliable);
    void broadcast(const void* data, size_t size, NetworkChannel channel, bool reliable);
    std::vector<NetworkEvent> poll(uint32_t timeoutMs = 0);
    void disconnect();
    bool isConnected() const;
    bool isServer() const;

private:
    bool m_isServer = false;
    bool m_connected = false;
};

} // namespace glory
