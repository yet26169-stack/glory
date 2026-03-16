#include "hud/FloatingText.h"

#include <cmath>
#include <cstdio>

namespace glory {

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
        float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH; // flip Y

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

} // namespace glory
