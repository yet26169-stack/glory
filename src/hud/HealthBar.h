#pragma once
/// World-space health bars rendered above entities via ImGui overlay.
///
/// For each visible entity with StatsComponent, projects a bar above the
/// entity's head.  Green for allies, red for enemies.  Champions also show
/// a mana bar.  LOD: skip bars too small on screen.

#include <entt.hpp>
#include <glm/glm.hpp>
#include <imgui.h>

namespace glory {

class HealthBar {
public:
    struct Config {
        float barWidth       = 64.0f;  // pixels at reference distance
        float barHeight      = 6.0f;
        float manaBarHeight  = 4.0f;
        float yOffset        = 3.2f;   // world units above entity origin (characters)
        float structureYOffset = 8.0f; // world units above entity origin (towers/nexus)
        float minScreenSize  = 4.0f;   // skip if bar < this many pixels wide
        float borderSize     = 1.0f;

        ImU32 allyColor      = IM_COL32(50, 200, 80, 255);
        ImU32 enemyColor     = IM_COL32(220, 40, 40, 255);
        ImU32 manaColor      = IM_COL32(60, 120, 255, 255);
        ImU32 bgColor        = IM_COL32(20, 20, 20, 180);
        ImU32 borderColor    = IM_COL32(0, 0, 0, 200);
        ImU32 shieldColor    = IM_COL32(240, 240, 240, 200);
    };

    /// Render health bars for all visible entities.
    ///   vp         – combined view-projection matrix
    ///   screenW/H  – window dimensions
    ///   playerTeam – the local player's team for color selection
    void render(const entt::registry& reg,
                const glm::mat4& vp, float screenW, float screenH,
                uint8_t playerTeam);

    Config& config() { return m_config; }

private:
    Config m_config;
};

} // namespace glory
