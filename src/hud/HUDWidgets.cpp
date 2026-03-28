#include "ability/AbilityComponents.h"
#include "ability/AbilityTypes.h"
#include "combat/CombatComponents.h"
#include "combat/EconomySystem.h"
#include "combat/HeroDefinition.h"
#include "combat/RespawnSystem.h"
#include "combat/StructureSystem.h"
#include "hud/AbilityBar.h"
#include "hud/FloatingText.h"
#include "hud/HealthBar.h"
#include "hud/KillFeed.h"
#include "hud/RespawnOverlay.h"
#include "scene/Components.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace glory {

// ═══ FloatingText.cpp ═══

void FloatingText::spawn(const glm::vec3& worldPos, float amount, DamageKind kind) {
    auto& e = m_pool[m_next];
    e.active    = true;
    e.age       = 0.0f;
    e.lifetime  = (kind == DamageKind::Crit) ? 1.3f : 1.0f;
    e.worldPos  = worldPos + glm::vec3(0.f, 2.2f, 0.f); // above head
    e.amount    = amount;
    e.kind      = kind;
    e.riseSpeed = 55.0f + (kind == DamageKind::Crit ? 20.0f : 0.0f);
    // Small random-ish horizontal scatter using amount as seed
    e.xJitter   = std::fmod(amount * 7.13f, 30.0f) - 15.0f;

    m_next = (m_next + 1) % MAX_TEXTS;
}

void FloatingText::update(const glm::mat4& vp, float screenW, float screenH, float dt) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    for (auto& e : m_pool) {
        if (!e.active) continue;

        e.age += dt;
        if (e.age >= e.lifetime) {
            e.active = false;
            continue;
        }

        // Project world → screen (NDC → pixel)
        glm::vec4 clip = vp * glm::vec4(e.worldPos, 1.0f);
        if (clip.w <= 0.001f) continue; // behind camera
        glm::vec3 ndc = glm::vec3(clip) / clip.w;

        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        float sy = (ndc.y * 0.5f + 0.5f) * screenH;

        // Rise upward in screen space
        float t = e.age / e.lifetime;
        sy -= e.riseSpeed * e.age;
        sx += e.xJitter * (1.0f - t); // jitter fades out

        // Alpha fade: full for first 60%, then fade to 0
        float alpha = 1.0f;
        if (t > 0.6f) alpha = 1.0f - (t - 0.6f) / 0.4f;
        alpha = std::max(0.0f, std::min(1.0f, alpha));

        ImU32 baseCol = colorForKind(e.kind);
        uint8_t a = static_cast<uint8_t>(alpha * 255.0f);
        ImU32 col = (baseCol & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24);

        float fontSize = sizeForKind(e.kind);

        // Format the number
        char buf[32];
        if (e.kind == DamageKind::Heal)
            std::snprintf(buf, sizeof(buf), "+%.0f", e.amount);
        else
            std::snprintf(buf, sizeof(buf), "%.0f", e.amount);

        // Scale text via SetFontSize isn't easily done with ImGui default font,
        // so we draw shadow + text at a fixed font size, with crits slightly offset
        float scale = fontSize / 13.0f; // 13 = default ImGui font size

        // Drop shadow
        ImU32 shadow = IM_COL32(0, 0, 0, a / 2);
        dl->AddText(nullptr, fontSize, ImVec2(sx + 1.0f, sy + 1.0f), shadow, buf);
        // Main text
        dl->AddText(nullptr, fontSize, ImVec2(sx, sy), col, buf);
    }
}

ImU32 FloatingText::colorForKind(DamageKind k) {
    switch (k) {
    case DamageKind::Physical: return IM_COL32(255, 255, 255, 255);
    case DamageKind::Magic:    return IM_COL32(180, 120, 255, 255);
    case DamageKind::True:     return IM_COL32(255, 255, 255, 255);
    case DamageKind::Heal:     return IM_COL32(80, 255, 80, 255);
    case DamageKind::Crit:     return IM_COL32(255, 230, 50, 255);
    }
    return IM_COL32(255, 255, 255, 255);
}

float FloatingText::sizeForKind(DamageKind k) {
    switch (k) {
    case DamageKind::Crit:  return 22.0f;
    case DamageKind::Heal:  return 16.0f;
    default:                return 15.0f;
    }
}

// ═══ HealthBar.cpp ═══

void HealthBar::render(const entt::registry& reg,
                       const glm::mat4& vp, float screenW, float screenH,
                       uint8_t playerTeam, float renderAlpha) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    auto view = reg.view<const TransformComponent, const StatsComponent, const TeamComponent>();
    for (auto [entity, tc, stats, team] : view.each()) {
        float currentHP = stats.base.currentHP;
        float maxHP     = stats.total().maxHP;
        if (maxHP <= 0.0f || currentHP <= 0.0f) continue;

        // Compute Y offset from actual model bounds when available,
        // falling back to per-type offsets for skinned / unknown entities.
        float yOff;
        if (auto* mc = reg.try_get<MeshComponent>(entity)) {
            float modelTop = tc.scale.y * mc->localAABB.max.y;
            yOff = modelTop + m_config.yPadding;
        } else if (reg.all_of<GPUSkinnedMeshComponent>(entity)) {
            if (reg.all_of<MinionComponent>(entity))
                yOff = m_config.minionYOffset + m_config.yPadding;
            else if (reg.all_of<HeroComponent>(entity))
                yOff = m_config.championYOffset + m_config.yPadding;
            else
                yOff = m_config.yOffset + m_config.yPadding;
        } else {
            yOff = m_config.yOffset;
        }

        // Interpolate position to match the GPU-rendered transform
        glm::vec3 interpPos = glm::mix(tc.prevPosition, tc.position, renderAlpha);

        // Project world position above head
        glm::vec3 worldPos = interpPos + glm::vec3(0.0f, yOff, 0.0f);
        glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.001f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;

        // NDC → screen
        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        float sy = (ndc.y * 0.5f + 0.5f) * screenH;

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

// ═══ AbilityBar.cpp ═══

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

    // Skill point availability
    int heroLevel = 1;
    if (reg.all_of<EconomyComponent>(player))
        heroLevel = reg.get<EconomyComponent>(player).level;
    int spentPoints = 0;
    for (const auto& a : book.abilities) spentPoints += a.level;
    bool hasSkillPoints = (spentPoints < heroLevel);

    for (int i = 0; i < 4; ++i) {
        const auto& inst = book.abilities[i]; // Q=0, W=1, E=2, R=3
        int maxLvl = (i == 3) ? 3 : 5; // R max 3, others max 5
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

        // Skill-point-available indicator (green glow border)
        if (hasSkillPoints && inst.def && inst.level < maxLvl) {
            bool canLevel = true;
            // R requires hero levels 6/11/16
            if (i == 3) {
                int reqLvl = (inst.level == 0) ? 6 : (inst.level == 1) ? 11 : 16;
                if (heroLevel < reqLvl) canLevel = false;
            }
            if (canLevel) {
                ImU32 glowCol = IM_COL32(50, 255, 100, 180);
                dl->AddRect(ImVec2(tl.x - 2, tl.y - 2), ImVec2(br.x + 2, br.y + 2),
                            glowCol, 5.0f, 0, 2.5f);
                // "Ctrl+KEY" text above icon
                char ctrlBuf[16];
                std::snprintf(ctrlBuf, sizeof(ctrlBuf), "Ctrl+%s", SLOT_KEYS[i]);
                ImVec2 ctrlSize = ImGui::CalcTextSize(ctrlBuf);
                dl->AddText(ImVec2(x + (m_config.iconSize - ctrlSize.x) * 0.5f,
                                   y - ctrlSize.y - 2.0f),
                            glowCol, ctrlBuf);
            }
        }

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
            float dotSpacing = 8.0f;
            float dotsWidth = maxLvl * dotSpacing;
            float dotStartX = x + (m_config.iconSize - dotsWidth) * 0.5f + dotSpacing * 0.5f;
            float dotY = y + m_config.iconSize + 4.0f;
            for (int lv = 0; lv < maxLvl; ++lv) {
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

// ═══ KillFeed.cpp ═══

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

// ═══ RespawnOverlay.cpp ═══

bool RespawnOverlay::render(const entt::registry& reg, entt::entity player,
                             float screenW, float screenH) {
    if (player == entt::null) return false;

    auto* rc = reg.try_get<RespawnComponent>(player);
    if (!rc || rc->state == LifeState::ALIVE) return false;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return false;

    // Semi-transparent dark overlay at top center
    float overlayW = 320.0f;
    float overlayH = 120.0f;
    float ox = (screenW - overlayW) * 0.5f;
    float oy = screenH * 0.3f;

    dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + overlayW, oy + overlayH),
                       IM_COL32(0, 0, 0, 180), 8.0f);
    dl->AddRect(ImVec2(ox, oy), ImVec2(ox + overlayW, oy + overlayH),
                IM_COL32(180, 50, 50, 255), 8.0f, 0, 2.0f);

    // "YOU DIED" text
    const char* deathText = "YOU DIED";
    ImVec2 textSize = ImGui::CalcTextSize(deathText);
    dl->AddText(nullptr, 24.0f,
                ImVec2(ox + (overlayW - textSize.x * 1.7f) * 0.5f, oy + 8.0f),
                IM_COL32(220, 60, 60, 255), deathText);

    // Countdown timer
    if (rc->state == LifeState::DYING) {
        dl->AddText(nullptr, 16.0f,
                    ImVec2(ox + overlayW * 0.5f - 40.0f, oy + 45.0f),
                    IM_COL32(200, 200, 200, 255), "Dying...");
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Respawn in %.1fs", rc->respawnTimer);
        ImVec2 timerSize = ImGui::CalcTextSize(buf);
        dl->AddText(nullptr, 18.0f,
                    ImVec2(ox + (overlayW - timerSize.x * 1.3f) * 0.5f, oy + 45.0f),
                    IM_COL32(255, 255, 200, 255), buf);

        // Progress bar
        float barW = overlayW - 40.0f;
        float barH = 8.0f;
        float barX = ox + 20.0f;
        float barY = oy + 72.0f;
        float progress = (rc->totalRespawn > 0.0f)
                         ? 1.0f - (rc->respawnTimer / rc->totalRespawn)
                         : 0.0f;
        progress = std::max(0.0f, std::min(1.0f, progress));

        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                           IM_COL32(40, 40, 40, 200), 3.0f);
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * progress, barY + barH),
                           IM_COL32(100, 200, 100, 255), 3.0f);
    }

    // "Spectate Allies" button
    bool spectateClicked = false;
    ImGui::SetNextWindowPos(ImVec2(ox + (overlayW - 140.0f) * 0.5f, oy + overlayH - 32.0f));
    ImGui::SetNextWindowSize(ImVec2(140.0f, 28.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.15f, 0.2f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.3f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.4f, 0.6f, 1.0f));
    if (ImGui::Begin("##respawn_spectate", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::Button("Spectate Allies", ImVec2(136.0f, 24.0f)))
            spectateClicked = true;
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    return spectateClicked;
}

} // namespace glory
