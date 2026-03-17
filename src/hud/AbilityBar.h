#pragma once
/// Bottom-center ability bar: Q/W/E/R icons with cooldown overlay, mana cost,
/// and level dots.

#include <entt.hpp>
#include <imgui.h>
#include <string>
#include <array>

namespace glory {

struct EconomyComponent;

class AbilityBar {
public:
    struct Config {
        float iconSize     = 48.0f;
        float iconSpacing  = 6.0f;
        float bottomMargin = 40.0f;

        ImU32 readyColor     = IM_COL32(60, 60, 70, 220);
        ImU32 cooldownColor  = IM_COL32(30, 30, 35, 220);
        ImU32 noManaColor    = IM_COL32(40, 40, 80, 220);
        ImU32 borderColor    = IM_COL32(180, 170, 140, 255);
        ImU32 cooldownOverlay= IM_COL32(0, 0, 0, 160);
        ImU32 textColor      = IM_COL32(255, 255, 255, 255);
        ImU32 levelDotOn     = IM_COL32(255, 200, 50, 255);
        ImU32 levelDotOff    = IM_COL32(80, 80, 80, 180);
        ImU32 hotKeyColor    = IM_COL32(255, 255, 200, 200);
    };

    /// Render the ability bar for the given player entity.
    void render(const entt::registry& reg, entt::entity player,
                float screenW, float screenH);

    Config& config() { return m_config; }

private:
    Config m_config;

    static constexpr const char* SLOT_KEYS[] = { "Q", "W", "E", "R" };

    void drawCooldownPie(ImDrawList* dl, ImVec2 center, float radius,
                         float fraction, ImU32 color) const;
};

} // namespace glory
