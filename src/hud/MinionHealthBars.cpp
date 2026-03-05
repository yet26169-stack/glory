#include "hud/MinionHealthBars.h"
#include "minion/MinionComponents.h"
#include "scene/Components.h"

#include <imgui.h>

namespace glory {

void MinionHealthBars::draw(entt::registry &registry,
                            const glm::mat4 &viewProj,
                            float screenW, float screenH,
                            const glm::vec3 &cameraPos,
                            float maxRenderDist) {
  auto *drawList = ImGui::GetBackgroundDrawList();
  if (!drawList)
    return;

  auto view = registry.view<MinionTag, MinionIdentityComponent,
                             MinionHealthComponent, TransformComponent>();
  for (auto entity : view) {
    auto &hp = view.get<MinionHealthComponent>(entity);
    // Skip full-HP and dead minions
    if (hp.isDead || hp.currentHP >= hp.maxHP)
      continue;

    auto &transform = view.get<TransformComponent>(entity);

    // Distance cull
    float dist = glm::length(cameraPos - transform.position);
    if (dist > maxRenderDist)
      continue;

    // Project world position (offset above minion) to screen
    glm::vec4 worldPos(transform.position + glm::vec3(0.0f, 1.5f, 0.0f), 1.0f);
    glm::vec4 clip = viewProj * worldPos;
    if (clip.w <= 0.0f)
      continue; // behind camera

    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    float sx = (ndc.x * 0.5f + 0.5f) * screenW;
    float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH; // flip Y

    // Bar dimensions
    constexpr float BAR_W = 40.0f;
    constexpr float BAR_H = 5.0f;
    float left = sx - BAR_W * 0.5f;
    float top = sy - BAR_H * 0.5f;

    auto &id = view.get<MinionIdentityComponent>(entity);
    bool isBlue = (id.team == TeamID::Blue);

    // Background (dark team tint)
    ImU32 bgColor = isBlue ? IM_COL32(10, 15, 40, 180)
                           : IM_COL32(40, 10, 10, 180);
    drawList->AddRectFilled(ImVec2(left, top),
                            ImVec2(left + BAR_W, top + BAR_H), bgColor);

    // HP fill
    float ratio = hp.currentHP / hp.maxHP;
    ImU32 hpColor;
    if (ratio > 0.5f)
      hpColor = IM_COL32(50, 205, 50, 220);  // green
    else if (ratio > 0.25f)
      hpColor = IM_COL32(255, 165, 0, 220);  // orange
    else
      hpColor = IM_COL32(220, 20, 20, 220);  // red
    drawList->AddRectFilled(ImVec2(left, top),
                            ImVec2(left + BAR_W * ratio, top + BAR_H), hpColor);

    // Team-colored border
    ImU32 borderColor = isBlue ? IM_COL32(60, 120, 255, 200)
                               : IM_COL32(255, 60, 60, 200);
    drawList->AddRect(ImVec2(left, top),
                      ImVec2(left + BAR_W, top + BAR_H), borderColor);
  }
}

} // namespace glory
