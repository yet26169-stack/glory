#include "network/Transport.h"
#include <spdlog/spdlog.h>

namespace glory {

void Transport::initServer(uint16_t port, uint32_t maxClients) {
    spdlog::info("[Transport] initServer stub: port={}, maxClients={}", port, maxClients);
    m_isServer = true;
    m_connected = true;
}

void Transport::initClient(const std::string& host, uint16_t port) {
    spdlog::info("[Transport] initClient stub: host={}, port={}", host, port);
    m_isServer = false;
    m_connected = true;
}

void Transport::send(uint32_t peerId, const void* data, size_t size,
                     NetworkChannel channel, bool reliable) {
    spdlog::trace("[Transport] send stub: peer={}, size={}, channel={}, reliable={}",
                  peerId, size, static_cast<uint8_t>(channel), reliable);
}

void Transport::broadcast(const void* data, size_t size,
                          NetworkChannel channel, bool reliable) {
    spdlog::trace("[Transport] broadcast stub: size={}, channel={}, reliable={}",
                  size, static_cast<uint8_t>(channel), reliable);
}

std::vector<NetworkEvent> Transport::poll(uint32_t timeoutMs) {
    spdlog::trace("[Transport] poll stub: timeoutMs={}", timeoutMs);
    return {};
}

void Transport::disconnect() {
    spdlog::info("[Transport] disconnect stub");
    m_connected = false;
}

bool Transport::isConnected() const {
    return m_connected;
}

bool Transport::isServer() const {
    return m_isServer;
}

} // namespace glory
