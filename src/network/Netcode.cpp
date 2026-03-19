#include "network/InputSync.h"
#include "network/NetworkGameLoop.h"
#include "network/StateSnapshot.h"
#include "scene/Components.h"

#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

namespace glory {

// ═══ InputSync.cpp ═══

void InputSynchronizer::init(uint8_t localPlayerId, uint8_t playerCount) {
    m_localPlayerId = localPlayerId;
    m_playerCount = playerCount;
    m_localHistory.clear();
    for (auto& dq : m_remoteHistory) dq.clear();
    m_latestLocal = {};
    spdlog::info("[InputSync] init: localPlayer={}, playerCount={}", localPlayerId, playerCount);
}

void InputSynchronizer::recordLocalInput(const InputFrame& frame) {
    m_latestLocal = frame;
    m_localHistory.push_back(frame);

    while (m_localHistory.size() > 256) {
        m_localHistory.pop_front();
    }
}

void InputSynchronizer::receiveRemoteInput(uint8_t playerId, const InputFrame& frame) {
    if (playerId >= MAX_PLAYERS) {
        spdlog::warn("[InputSync] receiveRemoteInput: invalid playerId={}", playerId);
        return;
    }

    m_remoteHistory[playerId].push_back(frame);

    while (m_remoteHistory[playerId].size() > 256) {
        m_remoteHistory[playerId].pop_front();
    }
}

bool InputSynchronizer::hasAllInputs(uint32_t tick) const {
    // Check local
    bool localFound = false;
    for (const auto& f : m_localHistory) {
        if (f.tick == tick) { localFound = true; break; }
    }
    if (!localFound) return false;

    // Check all remotes
    for (uint8_t p = 0; p < m_playerCount; ++p) {
        if (p == m_localPlayerId) continue;
        bool found = false;
        for (const auto& f : m_remoteHistory[p]) {
            if (f.tick == tick) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

bool InputSynchronizer::getCollectedFrame(uint32_t tick, CollectedInputFrame& out) const {
    out.tick = tick;
    out.playerCount = m_playerCount;

    bool localFound = false;
    for (const auto& f : m_localHistory) {
        if (f.tick == tick) {
            out.inputs[m_localPlayerId] = f;
            localFound = true;
            break;
        }
    }
    if (!localFound) return false;

    for (uint8_t p = 0; p < m_playerCount; ++p) {
        if (p == m_localPlayerId) continue;

        bool found = false;
        for (const auto& f : m_remoteHistory[p]) {
            if (f.tick == tick) {
                out.inputs[p] = f;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

const InputFrame& InputSynchronizer::getLatestLocalInput() const {
    return m_latestLocal;
}

std::vector<uint8_t> InputSynchronizer::serializeInput(const InputPacket& pkt) {
    std::vector<uint8_t> buf(sizeof(InputPacket));
    std::memcpy(buf.data(), &pkt, sizeof(InputPacket));
    return buf;
}

InputPacket InputSynchronizer::deserializeInput(const uint8_t* data, size_t size) {
    InputPacket pkt{};
    if (size >= sizeof(InputPacket)) {
        std::memcpy(&pkt, data, sizeof(InputPacket));
    }
    return pkt;
}

void InputSynchronizer::setInputDelay(uint32_t delayTicks) {
    m_inputDelay = delayTicks;
    spdlog::info("[InputSync] setInputDelay: {} ticks", delayTicks);
}

uint32_t InputSynchronizer::getInputDelay() const {
    return m_inputDelay;
}

uint32_t InputSynchronizer::getBufferedAheadCount(uint32_t confirmedTick) const {
    uint32_t count = 0;
    for (const auto& f : m_localHistory) {
        if (f.tick > confirmedTick) ++count;
    }
    return count;
}

// ═══ StateSnapshot.cpp ═══

// ── Serialization helpers ───────────────────────────────────────────────────

static void writeBytes(std::vector<uint8_t>& buf, const void* src, size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), p, p + n);
}

template<typename T>
static void writeVal(std::vector<uint8_t>& buf, const T& val) {
    writeBytes(buf, &val, sizeof(T));
}

template<typename T>
static T readVal(const uint8_t* buf, size_t& offset) {
    T val;
    std::memcpy(&val, buf + offset, sizeof(T));
    offset += sizeof(T);
    return val;
}

// Serialize a component pool: [count][entity0, comp0, entity1, comp1, ...]
template<typename T>
void StateSnapshot::serializePool(const entt::registry& reg, std::vector<uint8_t>& buf) const {
    auto view = reg.view<T>();
    uint32_t count = static_cast<uint32_t>(view.size());
    writeVal(buf, count);
    for (auto entity : view) {
        uint32_t e = static_cast<uint32_t>(entity);
        writeVal(buf, e);
        const T& comp = view.template get<T>(entity);
        writeBytes(buf, &comp, sizeof(T));
    }
}

template<typename T>
size_t StateSnapshot::deserializePool(entt::registry& reg, const uint8_t* buf, size_t offset) const {
    uint32_t count = readVal<uint32_t>(buf, offset);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t e = readVal<uint32_t>(buf, offset);
        T comp;
        std::memcpy(&comp, buf + offset, sizeof(T));
        offset += sizeof(T);

        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity)) {
            entity = reg.create(entity);
        }
        reg.emplace_or_replace<T>(entity, comp);
    }
    return offset;
}

// ── StateSnapshot ───────────────────────────────────────────────────────────

void StateSnapshot::capture(const entt::registry& reg) {
    data.clear();
    data.reserve(4096);

    // Serialize all gameplay-relevant components (POD types only)
    serializePool<TransformComponent>(reg, data);
    serializePool<UnitComponent>(reg, data);
    serializePool<CharacterComponent>(reg, data);

    checksum = computeChecksum();

    spdlog::trace("[StateSnapshot] capture: tick={}, size={} bytes, checksum=0x{:08X}",
                  tick, data.size(), checksum);
}

void StateSnapshot::restore(entt::registry& reg) const {
    if (data.empty()) {
        spdlog::warn("[StateSnapshot] restore: empty snapshot for tick={}", tick);
        return;
    }

    size_t offset = 0;

    // Restore in same order as capture
    offset = deserializePool<TransformComponent>(reg, data.data(), offset);
    offset = deserializePool<UnitComponent>(reg, data.data(), offset);
    offset = deserializePool<CharacterComponent>(reg, data.data(), offset);

    spdlog::info("[StateSnapshot] restore: tick={}, read {} / {} bytes", tick, offset, data.size());
}

uint32_t StateSnapshot::computeChecksum() const {
    // FNV-1a hash over snapshot data
    uint32_t hash = 2166136261u;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

// ── RollbackManager ─────────────────────────────────────────────────────────

void RollbackManager::saveState(uint32_t tick, const entt::registry& reg) {
    auto& slot = m_history[m_head % ROLLBACK_WINDOW];
    slot.tick = tick;
    slot.capture(reg);

    m_head = (m_head + 1) % ROLLBACK_WINDOW;
    if (m_count < ROLLBACK_WINDOW) ++m_count;
}

bool RollbackManager::rollbackTo(uint32_t tick, entt::registry& reg) {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) {
            m_history[idx].restore(reg);
            spdlog::info("[RollbackManager] rollbackTo: tick={}", tick);
            return true;
        }
    }
    spdlog::warn("[RollbackManager] rollbackTo: tick={} not found", tick);
    return false;
}

bool RollbackManager::hasState(uint32_t tick) const {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) return true;
    }
    return false;
}

uint32_t RollbackManager::oldestTick() const {
    if (m_count == 0) return 0;
    uint32_t idx = (m_head + ROLLBACK_WINDOW - m_count) % ROLLBACK_WINDOW;
    return m_history[idx].tick;
}

uint32_t RollbackManager::newestTick() const {
    if (m_count == 0) return 0;
    uint32_t idx = (m_head + ROLLBACK_WINDOW - 1) % ROLLBACK_WINDOW;
    return m_history[idx].tick;
}

bool RollbackManager::verifyChecksum(uint32_t tick, uint32_t remoteChecksum) const {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) {
            bool match = (m_history[idx].checksum == remoteChecksum);
            if (!match) {
                spdlog::warn("[RollbackManager] DESYNC at tick={}: local=0x{:08X} remote=0x{:08X}",
                             tick, m_history[idx].checksum, remoteChecksum);
            }
            return match;
        }
    }
    return false;
}

uint32_t RollbackManager::getChecksum(uint32_t tick) const {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) return m_history[idx].checksum;
    }
    return 0;
}

void RollbackManager::resimulate(uint32_t fromTick, uint32_t toTick, float dt,
                                 entt::registry& reg, ResimCallback cb) {
    if (!rollbackTo(fromTick, reg)) return;

    spdlog::info("[RollbackManager] resimulating ticks {} → {}", fromTick, toTick);
    for (uint32_t t = fromTick; t < toTick; ++t) {
        cb(reg, t, dt);
        saveState(t + 1, reg);
    }
}

// ═══ NetworkGameLoop.cpp ═══

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
