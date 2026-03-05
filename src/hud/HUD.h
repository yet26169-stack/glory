#pragma once
// ── Minimalist MOBA HUD ──────────────────────────────────────────────────
// Clean, modern HUD layout (matching normal_easy_hud.pxd):
//   - Bottom-left: Circle (portrait/minimap)
//   - Bottom-center: Rounded rectangle (abilities, HP/mana bars)
//   - Bottom-right: Square (items, chat, score)
//   - Top-left: Score + timer
// Rendered via Dear ImGui background draw list (no textures needed).

#include <entt.hpp>
#include <glm/glm.hpp>

using HudColor = unsigned int;

namespace glory {

class Scene;
struct CombatStatsComponent;
struct AbilityBookComponent;
struct AbilityInstance;

class HUD {
public:
    void init(float screenWidth, float screenHeight);
    void resize(float screenWidth, float screenHeight);
    void setGameTime(float gameTimeSec) { m_gameTime = gameTimeSec; }

    void draw(const Scene& scene, entt::entity player);

private:
    void drawScorePanel();
    void drawCirclePanel();
    void drawCenterPanel(const CombatStatsComponent& stats, const AbilityBookComponent* book);
    void drawSquarePanel();
    void drawAbilityBar(float x, float y, const AbilityBookComponent& book);
    void drawHealthBar(float x, float y, float w, const CombatStatsComponent& stats);
    void drawResourceBar(float x, float y, float w, const CombatStatsComponent& stats);
    void drawTooltip(const AbilityInstance& ability);

    float m_screenW = 1280.0f;
    float m_screenH = 720.0f;
    float m_gameTime = 0.0f;
};

} // namespace glory
