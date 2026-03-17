#include "hud/RespawnOverlay.h"
#include "combat/RespawnSystem.h"

#include <cstdio>

namespace glory {

bool RespawnOverlay::render(const entt::registry& reg, entt::entity player,
                             float screenW, float screenH) {
    if (player == entt::null) return false;

    auto* rc = reg.try_get<RespawnComponent>(player);
    if (!rc || rc->state == LifeState::ALIVE) return false;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return false;

    // Semi-transparent dark overlay at top center
    float overlayW = 320.0f;
    float overlayH = 120.0f;
    float ox = (screenW - overlayW) * 0.5f;
    float oy = screenH * 0.3f;

    dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + overlayW, oy + overlayH),
                       IM_COL32(0, 0, 0, 180), 8.0f);
    dl->AddRect(ImVec2(ox, oy), ImVec2(ox + overlayW, oy + overlayH),
                IM_COL32(180, 50, 50, 255), 8.0f, 0, 2.0f);

    // "YOU DIED" text
    const char* deathText = "YOU DIED";
    ImVec2 textSize = ImGui::CalcTextSize(deathText);
    dl->AddText(nullptr, 24.0f,
                ImVec2(ox + (overlayW - textSize.x * 1.7f) * 0.5f, oy + 8.0f),
                IM_COL32(220, 60, 60, 255), deathText);

    // Countdown timer
    if (rc->state == LifeState::DYING) {
        dl->AddText(nullptr, 16.0f,
                    ImVec2(ox + overlayW * 0.5f - 40.0f, oy + 45.0f),
                    IM_COL32(200, 200, 200, 255), "Dying...");
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Respawn in %.1fs", rc->respawnTimer);
        ImVec2 timerSize = ImGui::CalcTextSize(buf);
        dl->AddText(nullptr, 18.0f,
                    ImVec2(ox + (overlayW - timerSize.x * 1.3f) * 0.5f, oy + 45.0f),
                    IM_COL32(255, 255, 200, 255), buf);

        // Progress bar
        float barW = overlayW - 40.0f;
        float barH = 8.0f;
        float barX = ox + 20.0f;
        float barY = oy + 72.0f;
        float progress = (rc->totalRespawn > 0.0f)
                         ? 1.0f - (rc->respawnTimer / rc->totalRespawn)
                         : 0.0f;
        progress = std::max(0.0f, std::min(1.0f, progress));

        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                           IM_COL32(40, 40, 40, 200), 3.0f);
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * progress, barY + barH),
                           IM_COL32(100, 200, 100, 255), 3.0f);
    }

    // "Spectate Allies" button
    bool spectateClicked = false;
    ImGui::SetNextWindowPos(ImVec2(ox + (overlayW - 140.0f) * 0.5f, oy + overlayH - 32.0f));
    ImGui::SetNextWindowSize(ImVec2(140.0f, 28.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.2f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.3f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.4f, 0.6f, 1.0f));
    if (ImGui::Begin("##respawn_spectate", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::Button("Spectate Allies", ImVec2(136.0f, 24.0f)))
            spectateClicked = true;
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    return spectateClicked;
}

} // namespace glory
