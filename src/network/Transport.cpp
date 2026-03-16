#include "network/Transport.h"
#include <enet/enet.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace glory {

bool Transport::s_enetInitialized = false;

void Transport::ensureEnetInit() {
    if (!s_enetInitialized) {
        if (enet_initialize() != 0) {
            throw std::runtime_error("Failed to initialize ENet");
        }
        std::atexit(enet_deinitialize);
        s_enetInitialized = true;
    }
}

Transport::Transport()  { ensureEnetInit(); }
Transport::~Transport() { disconnect(); }

void Transport::initServer(uint16_t port, uint32_t maxClients) {
    disconnect();

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    m_host = enet_host_create(&address, maxClients,
                              static_cast<size_t>(NetworkChannel::COUNT),
                              0, 0);
    if (!m_host) {
        spdlog::error("[Transport] Failed to create server on port {}", port);
        return;
    }

    m_isServer  = true;
    m_connected = true;
    m_localPeerId = 0;  // server is always peer 0
    spdlog::info("[Transport] Server listening on port {} (max {} clients)", port, maxClients);
}

void Transport::initClient(const std::string& host, uint16_t port) {
    disconnect();

    m_host = enet_host_create(nullptr, 1,
                              static_cast<size_t>(NetworkChannel::COUNT),
                              0, 0);
    if (!m_host) {
        spdlog::error("[Transport] Failed to create client host");
        return;
    }

    ENetAddress address{};
    enet_address_set_host(&address, host.c_str());
    address.port = port;

    m_server = enet_host_connect(m_host, &address,
                                 static_cast<size_t>(NetworkChannel::COUNT), 0);
    if (!m_server) {
        spdlog::error("[Transport] Failed to initiate connection to {}:{}", host, port);
        enet_host_destroy(m_host);
        m_host = nullptr;
        return;
    }

    m_isServer = false;
    spdlog::info("[Transport] Connecting to {}:{} ...", host, port);
}

void Transport::send(uint32_t peerId, const void* data, size_t size,
                     NetworkChannel channel, bool reliable) {
    if (!m_host) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                              : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);

    if (m_isServer) {
        // Server sends to a specific peer by ID
        if (peerId < m_host->peerCount) {
            enet_peer_send(&m_host->peers[peerId],
                           static_cast<enet_uint8>(channel), packet);
        }
    } else {
        // Client sends to the server
        if (m_server) {
            enet_peer_send(m_server, static_cast<enet_uint8>(channel), packet);
        }
    }
}

void Transport::broadcast(const void* data, size_t size,
                          NetworkChannel channel, bool reliable) {
    if (!m_host) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                              : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    enet_host_broadcast(m_host, static_cast<enet_uint8>(channel), packet);
}

std::vector<NetworkEvent> Transport::poll(uint32_t timeoutMs) {
    std::vector<NetworkEvent> events;
    if (!m_host) return events;

    ENetEvent ev;
    while (enet_host_service(m_host, &ev, timeoutMs) > 0) {
        timeoutMs = 0;  // only wait on the first call

        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                uint32_t pid = static_cast<uint32_t>(ev.peer - m_host->peers);
                spdlog::info("[Transport] Peer {} connected ({}:{})",
                             pid, ev.peer->address.host, ev.peer->address.port);
                if (!m_isServer) {
                    m_connected = true;
                    m_localPeerId = pid;
                }
                events.push_back({NetworkEvent::Type::Connected, pid, {}, NetworkChannel::Input});
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                uint32_t pid = static_cast<uint32_t>(ev.peer - m_host->peers);
                spdlog::info("[Transport] Peer {} disconnected", pid);
                if (!m_isServer) m_connected = false;
                events.push_back({NetworkEvent::Type::Disconnected, pid, {}, NetworkChannel::Input});
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                uint32_t pid = static_cast<uint32_t>(ev.peer - m_host->peers);
                auto ch = static_cast<NetworkChannel>(ev.channelID);
                std::vector<uint8_t> buf(ev.packet->data,
                                         ev.packet->data + ev.packet->dataLength);
                events.push_back({NetworkEvent::Type::DataReceived, pid, std::move(buf), ch});
                enet_packet_destroy(ev.packet);
                break;
            }
            default:
                break;
        }
    }

    return events;
}

void Transport::flush() {
    if (m_host) enet_host_flush(m_host);
}

void Transport::disconnect() {
    if (!m_host) return;

    if (m_server) {
        enet_peer_disconnect(m_server, 0);
        // Drain events for a brief period to allow graceful disconnect
        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 200) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(ev.packet);
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
        m_server = nullptr;
    }

    enet_host_destroy(m_host);
    m_host      = nullptr;
    m_connected = false;
    spdlog::info("[Transport] Disconnected");
}

bool Transport::isConnected() const { return m_connected; }
bool Transport::isServer()    const { return m_isServer; }

uint32_t Transport::peerCount() const {
    if (!m_host) return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < m_host->peerCount; ++i) {
        if (m_host->peers[i].state == ENET_PEER_STATE_CONNECTED) ++count;
    }
    return count;
}

} // namespace glory
