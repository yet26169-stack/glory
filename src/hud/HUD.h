#pragma once

#include "hud/Minimap.h"
#include "hud/FloatingText.h"
#include "hud/HealthBar.h"
#include "hud/AbilityBar.h"
#include "hud/Scoreboard.h"
#include "hud/KillFeed.h"
#include "hud/RespawnOverlay.h"

namespace glory {

class HUD {
public:
    HUD() = default;

    void update(MinimapUpdateContext& ctx) { m_minimap.update(ctx); }

    bool isMinimapHovered() const { return m_minimap.isInteracting(); }

    FloatingText& floatingText() { return m_floatingText; }
    HealthBar&    healthBar()    { return m_healthBar; }
    AbilityBar&   abilityBar()   { return m_abilityBar; }
    Scoreboard&   scoreboard()   { return m_scoreboard; }
    KillFeed&     killFeed()     { return m_killFeed; }
    RespawnOverlay& respawnOverlay() { return m_respawnOverlay; }

    /// Render all HUD overlays (call between ImGui::NewFrame and ImGui::Render).
    /// Returns true if the spectate button was clicked.
    bool renderOverlays(const entt::registry& reg, entt::entity player,
                        const glm::mat4& vp, float screenW, float screenH,
                        float dt, uint8_t playerTeam) {
        m_floatingText.update(vp, screenW, screenH, dt);
        m_healthBar.render(reg, vp, screenW, screenH, playerTeam);
        m_abilityBar.render(reg, player, screenW, screenH);
        m_killFeed.render(screenW, dt);
        m_scoreboard.render(screenW, screenH);
        return m_respawnOverlay.render(reg, player, screenW, screenH);
    }

private:
    Minimap        m_minimap;
    FloatingText   m_floatingText;
    HealthBar      m_healthBar;
    AbilityBar     m_abilityBar;
    Scoreboard     m_scoreboard;
    KillFeed       m_killFeed;
    RespawnOverlay m_respawnOverlay;
};

} // namespace glory
