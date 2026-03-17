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

    m_peerConnected.fill(false);
    for (uint8_t i = 0; i < playerCount; ++i)
        m_peerConnected[i] = true;

    // Initialize lobby
    if (role == NetworkRole::Server) {
        m_lobby.initServer(playerCount);
    } else if (role == NetworkRole::Client) {
        m_lobby.initClient();
    }

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

void NetworkGameLoop::pollAndRoute() {
    if (!m_initialized || m_role == NetworkRole::Offline) return;

    auto events = m_transport.poll();

    // Split events: lobby messages go to LobbySystem, gameplay messages handled here
    std::vector<NetworkEvent> lobbyEvents;

    for (auto& ev : events) {
        if (ev.type == NetworkEvent::Type::Connected ||
            ev.type == NetworkEvent::Type::Disconnected) {
            // Connection events always go to lobby
            lobbyEvents.push_back(ev);

            if (ev.type == NetworkEvent::Type::Disconnected && m_role == NetworkRole::Server) {
                // Find which player this peer was
                for (uint8_t i = 0; i < m_lobby.playerCount(); ++i) {
                    if (m_lobby.getSlot(i).peerId == ev.peerId &&
                        m_lobby.getSlot(i).connected) {
                        markPeerDisconnected(i);
                        break;
                    }
                }
            }
            continue;
        }

        if (ev.type == NetworkEvent::Type::DataReceived && !ev.data.empty()) {
            MsgType msgType = peekMsgType(ev.data.data(), ev.data.size());

            // Route by message type
            switch (msgType) {
            case MsgType::LOBBY_ASSIGN_ID:
            case MsgType::LOBBY_HERO_PICK:
            case MsgType::LOBBY_HERO_CONFIRM:
            case MsgType::LOBBY_ALL_READY:
            case MsgType::LOBBY_LOADED:
            case MsgType::LOBBY_START_GAME:
            case MsgType::PEER_DISCONNECT:
            case MsgType::PEER_RECONNECT:
            case MsgType::RECONNECT_REQUEST:
            case MsgType::RECONNECT_ACCEPT:
                lobbyEvents.push_back(std::move(ev));
                break;

            case MsgType::GAME_INPUT:
            case MsgType::GAME_CHECKSUM:
            case MsgType::GAME_FULL_SNAPSHOT:
                processGameplayData(ev);
                break;

            default:
                spdlog::warn("[NetGameLoop] unknown message type 0x{:02X}",
                             static_cast<uint8_t>(msgType));
                break;
            }
        }
    }

    // Process lobby events
    if (!lobbyEvents.empty()) {
        m_lobby.processEvents(m_transport, lobbyEvents);
    }
}

void NetworkGameLoop::preSimulation(uint32_t currentTick) {
    if (!m_initialized) return;
    m_currentTick = currentTick;

    if (m_role == NetworkRole::Offline) return;

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
    // Use the new MsgGameInput format with type header
    MsgGameInput msg;
    msg.playerId = m_lobby.localPlayerId();
    msg.tick = tick;

    const auto& latest = m_inputSync.getLatestLocalInput();
    msg.actionBitmask = latest.buttons;
    float angle = (latest.moveAngle / 65535.0f) * 6.2831853f;
    msg.moveTarget = {std::cos(angle), std::sin(angle)};
    msg.targetEntityId = latest.targetSlot;

    m_transport.broadcast(&msg, sizeof(msg), NetworkChannel::Input, true);
}

void NetworkGameLoop::sendChecksum(uint32_t tick) {
    uint32_t cs = m_rollbackMgr.getChecksum(tick);
    if (cs == 0) return;

    MsgChecksum msg;
    msg.tick     = tick;
    msg.checksum = cs;
    m_transport.broadcast(&msg, sizeof(msg), NetworkChannel::State, false);
}

void NetworkGameLoop::processGameplayData(const NetworkEvent& ev) {
    MsgType msgType = peekMsgType(ev.data.data(), ev.data.size());

    switch (msgType) {
    case MsgType::GAME_INPUT: {
        if (ev.data.size() < sizeof(MsgGameInput)) break;

        MsgGameInput msg;
        std::memcpy(&msg, ev.data.data(), sizeof(msg));

        InputFrame frame{};
        frame.tick       = msg.tick;
        frame.buttons    = static_cast<uint8_t>(msg.actionBitmask);
        frame.targetSlot = static_cast<uint8_t>(msg.targetEntityId);
        float a = std::atan2(msg.moveTarget.y, msg.moveTarget.x);
        if (a < 0) a += 6.2831853f;
        frame.moveAngle = static_cast<uint16_t>((a / 6.2831853f) * 65535.0f);

        m_inputSync.receiveRemoteInput(msg.playerId, frame);

        // Server relays to all other peers
        if (m_role == NetworkRole::Server) {
            m_transport.broadcast(ev.data.data(), ev.data.size(),
                                  NetworkChannel::Input, true);
        }
        break;
    }

    case MsgType::GAME_CHECKSUM: {
        if (ev.data.size() < sizeof(MsgChecksum)) break;

        MsgChecksum msg;
        std::memcpy(&msg, ev.data.data(), sizeof(msg));
        if (!m_rollbackMgr.verifyChecksum(msg.tick, msg.checksum)) {
            spdlog::warn("[NetGameLoop] desync at tick {}", msg.tick);
        }
        break;
    }

    case MsgType::GAME_FULL_SNAPSHOT:
        // TODO: handle full snapshot for reconnection
        spdlog::info("[NetGameLoop] received full snapshot (not yet processed)");
        break;

    default:
        break;
    }
}

void NetworkGameLoop::sendFullSnapshot(uint32_t peerId, uint32_t tick,
                                        const entt::registry& reg) {
    StateSnapshot snap;
    snap.tick = tick;
    snap.capture(reg);

    // Build message: header + snapshot data
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgFullSnapshot) + snap.data.size());

    MsgFullSnapshot header;
    header.tick     = tick;
    header.dataSize = static_cast<uint32_t>(snap.data.size());
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&header),
               reinterpret_cast<const uint8_t*>(&header) + sizeof(header));
    buf.insert(buf.end(), snap.data.begin(), snap.data.end());

    m_transport.send(peerId, buf.data(), buf.size(), NetworkChannel::State, true);
    spdlog::info("[NetGameLoop] sent full snapshot to peer {} (tick {}, {} bytes)",
                 peerId, tick, buf.size());
}

void NetworkGameLoop::markPeerDisconnected(uint8_t playerId) {
    if (playerId < MAX_PLAYERS) {
        m_peerConnected[playerId] = false;
        spdlog::info("[NetGameLoop] player {} marked disconnected (hero stands still)",
                     playerId);
    }
}

void NetworkGameLoop::markPeerReconnected(uint8_t playerId) {
    if (playerId < MAX_PLAYERS) {
        m_peerConnected[playerId] = true;
        spdlog::info("[NetGameLoop] player {} reconnected", playerId);
    }
}

bool NetworkGameLoop::isPeerConnected(uint8_t playerId) const {
    if (playerId >= MAX_PLAYERS) return false;
    return m_peerConnected[playerId];
}

void NetworkGameLoop::handleDesync(uint32_t tick, entt::registry& reg) {
    spdlog::warn("[NetGameLoop] handling desync: rolling back to tick {}", tick);
    m_rollbackMgr.resimulate(tick, m_confirmedTick, 1.0f / 30.0f, reg, nullptr);
}

} // namespace glory
