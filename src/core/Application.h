#pragma once

#include "core/FramePacer.h"
#include "window/Window.h"
#include "renderer/Renderer.h"

#include <string>

namespace glory {

class Application {
public:
    Application(const std::string& name, int width, int height);
    ~Application();

    void run();

    // 0 = uncapped; call before run() or at any time during run().
    void setTargetFps(int fps) { m_pacer.setTargetFps(fps); }
    int  getTargetFps() const  { return m_pacer.getTargetFps(); }

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

private:
    // Window is declared before Renderer so it outlives the renderer.
    // Renderer explicitly manages surface destruction before instance teardown.
    Window     m_window;
    Renderer   m_renderer;
    FramePacer m_pacer{60}; // default 60 fps cap
};

} // namespace glory
