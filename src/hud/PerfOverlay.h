#pragma once

// ── Performance Overlay ──────────────────────────────────────────────────────
// Compact ImGui window showing per-pass GPU timing, total GPU/CPU frame time,
// and FPS.  Toggled by F3.
// ──────────────────────────────────────────────────────────────────────────────

#include "renderer/GpuTimer.h"

#include <vector>

namespace glory {

class PerfOverlay {
public:
    PerfOverlay() = default;

    // Toggle visibility (wired to F3 key).
    void toggle() { m_visible = !m_visible; }
    bool isVisible() const { return m_visible; }

    // Draw the ImGui window.  Call between ImGui::NewFrame() and ImGui::Render().
    //   gpuResults  – output of GpuTimer::resolve() for the current frame
    //   gpuTotalMs  – GpuTimer::totalMs()
    //   cpuFrameMs  – wall-clock frame time (from FramePacer::getLastFrameMs())
    void draw(const std::vector<GpuTimingResult>& gpuResults,
              float gpuTotalMs,
              float cpuFrameMs);

private:
    bool m_visible = false;
};

} // namespace glory
