#include "core/Application.h"
#include "core/Profiler.h"

#include <spdlog/spdlog.h>
#include <chrono>

namespace glory {

Application::Application(const std::string& name,
                         const NetworkConfig& netCfg,
                         const GameConfig& gameConfig)
    : m_window(gameConfig.windowWidth, gameConfig.windowHeight, name)
    , m_renderer(m_window)
    , m_netConfig(netCfg)
    , m_gameConfig(gameConfig)
    , m_stateMachine(m_renderer, m_window)
{
    spdlog::info("Application '{}' initialized ({}x{}, fps={})",
                 name, gameConfig.windowWidth, gameConfig.windowHeight,
                 gameConfig.targetFps);

    m_pacer.setTargetFps(gameConfig.targetFps);

    // Wire nexus-death → post-game victory screen
    m_renderer.setVictoryCallback([this](uint8_t winningTeam) {
        m_stateMachine.setVictory(winningTeam == 0); // team 0 = player team
        m_stateMachine.transition(GameStateType::POST_GAME);
    });

    uint8_t localId = (netCfg.role == NetworkRole::Server) ? 0 : 1;
    m_netLoop.init(netCfg.role, localId, netCfg.playerCount);

    if (netCfg.role != NetworkRole::Offline) {
        m_netLoop.initTransport(netCfg.host, netCfg.port);
        wireLobbyCallbacks();
    }

    // Expose lobby to the state machine so HeroSelectState can do network picks
    m_stateMachine.setNetworkGameLoop(&m_netLoop);
}

Application::~Application() {
    m_netLoop.shutdown();
    spdlog::info("Application shutting down");
}

void Application::wireLobbyCallbacks() {
    auto& lobby = m_netLoop.lobby();

    // When all heroes are locked, transition everyone to LOADING
    lobby.onAllReady = [this]() {
        spdlog::info("[App] All heroes locked → LOADING");
        m_stateMachine.transition(GameStateType::LOADING);
    };

    // When all players loaded, transition to IN_GAME
    lobby.onStartGame = [this]() {
        spdlog::info("[App] All players loaded → IN_GAME");
        m_stateMachine.transition(GameStateType::IN_GAME);
    };

    // When a player disconnects during gameplay, their hero stands still
    lobby.onPlayerDisconnected = [this](uint8_t playerId) {
        m_netLoop.markPeerDisconnected(playerId);
    };

    // When a player reconnects, send full snapshot
    lobby.onPlayerReconnected = [this](uint8_t playerId) {
        m_netLoop.markPeerReconnected(playerId);
        // Send full state snapshot to help them catch up
        auto& slot = m_netLoop.lobby().getSlot(playerId);
        if (m_netLoop.lobby().phase() == LobbyPhase::IN_GAME) {
            m_netLoop.sendFullSnapshot(slot.peerId,
                                        m_netLoop.getCurrentTick(),
                                        m_renderer.getRegistry());
        }
    };
}

void Application::run() {
    spdlog::info("Entering main loop (target: {} fps)", m_pacer.getTargetFps());
    using Clock = std::chrono::steady_clock;
    constexpr double fixedDt      = 1.0 / 60.0;
    constexpr double maxFrameTime = 0.25;
    auto prevTime    = Clock::now();
    double accumulator = 0.0;
    uint32_t simTick = 0;

    while (!m_window.shouldClose() && !m_stateMachine.shouldQuit()) {
        GLORY_ZONE_N("GameLoop");

        m_pacer.beginFrame();

        m_window.pollEvents();
        auto now = Clock::now();
        double frameTime = std::chrono::duration<double>(now - prevTime).count();
        prevTime = now;
        if (frameTime > maxFrameTime) frameTime = maxFrameTime;
        accumulator += frameTime;

        // Poll network events every frame (lobby + gameplay routing)
        m_netLoop.pollAndRoute();

        while (accumulator >= fixedDt) {
            GLORY_ZONE_N("PhysicsStep");

            if (m_stateMachine.currentState() == GameStateType::IN_GAME) {
                m_netLoop.preSimulation(simTick);
                if (m_netLoop.shouldAdvanceTick()) {
                    m_stateMachine.update(static_cast<float>(fixedDt));
                    m_netLoop.postSimulation(simTick, m_renderer.getRegistry());
                    ++simTick;
                }
            } else {
                m_stateMachine.update(static_cast<float>(fixedDt));
            }

            accumulator -= fixedDt;
        }
        m_stateMachine.render(static_cast<float>(accumulator / fixedDt));

        {
            GLORY_ZONE_N("FramePacerSleep");
            m_pacer.endFrame();
        }

        GLORY_FRAME_MARK;
    }
    m_renderer.waitIdle();
    spdlog::info("Main loop ended");
}

} // namespace glory
