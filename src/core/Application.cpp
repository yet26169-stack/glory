#include "core/Application.h"

#include <spdlog/spdlog.h>

namespace glory {

Application::Application(const std::string& name, int width, int height)
    : m_window(width, height, name)
    , m_renderer(m_window)
{
    spdlog::info("Application '{}' initialized ({}x{})", name, width, height);
}

Application::~Application() {
    spdlog::info("Application shutting down");
}

void Application::run() {
    spdlog::info("Entering main loop");
    while (!m_window.shouldClose()) {
        m_window.pollEvents();
        m_renderer.drawFrame();
    }
    m_renderer.waitIdle();
    spdlog::info("Main loop ended");
}

} // namespace glory
