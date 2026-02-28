#pragma once

#include "window/Window.h"
#include "renderer/Renderer.h"

#include <string>

namespace glory {

class Application {
public:
    Application(const std::string& name, int width, int height);
    ~Application();

    void run();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

private:
    // Window is declared before Renderer so it outlives the renderer.
    // Renderer explicitly manages surface destruction before instance teardown.
    Window   m_window;
    Renderer m_renderer;
};

} // namespace glory
