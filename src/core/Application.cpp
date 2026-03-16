#include "core/Application.h"
#include "core/Profiler.h"

#include <spdlog/spdlog.h>
#include <chrono>

namespace glory {

Application::Application(const std::string& name, int width, int height,
                         const NetworkConfig& netCfg)
    : m_window(width, height, name)
    , m_renderer(m_window)
    , m_netConfig(netCfg)
{
    spdlog::info("Application '{}' initialized ({}x{})", name, width, height);

    uint8_t localId = (netCfg.role == NetworkRole::Server) ? 0 : 1;
    m_netLoop.init(netCfg.role, localId, netCfg.playerCount);

    if (netCfg.role != NetworkRole::Offline) {
        m_netLoop.initTransport(netCfg.host, netCfg.port);
    }
}

Application::~Application() {
    m_netLoop.shutdown();
    spdlog::info("Application shutting down");
}

void Application::run() {
    spdlog::info("Entering main loop (target: {} fps)", m_pacer.getTargetFps());
    using Clock = std::chrono::steady_clock;
    constexpr double fixedDt      = 1.0 / 60.0;
    constexpr double maxFrameTime = 0.25;
    auto prevTime    = Clock::now();
    double accumulator = 0.0;
    uint32_t simTick = 0;

    while (!m_window.shouldClose()) {
        GLORY_ZONE_N("GameLoop");

        m_pacer.beginFrame();

        m_window.pollEvents();
        auto now = Clock::now();
        double frameTime = std::chrono::duration<double>(now - prevTime).count();
        prevTime = now;
        if (frameTime > maxFrameTime) frameTime = maxFrameTime;
        accumulator += frameTime;
        while (accumulator >= fixedDt) {
            GLORY_ZONE_N("PhysicsStep");

            m_netLoop.preSimulation(simTick);

            if (m_netLoop.shouldAdvanceTick()) {
                m_renderer.simulateStep(static_cast<float>(fixedDt));
                m_netLoop.postSimulation(simTick, m_renderer.getRegistry());
                ++simTick;
            }

            accumulator -= fixedDt;
        }
        m_renderer.renderFrame(static_cast<float>(accumulator / fixedDt));

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
