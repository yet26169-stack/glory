#pragma once
/// TAB-toggle scoreboard: two-column layout (blue team / red team),
/// showing champion name, K/D/A, CS, level.

#include <entt.hpp>
#include <imgui.h>

#include <string>
#include <vector>

namespace glory {

class Scoreboard {
public:
    /// Per-player row data.  In a full game this comes from the networked
    /// game state; for now it's pulled from ECS components each frame.
    struct PlayerInfo {
        std::string name      = "Unknown";
        uint8_t     team      = 0;     // 0=blue, 1=red
        int         kills     = 0;
        int         deaths    = 0;
        int         assists   = 0;
        int         cs        = 0;     // creep score
        int         level     = 1;
        float       currentHP = 0.0f;
        float       maxHP     = 0.0f;
    };

    /// Toggle visibility (call when TAB pressed).
    void toggle() { m_visible = !m_visible; }
    void show()   { m_visible = true; }
    void hide()   { m_visible = false; }
    bool isVisible() const { return m_visible; }

    /// Set player data externally (populated from registry/network).
    void setPlayers(std::vector<PlayerInfo> players) { m_players = std::move(players); }

    /// Render the scoreboard overlay.
    void render(float screenW, float screenH);

private:
    bool m_visible = false;
    std::vector<PlayerInfo> m_players;

    void drawTeamColumn(ImDrawList* dl, float x, float y, float colW,
                        uint8_t team, ImU32 headerColor) const;
};

} // namespace glory
