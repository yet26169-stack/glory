#include "hud/PerfOverlay.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace glory {

void PerfOverlay::draw(const std::vector<GpuTimingResult>& gpuResults,
                       float gpuTotalMs,
                       float cpuFrameMs) {
    if (!m_visible) return;

    float fps = cpuFrameMs > 0.001f ? 1000.0f / cpuFrameMs : 0.0f;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("Perf##PerfOverlay", &m_visible, flags)) {
        ImGui::End();
        return;
    }

    // ── Summary row ──────────────────────────────────────────────────────
    ImGui::Text("FPS: %.0f", fps);
    ImGui::SameLine(140);
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "CPU: %.2f ms", cpuFrameMs);

    ImGui::TextColored(ImVec4(0.6f, 0.7f, 1.0f, 1.0f),
                       "GPU total: %.2f ms", gpuTotalMs);
    ImGui::Separator();

    // ── Per-pass GPU bars ────────────────────────────────────────────────
    if (gpuResults.empty()) {
        ImGui::TextDisabled("(no GPU data)");
    } else {
        // Find max for bar scaling
        float maxMs = 0.0f;
        for (auto& r : gpuResults) maxMs = std::max(maxMs, r.ms);
        if (maxMs < 0.01f) maxMs = 0.01f;

        for (auto& r : gpuResults) {
            float fraction = r.ms / maxMs;

            // Color: green → yellow → red based on fraction of max
            ImVec4 barColor;
            if (fraction < 0.5f) {
                barColor = ImVec4(0.2f + fraction, 0.8f, 0.2f, 0.9f);
            } else {
                barColor = ImVec4(0.9f, 1.0f - (fraction - 0.5f) * 1.6f, 0.2f, 0.9f);
            }

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);

            char label[64];
            std::snprintf(label, sizeof(label), "%.3f ms", r.ms);
            ImGui::ProgressBar(fraction, ImVec2(-1, 0), label);

            ImGui::PopStyleColor();
            ImGui::SameLine(0, 0);
            ImGui::Text("  %s", r.name);
        }
    }

    ImGui::End();
}

} // namespace glory
