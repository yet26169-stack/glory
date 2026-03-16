#include "hud/Scoreboard.h"

#include <cstdio>
#include <algorithm>

namespace glory {

void Scoreboard::render(float screenW, float screenH) {
    if (!m_visible || m_players.empty()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float boardW = 600.0f;
    const float rowH   = 28.0f;
    const float headerH = 32.0f;
    const float pad    = 12.0f;

    // Count teams
    int blueCount = 0, redCount = 0;
    for (const auto& p : m_players) {
        if (p.team == 0) ++blueCount;
        else             ++redCount;
    }
    int maxRows = std::max(blueCount, redCount);
    float boardH = headerH + maxRows * rowH + pad * 2;

    float bx = (screenW - boardW) * 0.5f;
    float by = (screenH - boardH) * 0.3f; // slightly above center

    // Background
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + boardW, by + boardH),
                      IM_COL32(15, 18, 25, 230), 6.0f);
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + boardW, by + boardH),
                IM_COL32(80, 75, 60, 200), 6.0f, 0, 1.5f);

    float colW = (boardW - pad * 3) * 0.5f;

    // Blue team (left)
    drawTeamColumn(dl, bx + pad, by + pad, colW, 0, IM_COL32(40, 80, 160, 255));

    // Red team (right)
    drawTeamColumn(dl, bx + pad * 2 + colW, by + pad, colW, 1, IM_COL32(160, 40, 40, 255));
}

void Scoreboard::drawTeamColumn(ImDrawList* dl, float x, float y, float colW,
                                 uint8_t team, ImU32 headerColor) const {
    const float rowH = 28.0f;
    const float headerH = 32.0f;

    // Header
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + headerH), headerColor, 3.0f);
    const char* teamName = (team == 0) ? "BLUE TEAM" : "RED TEAM";
    ImVec2 tnSize = ImGui::CalcTextSize(teamName);
    dl->AddText(ImVec2(x + (colW - tnSize.x) * 0.5f, y + (headerH - tnSize.y) * 0.5f),
                IM_COL32(255, 255, 255, 255), teamName);

    // Column headers
    float hy = y + headerH + 2.0f;
    dl->AddText(ImVec2(x + 4.0f, hy), IM_COL32(180, 180, 180, 200), "Name");
    dl->AddText(ImVec2(x + colW * 0.45f, hy), IM_COL32(180, 180, 180, 200), "K/D/A");
    dl->AddText(ImVec2(x + colW * 0.72f, hy), IM_COL32(180, 180, 180, 200), "CS");
    dl->AddText(ImVec2(x + colW * 0.88f, hy), IM_COL32(180, 180, 180, 200), "Lv");

    float rowY = hy + 18.0f;
    for (const auto& p : m_players) {
        if (p.team != team) continue;

        // Alternating row background
        int rowIdx = static_cast<int>((rowY - hy - 18.0f) / rowH);
        if (rowIdx % 2 == 0)
            dl->AddRectFilled(ImVec2(x, rowY), ImVec2(x + colW, rowY + rowH),
                              IM_COL32(255, 255, 255, 10));

        // Alive/dead indicator
        ImU32 nameCol = (p.currentHP > 0.0f) ? IM_COL32(255, 255, 255, 255)
                                              : IM_COL32(120, 120, 120, 200);

        // Name
        dl->AddText(ImVec2(x + 4.0f, rowY + 4.0f), nameCol, p.name.c_str());

        // K/D/A
        char kda[24];
        std::snprintf(kda, sizeof(kda), "%d/%d/%d", p.kills, p.deaths, p.assists);
        dl->AddText(ImVec2(x + colW * 0.45f, rowY + 4.0f),
                    IM_COL32(220, 220, 220, 255), kda);

        // CS
        char cs[8];
        std::snprintf(cs, sizeof(cs), "%d", p.cs);
        dl->AddText(ImVec2(x + colW * 0.72f, rowY + 4.0f),
                    IM_COL32(200, 200, 160, 255), cs);

        // Level
        char lv[4];
        std::snprintf(lv, sizeof(lv), "%d", p.level);
        dl->AddText(ImVec2(x + colW * 0.88f, rowY + 4.0f),
                    IM_COL32(255, 200, 50, 255), lv);

        rowY += rowH;
    }
}

} // namespace glory
