#include "hud/HealthBar.h"
#include "scene/Components.h"
#include "ability/AbilityComponents.h"
#include "combat/CombatComponents.h"

namespace glory {

void HealthBar::render(const entt::registry& reg,
                       const glm::mat4& vp, float screenW, float screenH,
                       uint8_t playerTeam) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    auto view = reg.view<const TransformComponent, const StatsComponent, const TeamComponent>();
    for (auto [entity, tc, stats, team] : view.each()) {
        float currentHP = stats.base.currentHP;
        float maxHP     = stats.total().maxHP;
        if (maxHP <= 0.0f || currentHP <= 0.0f) continue;

        // Project world position above head
        glm::vec3 worldPos = tc.position + glm::vec3(0.0f, m_config.yOffset, 0.0f);
        glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.001f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;

        // NDC → screen
        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;

        // Scale bar width by projected distance (perspective LOD)
        float projScale = 1.0f / clip.w;
        float barW = m_config.barWidth * projScale * 80.0f; // tuned scale factor
        barW = std::min(barW, m_config.barWidth * 1.5f);    // cap max size

        if (barW < m_config.minScreenSize) continue; // LOD skip

        float barH = m_config.barHeight * (barW / m_config.barWidth);
        barH = std::max(barH, 3.0f);

        float hpFrac = std::min(currentHP / maxHP, 1.0f);
        bool isAlly = (static_cast<uint8_t>(team.team) == playerTeam);
        ImU32 hpColor = isAlly ? m_config.allyColor : m_config.enemyColor;

        float x0 = sx - barW * 0.5f;
        float y0 = sy;

        // Border
        dl->AddRectFilled(ImVec2(x0 - m_config.borderSize, y0 - m_config.borderSize),
                          ImVec2(x0 + barW + m_config.borderSize, y0 + barH + m_config.borderSize),
                          m_config.borderColor);
        // Background
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW, y0 + barH), m_config.bgColor);
        // Health fill
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + barW * hpFrac, y0 + barH), hpColor);

        // Mana bar for entities with ResourceComponent
        if (reg.all_of<ResourceComponent>(entity)) {
            const auto& res = reg.get<ResourceComponent>(entity);
            if (res.maximum > 0.0f) {
                float manaFrac = std::min(res.current / res.maximum, 1.0f);
                float mH = m_config.manaBarHeight * (barW / m_config.barWidth);
                mH = std::max(mH, 2.0f);
                float my0 = y0 + barH + 1.0f;

                dl->AddRectFilled(ImVec2(x0, my0), ImVec2(x0 + barW, my0 + mH), m_config.bgColor);
                dl->AddRectFilled(ImVec2(x0, my0), ImVec2(x0 + barW * manaFrac, my0 + mH),
                                  m_config.manaColor);
            }
        }
    }
}

} // namespace glory
