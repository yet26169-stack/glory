#include "network/NetworkGameLoop.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>

namespace glory {

void NetworkGameLoop::init(NetworkRole role, uint8_t localPlayerId, uint8_t playerCount) {
    m_role = role;
    m_currentTick = 0;
    m_confirmedTick = 0;
    m_inputSync.init(localPlayerId, playerCount);
    m_initialized = true;
    spdlog::info("[NetGameLoop] init: role={}, localPlayer={}, players={}",
                 static_cast<int>(role), localPlayerId, playerCount);
}

void NetworkGameLoop::initTransport(const std::string& host, uint16_t port) {
    m_port = port;
    switch (m_role) {
        case NetworkRole::Server:
            m_transport.initServer(port, 10);
            spdlog::info("[NetGameLoop] listening on port {}", port);
            break;
        case NetworkRole::Client:
            m_transport.initClient(host, port);
            spdlog::info("[NetGameLoop] connecting to {}:{}", host, port);
            break;
        case NetworkRole::Offline:
            break;
    }
}

void NetworkGameLoop::shutdown() {
    if (!m_initialized) return;
    m_transport.disconnect();
    m_initialized = false;
    spdlog::info("[NetGameLoop] shutdown");
}

void NetworkGameLoop::submitLocalInput(const InputFrame& frame) {
    m_inputSync.recordLocalInput(frame);
}

void NetworkGameLoop::preSimulation(uint32_t currentTick) {
    if (!m_initialized) return;
    m_currentTick = currentTick;

    if (m_role == NetworkRole::Offline) return;

    processIncoming();

    // Send local input for future ticks (input delay hiding)
    uint32_t inputDelay = m_inputSync.getInputDelay();
    uint32_t futureTick = currentTick + inputDelay;
    sendLocalInput(futureTick);
}

void NetworkGameLoop::postSimulation(uint32_t currentTick, entt::registry& reg) {
    if (!m_initialized) return;

    if (m_role == NetworkRole::Offline) return;

    m_rollbackMgr.saveState(currentTick, reg);

    if (currentTick % CHECKSUM_INTERVAL == 0) {
        sendChecksum(currentTick);
    }

    m_confirmedTick = currentTick;
}

bool NetworkGameLoop::shouldAdvanceTick() const {
    if (!m_initialized) return false;
    if (m_role == NetworkRole::Offline) return true;

    return m_inputSync.hasAllInputs(m_currentTick);
}

void NetworkGameLoop::sendLocalInput(uint32_t tick) {
    InputPacket pkt{};
    pkt.tick = tick;

    const auto& latest = m_inputSync.getLatestLocalInput();
    pkt.playerId = 0;
    pkt.actionBitmask = latest.buttons;
    float angle = (latest.moveAngle / 65535.0f) * 6.2831853f;
    pkt.moveTarget = {std::cos(angle), std::sin(angle)};
    pkt.targetEntityId = latest.targetSlot;

    auto data = InputSynchronizer::serializeInput(pkt);
    m_transport.broadcast(data.data(), data.size(), NetworkChannel::Input, true);
}

void NetworkGameLoop::sendChecksum(uint32_t tick) {
    uint32_t cs = m_rollbackMgr.getChecksum(tick);
    if (cs == 0) return;

    ChecksumPacket cpkt{tick, cs};
    m_transport.broadcast(reinterpret_cast<const uint8_t*>(&cpkt),
                          sizeof(cpkt), NetworkChannel::State, false);
}

void NetworkGameLoop::processIncoming() {
    auto events = m_transport.poll();

    for (auto& ev : events) {
        switch (ev.type) {
            case NetworkEvent::Type::Connected:
                spdlog::info("[NetGameLoop] peer {} connected", ev.peerId);
                break;

            case NetworkEvent::Type::Disconnected:
                spdlog::info("[NetGameLoop] peer {} disconnected", ev.peerId);
                break;

            case NetworkEvent::Type::DataReceived:
                if (ev.channel == NetworkChannel::Input &&
                    ev.data.size() >= sizeof(InputPacket))
                {
                    auto pkt = InputSynchronizer::deserializeInput(
                        ev.data.data(), ev.data.size());

                    InputFrame frame{};
                    frame.tick = pkt.tick;
                    frame.buttons = static_cast<uint8_t>(pkt.actionBitmask);
                    frame.targetSlot = static_cast<uint8_t>(pkt.targetEntityId);
                    float a = std::atan2(pkt.moveTarget.y, pkt.moveTarget.x);
                    if (a < 0) a += 6.2831853f;
                    frame.moveAngle = static_cast<uint16_t>((a / 6.2831853f) * 65535.0f);

                    m_inputSync.receiveRemoteInput(
                        static_cast<uint8_t>(ev.peerId), frame);

                    // Server relays to all other peers
                    if (m_role == NetworkRole::Server) {
                        m_transport.broadcast(ev.data.data(), ev.data.size(),
                                              NetworkChannel::Input, true);
                    }
                }
                else if (ev.channel == NetworkChannel::State &&
                         ev.data.size() >= sizeof(ChecksumPacket))
                {
                    ChecksumPacket cpkt;
                    std::memcpy(&cpkt, ev.data.data(), sizeof(cpkt));
                    if (!m_rollbackMgr.verifyChecksum(cpkt.tick, cpkt.checksum)) {
                        spdlog::warn("[NetGameLoop] desync at tick {}", cpkt.tick);
                    }
                }
                break;
        }
    }
}

void NetworkGameLoop::handleDesync(uint32_t tick, entt::registry& reg) {
    spdlog::warn("[NetGameLoop] handling desync: rolling back to tick {}", tick);
    m_rollbackMgr.resimulate(tick, m_confirmedTick, 1.0f / 30.0f, reg, nullptr);
}

} // namespace glory
