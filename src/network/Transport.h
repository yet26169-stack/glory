#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct _ENetHost;
struct _ENetPeer;

namespace glory {

enum class NetworkChannel : uint8_t {
    Input    = 0,  // reliable ordered — lockstep input packets
    State    = 1,  // unreliable sequenced — state checksums
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
    Transport();
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    void initServer(uint16_t port, uint32_t maxClients);
    void initClient(const std::string& host, uint16_t port);
    void send(uint32_t peerId, const void* data, size_t size, NetworkChannel channel, bool reliable);
    void broadcast(const void* data, size_t size, NetworkChannel channel, bool reliable);
    std::vector<NetworkEvent> poll(uint32_t timeoutMs = 0);
    void disconnect();
    void flush();
    bool isConnected() const;
    bool isServer() const;
    uint32_t localPeerId() const { return m_localPeerId; }
    uint32_t peerCount() const;

private:
    _ENetHost* m_host   = nullptr;
    _ENetPeer* m_server = nullptr;  // client-side: the server peer
    bool m_isServer     = false;
    bool m_connected    = false;
    uint32_t m_localPeerId = 0;
    static bool s_enetInitialized;
    static void ensureEnetInit();
};

} // namespace glory
