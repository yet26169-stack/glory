#include "hud/KillFeed.h"

#include <algorithm>
#include <cstdio>

namespace glory {

void KillFeed::push(const std::string& killer, uint8_t killerTeam,
                    const std::string& victim, uint8_t victimTeam) {
    Event ev;
    ev.killerName = killer;
    ev.victimName = victim;
    ev.killerTeam = killerTeam;
    ev.victimTeam = victimTeam;
    ev.age        = 0.0f;

    m_events.push_front(ev);

    // Cap total stored events
    while (m_events.size() > 20)
        m_events.pop_back();
}

void KillFeed::render(float screenW, float dt) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    // Age all events and remove expired ones
    for (auto& ev : m_events) ev.age += dt;
    while (!m_events.empty() && m_events.back().age >= FADE_TIME)
        m_events.pop_back();

    // Render up to MAX_VISIBLE, top-right corner
    const float rightMargin = 10.0f;
    const float topMargin   = 10.0f;
    const float lineH       = 22.0f;

    int shown = 0;
    for (const auto& ev : m_events) {
        if (shown >= MAX_VISIBLE) break;

        float alpha = 1.0f;
        if (ev.age > FADE_START)
            alpha = 1.0f - (ev.age - FADE_START) / (FADE_TIME - FADE_START);
        alpha = std::max(0.0f, std::min(1.0f, alpha));
        uint8_t a = static_cast<uint8_t>(alpha * 255.0f);

        // Build the text
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s  killed  %s",
                      ev.killerName.c_str(), ev.victimName.c_str());

        ImVec2 textSize = ImGui::CalcTextSize(buf);
        float bgW = textSize.x + 16.0f;
        float bgH = lineH;
        float x = screenW - rightMargin - bgW;
        float y = topMargin + shown * (lineH + 4.0f);

        // Background pill
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bgW, y + bgH),
                          IM_COL32(15, 15, 20, static_cast<int>(180 * alpha)), 4.0f);

        // Killer name in team color
        ImU32 killerCol = (ev.killerTeam == 0)
                          ? IM_COL32(80, 180, 255, a)
                          : IM_COL32(255, 80, 80, a);
        float tx = x + 8.0f;
        float ty = y + (bgH - textSize.y) * 0.5f;

        dl->AddText(ImVec2(tx, ty), killerCol, ev.killerName.c_str());
        tx += ImGui::CalcTextSize(ev.killerName.c_str()).x;

        dl->AddText(ImVec2(tx, ty), IM_COL32(180, 180, 180, a), "  killed  ");
        tx += ImGui::CalcTextSize("  killed  ").x;

        // Victim name in team color
        ImU32 victimCol = (ev.victimTeam == 0)
                          ? IM_COL32(80, 180, 255, a)
                          : IM_COL32(255, 80, 80, a);
        dl->AddText(ImVec2(tx, ty), victimCol, ev.victimName.c_str());

        ++shown;
    }
}

} // namespace glory
