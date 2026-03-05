#include "hud/HUD.h"
#include "ability/AbilityComponents.h"
#include "ability/AbilityDef.h"
#include "ability/AbilityTypes.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace glory {

// ── Color palette (clean/minimal) ─────────────────────────────────────────
namespace col {
    static const ImU32 outline       = IM_COL32(30, 30, 30, 255);
    static const ImU32 outlineLight  = IM_COL32(60, 60, 60, 200);
    static const ImU32 panelBg       = IM_COL32(20, 20, 25, 200);
    static const ImU32 panelBgSlot   = IM_COL32(30, 35, 40, 220);
    static const ImU32 textWhite     = IM_COL32(230, 230, 230, 255);
    static const ImU32 textDim       = IM_COL32(140, 140, 140, 200);
    static const ImU32 textGold      = IM_COL32(255, 210, 80, 255);
    static const ImU32 accent        = IM_COL32(80, 160, 220, 255);
    static const ImU32 accentDim     = IM_COL32(60, 120, 170, 180);

    static const ImU32 hpGreen       = IM_COL32(60, 200, 60, 255);
    static const ImU32 hpGreenDark   = IM_COL32(35, 140, 35, 255);
    static const ImU32 hpOrange      = IM_COL32(255, 165, 0, 255);
    static const ImU32 hpRed         = IM_COL32(220, 40, 40, 255);
    static const ImU32 manaBlue      = IM_COL32(60, 100, 220, 255);
    static const ImU32 manaBlueDark  = IM_COL32(35, 60, 150, 255);
    static const ImU32 energyYellow  = IM_COL32(220, 200, 50, 255);
    static const ImU32 rageRed       = IM_COL32(200, 40, 40, 255);
    static const ImU32 cdOverlay     = IM_COL32(10, 10, 10, 170);
    static const ImU32 cdText        = IM_COL32(255, 255, 255, 220);
} // namespace col

void HUD::init(float screenWidth, float screenHeight) {
    m_screenW = screenWidth;
    m_screenH = screenHeight;
}

void HUD::resize(float screenWidth, float screenHeight) {
    m_screenW = screenWidth;
    m_screenH = screenHeight;
}

void HUD::draw(const Scene& scene, entt::entity player) {
    const auto& reg = scene.getRegistry();
    const auto* stats = reg.try_get<CombatStatsComponent>(player);
    const auto* book  = reg.try_get<AbilityBookComponent>(player);
    if (!stats) return;

    drawScorePanel();
    drawCirclePanel();
    drawCenterPanel(*stats, book);
    drawSquarePanel();
}

// ═══════════════════════════════════════════════════════════════════════════
// TOP-LEFT: SCORE / TIMER
// ═══════════════════════════════════════════════════════════════════════════

void HUD::drawScorePanel() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float x = 10.0f, y = 8.0f;
    float w = 170.0f, h = 46.0f;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col::panelBg, 6.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col::outline, 6.0f, 0, 2.0f);

    int mins = (int)(m_gameTime) / 60;
    int secs = (int)(m_gameTime) % 60;
    char timeBuf[16];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", mins, secs);
    ImVec2 timeSize = ImGui::CalcTextSize(timeBuf);
    dl->AddText(ImVec2(x + (w - timeSize.x) * 0.5f, y + 24.0f), col::accentDim, timeBuf);
}

// ═══════════════════════════════════════════════════════════════════════════
// BOTTOM-LEFT: CIRCLE (portrait / minimap)
// ═══════════════════════════════════════════════════════════════════════════

void HUD::drawCirclePanel() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float radius = 70.0f;
    float cx = 100.0f;
    float cy = m_screenH - 90.0f;

    // Background
    dl->AddCircleFilled(ImVec2(cx, cy), radius, col::panelBg, 48);

    // Outline
    dl->AddCircle(ImVec2(cx, cy), radius, col::outline, 48, 3.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// BOTTOM-CENTER: ROUNDED RECTANGLE (abilities, bars)
// ═══════════════════════════════════════════════════════════════════════════

void HUD::drawCenterPanel(const CombatStatsComponent& stats, const AbilityBookComponent* book) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Layout: rounded rect that visually connects to the circle on the left
    float panelW = 420.0f;
    float panelH = 120.0f;
    float panelX = 160.0f;
    float panelY = m_screenH - panelH - 25.0f;
    float rounding = panelH * 0.5f; // fully rounded ends

    // Background
    dl->AddRectFilled(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH),
                      col::panelBg, rounding);
    // Outline
    dl->AddRect(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH),
                col::outline, rounding, 0, 3.0f);

    // Level badge (left rounded area)
    char lvlBuf[8];
    std::snprintf(lvlBuf, sizeof(lvlBuf), "%d", stats.level);
    ImVec2 lvlSz = ImGui::CalcTextSize(lvlBuf);
    float badgeCX = panelX + rounding;
    float badgeCY = panelY + panelH * 0.5f;
    dl->AddCircleFilled(ImVec2(badgeCX, badgeCY), 22.0f, col::panelBgSlot);
    dl->AddCircle(ImVec2(badgeCX, badgeCY), 22.0f, col::accentDim, 0, 1.5f);
    dl->AddText(ImVec2(badgeCX - lvlSz.x * 0.5f, badgeCY - lvlSz.y * 0.5f), col::textWhite, lvlBuf);

    // Ability icons
    float abilityX = panelX + rounding + 40.0f;
    float abilityY = panelY + 14.0f;
    if (book) {
        drawAbilityBar(abilityX, abilityY, *book);
    }

    // Health bar (below abilities)
    float iconSize = 42.0f;
    float spacing = 5.0f;
    float barW = 4 * (iconSize + spacing) - spacing;
    float hpX = abilityX;
    float hpY = abilityY + iconSize + 18.0f;
    drawHealthBar(hpX, hpY, barW, stats);

    // Resource bar (below health)
    float resY = hpY + 20.0f;
    drawResourceBar(hpX, resY, barW, stats);
}

void HUD::drawAbilityBar(float x, float y, const AbilityBookComponent& book) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float iconSize = 42.0f;
    const float spacing  = 5.0f;
    const char* labels[] = {"Q", "W", "E", "R"};
    const AbilitySlot slots[] = {AbilitySlot::Q, AbilitySlot::W,
                                  AbilitySlot::E, AbilitySlot::R};

    for (int i = 0; i < 4; ++i) {
        float ix = x + i * (iconSize + spacing);
        const auto& ab = book.get(slots[i]);

        // Slot background
        ImU32 bgCol = (ab.level > 0) ? col::panelBgSlot : IM_COL32(20, 20, 25, 200);
        dl->AddRectFilled(ImVec2(ix, y), ImVec2(ix + iconSize, y + iconSize), bgCol, 4.0f);

        // Ability color fill
        if (ab.level > 0) {
            ImU32 abilityCol;
            switch (i) {
                case 0: abilityCol = IM_COL32(130, 50, 180, 100); break;
                case 1: abilityCol = IM_COL32(50, 80, 180, 100); break;
                case 2: abilityCol = IM_COL32(50, 150, 180, 100); break;
                case 3: abilityCol = IM_COL32(40, 120, 140, 100); break;
                default: abilityCol = col::panelBgSlot; break;
            }
            dl->AddRectFilled(ImVec2(ix + 2, y + 2),
                              ImVec2(ix + iconSize - 2, y + iconSize - 2),
                              abilityCol, 3.0f);
        }

        // Cooldown overlay
        if (ab.cooldownRemaining > 0.0f && ab.def) {
            float maxCd = ab.def->cooldownPerLevel[std::max(0, ab.level - 1)];
            float cdFrac = (maxCd > 0.0f) ? (ab.cooldownRemaining / maxCd) : 0.0f;
            cdFrac = std::clamp(cdFrac, 0.0f, 1.0f);
            float overlayH = iconSize * cdFrac;
            dl->AddRectFilled(ImVec2(ix, y), ImVec2(ix + iconSize, y + overlayH),
                              col::cdOverlay, 4.0f);
            char cd[8];
            std::snprintf(cd, sizeof(cd), "%.0f", std::ceil(ab.cooldownRemaining));
            ImVec2 cdSz = ImGui::CalcTextSize(cd);
            dl->AddText(ImVec2(ix + (iconSize - cdSz.x) * 0.5f,
                               y + (iconSize - cdSz.y) * 0.5f),
                        col::cdText, cd);
        }

        // Border
        ImU32 borderCol = (ab.level > 0) ? col::outlineLight : IM_COL32(40, 40, 40, 150);
        if (ab.currentPhase != AbilityPhase::READY && ab.level > 0)
            borderCol = col::accent;
        dl->AddRect(ImVec2(ix, y), ImVec2(ix + iconSize, y + iconSize),
                    borderCol, 4.0f, 0, 1.5f);

        // Key label
        ImVec2 labelSz = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(ix + (iconSize - labelSz.x) * 0.5f, y + iconSize + 2.0f),
                    col::textDim, labels[i]);

        // Hover tooltip
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.x >= ix && mouse.x <= ix + iconSize &&
            mouse.y >= y && mouse.y <= y + iconSize && ab.def) {
            drawTooltip(ab);
        }
    }
}

void HUD::drawHealthBar(float x, float y, float w, const CombatStatsComponent& stats) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float h = 16.0f;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(15, 15, 15, 220), 3.0f);

    float frac = std::clamp(stats.currentHP / std::max(stats.maxHP, 1.0f), 0.0f, 1.0f);
    ImU32 hpCol = frac > 0.5f  ? col::hpGreen
                : frac > 0.25f ? col::hpOrange
                :                col::hpRed;
    ImU32 hpColDark = frac > 0.5f  ? col::hpGreenDark
                    : frac > 0.25f ? IM_COL32(160, 100, 0, 255)
                    :                IM_COL32(140, 20, 20, 255);

    if (frac > 0.0f) {
        float fillW = (w - 2) * frac;
        dl->AddRectFilledMultiColor(ImVec2(x + 1, y + 1), ImVec2(x + 1 + fillW, y + h - 1),
                                    hpCol, hpCol, hpColDark, hpColDark);
    }

    if (stats.shield > 0.0f) {
        float shieldFrac = std::clamp(stats.shield / std::max(stats.maxHP, 1.0f), 0.0f, 1.0f - frac);
        float shieldStart = x + 1 + (w - 2) * frac;
        dl->AddRectFilled(ImVec2(shieldStart, y + 1),
                          ImVec2(shieldStart + (w - 2) * shieldFrac, y + h - 1),
                          IM_COL32(200, 200, 200, 150), 1.0f);
    }

    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col::outlineLight, 3.0f, 0, 1.0f);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f / %.0f", stats.currentHP, stats.maxHP);
    ImVec2 textSize = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(x + (w - textSize.x) * 0.5f, y + (h - textSize.y) * 0.5f),
                col::textWhite, buf);
}

void HUD::drawResourceBar(float x, float y, float w, const CombatStatsComponent& stats) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float h = 12.0f;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(15, 15, 15, 220), 3.0f);

    float frac = std::clamp(stats.currentResource / std::max(stats.maxResource, 1.0f), 0.0f, 1.0f);

    ImU32 resCol, resColDark;
    switch (stats.resourceType) {
    case ResourceType::MANA:
        resCol = col::manaBlue; resColDark = col::manaBlueDark; break;
    case ResourceType::ENERGY:
        resCol = col::energyYellow; resColDark = IM_COL32(140, 120, 20, 255); break;
    case ResourceType::RAGE:
        resCol = col::rageRed; resColDark = IM_COL32(130, 20, 20, 255); break;
    default:
        resCol = col::manaBlue; resColDark = col::manaBlueDark; break;
    }

    if (frac > 0.0f) {
        float fillW = (w - 2) * frac;
        dl->AddRectFilledMultiColor(ImVec2(x + 1, y + 1), ImVec2(x + 1 + fillW, y + h - 1),
                                    resCol, resCol, resColDark, resColDark);
    }

    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col::outlineLight, 3.0f, 0, 1.0f);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f / %.0f", stats.currentResource, stats.maxResource);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f),
                IM_COL32(220, 220, 240, 230), buf);
}

// ═══════════════════════════════════════════════════════════════════════════
// BOTTOM-RIGHT: SQUARE (items / chat / score)
// ═══════════════════════════════════════════════════════════════════════════

void HUD::drawSquarePanel() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float size = 140.0f;
    float x = m_screenW - size - 15.0f;
    float y = m_screenH - size - 20.0f;

    // Background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), col::panelBg, 4.0f);
    // Outline
    dl->AddRect(ImVec2(x, y), ImVec2(x + size, y + size), col::outline, 4.0f, 0, 3.0f);

    // Item slots (2x3 grid)
    float slotSize = 30.0f;
    float slotSpacing = 4.0f;
    float gridW = 3 * slotSize + 2 * slotSpacing;
    float startX = x + (size - gridW) * 0.5f;
    float startY = y + 10.0f;
    for (int row = 0; row < 2; ++row) {
        for (int c = 0; c < 3; ++c) {
            float sx = startX + c * (slotSize + slotSpacing);
            float sy = startY + row * (slotSize + slotSpacing);
            dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + slotSize, sy + slotSize),
                              col::panelBgSlot, 3.0f);
            dl->AddRect(ImVec2(sx, sy), ImVec2(sx + slotSize, sy + slotSize),
                        col::outlineLight, 3.0f, 0, 1.0f);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// TOOLTIP
// ═══════════════════════════════════════════════════════════════════════════

void HUD::drawTooltip(const AbilityInstance& ab) {
    if (!ab.def) return;
    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.08f, 0.10f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushTextWrapPos(280.0f);

    ImGui::TextColored(ImVec4(0.31f, 0.63f, 0.86f, 1.0f), "%s",
                       ab.def->displayName.c_str());
    ImGui::Separator();

    int lvlIdx = std::max(0, ab.level - 1);
    float cd   = ab.def->cooldownPerLevel[lvlIdx];
    float cost = ab.def->costPerLevel[lvlIdx];

    ImGui::Text("Cooldown: %.1fs", cd);
    if (cost > 0.0f) {
        const char* resName = "resource";
        if (ab.def->resourceType == ResourceType::MANA)   resName = "mana";
        if (ab.def->resourceType == ResourceType::ENERGY) resName = "energy";
        if (ab.def->resourceType == ResourceType::RAGE)   resName = "rage";
        ImGui::Text("Cost: %.0f %s", cost, resName);
    }
    if (ab.def->castRange > 0.0f)
        ImGui::Text("Range: %.0f", ab.def->castRange);

    if (ab.level > 0)
        ImGui::TextColored(ImVec4(0.31f, 0.63f, 0.86f, 1.0f), "Level: %d", ab.level);
    else
        ImGui::TextDisabled("Not learned");

    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::EndTooltip();
}

} // namespace glory
