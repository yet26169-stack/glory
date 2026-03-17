#include "hud/AbilityBar.h"
#include "ability/AbilityComponents.h"
#include "ability/AbilityTypes.h"
#include "combat/EconomySystem.h"

#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace glory {

void AbilityBar::render(const entt::registry& reg, entt::entity player,
                        float screenW, float screenH) {
    if (player == entt::null) return;
    if (!reg.all_of<AbilityBookComponent>(player)) return;

    const auto& book = reg.get<AbilityBookComponent>(player);

    // Get resource for mana check
    float currentMana = 999999.0f;
    if (reg.all_of<ResourceComponent>(player))
        currentMana = reg.get<ResourceComponent>(player).current;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float totalW = 4 * m_config.iconSize + 3 * m_config.iconSpacing;
    float startX = (screenW - totalW) * 0.5f;
    float startY = screenH - m_config.bottomMargin - m_config.iconSize;

    for (int i = 0; i < 4; ++i) {
        const auto& inst = book.abilities[i]; // Q=0, W=1, E=2, R=3
        float x = startX + i * (m_config.iconSize + m_config.iconSpacing);
        float y = startY;

        ImVec2 tl(x, y);
        ImVec2 br(x + m_config.iconSize, y + m_config.iconSize);

        // Determine state
        bool learned   = (inst.level > 0);
        bool onCD      = (inst.currentPhase == AbilityPhase::ON_COOLDOWN);
        float manaCost = 0.0f;
        if (learned && inst.def) {
            int idx = std::max(0, std::min(inst.level - 1, 4));
            manaCost = inst.def->costPerLevel[static_cast<size_t>(idx)];
        }
        bool noMana = learned && (currentMana < manaCost);

        // Background
        ImU32 bgCol = m_config.readyColor;
        if (!learned)      bgCol = IM_COL32(25, 25, 25, 200);
        else if (onCD)     bgCol = m_config.cooldownColor;
        else if (noMana)   bgCol = m_config.noManaColor;

        dl->AddRectFilled(tl, br, bgCol, 4.0f);
        dl->AddRect(tl, br, m_config.borderColor, 4.0f, 0, 1.5f);

        // Cooldown pie overlay
        if (onCD && learned && inst.def) {
            int idx = std::max(0, std::min(inst.level - 1, 4));
            float maxCD = inst.def->cooldownPerLevel[static_cast<size_t>(idx)];
            float frac = (maxCD > 0.0f) ? (inst.cooldownRemaining / maxCD) : 0.0f;
            frac = std::max(0.0f, std::min(1.0f, frac));

            ImVec2 center(x + m_config.iconSize * 0.5f, y + m_config.iconSize * 0.5f);
            drawCooldownPie(dl, center, m_config.iconSize * 0.45f, frac,
                            m_config.cooldownOverlay);

            // Cooldown remaining text
            char cdBuf[8];
            std::snprintf(cdBuf, sizeof(cdBuf), "%.1f", inst.cooldownRemaining);
            ImVec2 textSize = ImGui::CalcTextSize(cdBuf);
            dl->AddText(ImVec2(center.x - textSize.x * 0.5f,
                               center.y - textSize.y * 0.5f),
                        m_config.textColor, cdBuf);
        }

        // Hotkey label (top-left corner)
        dl->AddText(nullptr, 12.0f, ImVec2(x + 3.0f, y + 1.0f),
                    m_config.hotKeyColor, SLOT_KEYS[i]);

        // Mana cost (bottom center)
        if (learned && manaCost > 0.0f) {
            char costBuf[8];
            std::snprintf(costBuf, sizeof(costBuf), "%.0f", manaCost);
            ImVec2 costSize = ImGui::CalcTextSize(costBuf);
            ImU32 costCol = noMana ? IM_COL32(255, 80, 80, 255) : IM_COL32(100, 160, 255, 220);
            dl->AddText(ImVec2(x + (m_config.iconSize - costSize.x) * 0.5f,
                               y + m_config.iconSize - costSize.y - 2.0f),
                        costCol, costBuf);
        }

        // Level dots (below icon)
        if (learned) {
            int maxLevel = 5;
            float dotSpacing = 8.0f;
            float dotsWidth = maxLevel * dotSpacing;
            float dotStartX = x + (m_config.iconSize - dotsWidth) * 0.5f + dotSpacing * 0.5f;
            float dotY = y + m_config.iconSize + 4.0f;
            for (int lv = 0; lv < maxLevel; ++lv) {
                ImU32 dotCol = (lv < inst.level) ? m_config.levelDotOn : m_config.levelDotOff;
                dl->AddCircleFilled(ImVec2(dotStartX + lv * dotSpacing, dotY), 2.5f, dotCol);
            }
        }
    }

    // ── Gold and level display flanking the ability bar ──────────────────
    if (reg.all_of<EconomyComponent>(player)) {
        const auto& eco = reg.get<EconomyComponent>(player);

        // Level badge (left of ability bar)
        float levelX = startX - 50.0f;
        float levelY = startY + m_config.iconSize * 0.5f - 8.0f;
        char levelBuf[8];
        std::snprintf(levelBuf, sizeof(levelBuf), "Lv %d", eco.level);
        dl->AddText(nullptr, 16.0f, ImVec2(levelX, levelY),
                    IM_COL32(255, 215, 0, 255), levelBuf);

        // Gold display (right of ability bar)
        float goldX = startX + totalW + 10.0f;
        float goldY = levelY;
        char goldBuf[16];
        std::snprintf(goldBuf, sizeof(goldBuf), "%d G", eco.gold);
        dl->AddText(nullptr, 16.0f, ImVec2(goldX, goldY),
                    IM_COL32(255, 215, 0, 255), goldBuf);
    }
}

void AbilityBar::drawCooldownPie(ImDrawList* dl, ImVec2 center, float radius,
                                  float fraction, ImU32 color) const {
    if (fraction <= 0.0f) return;
    if (fraction >= 1.0f) {
        dl->AddCircleFilled(center, radius, color, 32);
        return;
    }

    // Draw a filled arc from 12 o'clock clockwise
    const int segments = 32;
    int segsToFill = static_cast<int>(fraction * segments + 0.5f);
    if (segsToFill < 1) return;

    // Build triangle fan
    for (int s = 0; s < segsToFill; ++s) {
        float a0 = -static_cast<float>(M_PI) * 0.5f + (static_cast<float>(s) / segments) * 2.0f * static_cast<float>(M_PI);
        float a1 = -static_cast<float>(M_PI) * 0.5f + (static_cast<float>(s + 1) / segments) * 2.0f * static_cast<float>(M_PI);

        ImVec2 p0 = center;
        ImVec2 p1(center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius);
        ImVec2 p2(center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius);
        dl->AddTriangleFilled(p0, p1, p2, color);
    }
}

} // namespace glory
