# HUD & Click Indicator — Implementation Plan

## Overview

Add a full MOBA-style heads-up display (HUD) and a world-space click indicator to the Glory engine. The HUD renders via Dear ImGui (already integrated for `DebugOverlay`). The click indicator is a world-space animated ring rendered via `DebugRenderer` (line renderer already present) or a dedicated quad pipeline.

---

## Architecture Context

| System | File(s) | Role |
|---|---|---|
| Dear ImGui integration | `DebugOverlay.h/cpp` | Already initialised with Vulkan backend; renders in the post-process pass after tone-mapping |
| Input | `InputManager.h/cpp` | GLFW key/mouse callbacks; `wasRightClicked()`, `getLastClickPos()`, `getMousePos()`, ability keys Q/W/E/R |
| Ability data | `AbilityComponents.h`, `AbilityDef.h`, `AbilityTypes.h` | `AbilityBookComponent` (Q/W/E/R/Passive/Summoner), `CombatStatsComponent` (HP, mana, stats), `AbilityInstance` (cooldown, phase) |
| World-space debug lines | `DebugRenderer.h/cpp` | `drawLine()`, `drawCircle()`, `drawSphere()` — rendered in HDR pass |
| Renderer frame flow | `Renderer.cpp` | `drawFrame()` → `recordCommandBuffer()` → shadow pass → HDR scene pass (terrain, meshes, particles, debug lines) → post-process pass (tone-map, then ImGui overlay) |
| Camera | `IsometricCamera.h/cpp` | MOBA camera: perspective, FOV 45°, zoom 15–50, pitch 56°, follows character |

---

## Part 1 — Click Indicator

A green shrinking ring on the terrain where the player right-clicks. Fades and shrinks over ~0.5 s, then disappears.

### 1.1 Data structure

**File:** `src/renderer/Renderer.h`

```cpp
struct ClickIndicator {
    glm::vec3 position{0.0f};   // world-space click position (snapped to terrain Y)
    float     lifetime = 0.0f;  // remaining seconds (starts at 0.5)
    float     maxLife  = 0.5f;  // total duration
};
```

Add member:
```cpp
std::optional<ClickIndicator> m_clickIndicator;
```

### 1.2 Spawn on right-click

**File:** `src/renderer/Renderer.cpp`, inside the existing right-click handler block (~line 292–320)

After setting `character.targetPosition = hitPos`:
```cpp
m_clickIndicator = ClickIndicator{hitPos, 0.5f, 0.5f};
```

### 1.3 Update each frame

**File:** `src/renderer/Renderer.cpp`, at the top of `recordCommandBuffer()` (or in `drawFrame()` before recording)

```cpp
if (m_clickIndicator) {
    m_clickIndicator->lifetime -= deltaTime;
    if (m_clickIndicator->lifetime <= 0.0f)
        m_clickIndicator.reset();
}
```

### 1.4 Render with DebugRenderer

**File:** `src/renderer/Renderer.cpp`, where `m_debugRenderer.clear()` / `m_debugRenderer.render()` are called

After clearing the debug renderer's per-frame data:
```cpp
if (m_clickIndicator) {
    float t = m_clickIndicator->lifetime / m_clickIndicator->maxLife; // 1→0
    float radius = 0.3f + 0.7f * t;        // shrinks from 1.0 to 0.3
    float alpha  = t;                       // fades out
    glm::vec4 color(0.2f, 1.0f, 0.4f, alpha); // green
    glm::vec3 pos = m_clickIndicator->position;
    pos.y += 0.05f;                         // slight offset above terrain to avoid z-fight
    m_debugRenderer.drawCircle(pos, radius, color, 48);
}
```

The `DebugRenderer` already supports alpha through vertex color (the debug shader uses vertex color directly). If the debug pipeline has blending disabled, enable `blendEnable = VK_TRUE` with standard alpha blending in `DebugRenderer::init()`.

### 1.5 (Optional enhancement) Textured quad indicator

For a higher-quality indicator (texture instead of lines), create a minimal pipeline:

1. Add `shaders/click_indicator.vert` and `shaders/click_indicator.frag` — a single billboard quad with a radial gradient ring texture, UV-animated to shrink.
2. Generate a 64×64 procedural ring texture at init (white ring on transparent background).
3. Render as a single instanced quad in the HDR pass after terrain, before particles.

This is optional polish — the `DebugRenderer` circle is sufficient for a first pass.

---

## Part 2 — HUD System

### 2.1 New files

| File | Purpose |
|---|---|
| `src/hud/HUD.h` | Class declaration |
| `src/hud/HUD.cpp` | Rendering logic using Dear ImGui |

Add both to `CMakeLists.txt` under the `Glory` target's source list.

### 2.2 HUD class interface

```cpp
// src/hud/HUD.h
#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>

namespace glory {

class Scene;
struct CombatStatsComponent;
struct AbilityBookComponent;

class HUD {
public:
    void init(float screenWidth, float screenHeight);
    void resize(float screenWidth, float screenHeight);

    // Call once per frame between ImGui::NewFrame() and ImGui::Render()
    void draw(const Scene& scene, entt::entity player);

private:
    void drawHealthBar(const CombatStatsComponent& stats);
    void drawResourceBar(const CombatStatsComponent& stats);
    void drawAbilityBar(const AbilityBookComponent& book);
    void drawStatPanel(const CombatStatsComponent& stats);
    void drawXPBar(const CombatStatsComponent& stats);
    void drawMinimap();       // placeholder for future minimap
    void drawKillFeed();      // placeholder
    void drawTooltip(const struct AbilityInstance& ability);

    float m_screenW = 1280.0f;
    float m_screenH = 720.0f;
};

} // namespace glory
```

### 2.3 HUD layout (MOBA standard)

```
┌──────────────────────────────────────────────────────────────────┐
│  [Minimap placeholder]                              [KDA / Timer]│
│  (bottom-left, 180×180)                          (top-right)     │
│                                                                  │
│                        GAME VIEW                                 │
│                                                                  │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────────┐│
│  │ [Portrait] [HP bar]  [═══ XP bar ═══]      [Stats]          ││
│  │            [MP bar]  [ Q ][ W ][ E ][ R ]  [AD/AP/ARM/MR]  ││
│  └──────────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────────┘
```

### 2.4 Health & resource bars

**Inside `HUD::drawHealthBar()`:**

```cpp
void HUD::drawHealthBar(const CombatStatsComponent& stats) {
    float barW = 300.0f, barH = 22.0f;
    float x = (m_screenW - barW) * 0.5f - 80.0f;
    float y = m_screenH - 65.0f;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barW, y + barH),
                      IM_COL32(20, 20, 20, 200), 4.0f);

    // Health fill
    float frac = std::clamp(stats.currentHP / stats.maxHP, 0.0f, 1.0f);
    ImU32 hpColor = frac > 0.5f ? IM_COL32(50, 205, 50, 255)   // green
                  : frac > 0.25f ? IM_COL32(255, 165, 0, 255)   // orange
                  : IM_COL32(220, 20, 20, 255);                  // red
    dl->AddRectFilled(ImVec2(x + 1, y + 1),
                      ImVec2(x + 1 + (barW - 2) * frac, y + barH - 1),
                      hpColor, 3.0f);

    // Shield overlay (lighter bar on top of HP)
    if (stats.shield > 0.0f) {
        float shieldFrac = std::clamp(stats.shield / stats.maxHP, 0.0f, 1.0f - frac);
        float shieldStart = x + 1 + (barW - 2) * frac;
        dl->AddRectFilled(ImVec2(shieldStart, y + 1),
                          ImVec2(shieldStart + (barW - 2) * shieldFrac, y + barH - 1),
                          IM_COL32(200, 200, 200, 180), 3.0f);
    }

    // Text
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f / %.0f", stats.currentHP, stats.maxHP);
    ImVec2 textSize = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(x + (barW - textSize.x) * 0.5f, y + 2), IM_COL32_WHITE, buf);
}
```

**`drawResourceBar()`** — identical layout shifted down by 26px, blue fill for mana, orange for energy, red for rage (switch on `stats.resourceType`).

### 2.5 Ability bar

```cpp
void HUD::drawAbilityBar(const AbilityBookComponent& book) {
    const float iconSize = 48.0f;
    const float spacing  = 8.0f;
    const float totalW   = 4 * iconSize + 3 * spacing;
    float startX = (m_screenW - totalW) * 0.5f;
    float y      = m_screenH - 120.0f;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const char* labels[] = {"Q", "W", "E", "R"};
    AbilitySlot slots[] = {AbilitySlot::Q, AbilitySlot::W,
                           AbilitySlot::E, AbilitySlot::R};

    for (int i = 0; i < 4; ++i) {
        float x = startX + i * (iconSize + spacing);
        const auto& ab = book.get(slots[i]);

        // Background
        ImU32 bgColor = (ab.level > 0) ? IM_COL32(40, 40, 60, 220)
                                        : IM_COL32(30, 30, 30, 180);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + iconSize, y + iconSize),
                          bgColor, 6.0f);

        // Cooldown sweep overlay
        if (ab.cooldownRemaining > 0.0f && ab.def) {
            float cdFrac = ab.cooldownRemaining / ab.def->cooldown;
            // Dark overlay proportional to remaining cooldown
            float overlayH = iconSize * cdFrac;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + iconSize, y + overlayH),
                              IM_COL32(0, 0, 0, 160), 6.0f);
            // Cooldown text
            char cd[8];
            snprintf(cd, sizeof(cd), "%.1f", ab.cooldownRemaining);
            ImVec2 cdSize = ImGui::CalcTextSize(cd);
            dl->AddText(ImVec2(x + (iconSize - cdSize.x) * 0.5f,
                               y + (iconSize - cdSize.y) * 0.5f),
                        IM_COL32(255, 255, 255, 200), cd);
        }

        // Key label (top-left corner)
        dl->AddText(ImVec2(x + 4, y + 2),
                    ab.level > 0 ? IM_COL32(255, 220, 80, 255)
                                 : IM_COL32(120, 120, 120, 200),
                    labels[i]);

        // Ability name (bottom, small)
        if (ab.def) {
            const char* name = ab.def->name.c_str();
            ImVec2 nameSize = ImGui::CalcTextSize(name);
            if (nameSize.x < iconSize)
                dl->AddText(ImVec2(x + (iconSize - nameSize.x) * 0.5f,
                                   y + iconSize - 14),
                            IM_COL32(200, 200, 200, 180), name);
        }

        // Border
        ImU32 borderColor = (ab.currentPhase != AbilityPhase::READY && ab.level > 0)
                          ? IM_COL32(255, 200, 50, 255)   // casting: gold
                          : IM_COL32(80, 80, 100, 255);   // idle: subtle
        dl->AddRect(ImVec2(x, y), ImVec2(x + iconSize, y + iconSize),
                    borderColor, 6.0f, 0, 2.0f);

        // Hover tooltip (check if mouse is over this icon)
        ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.x >= x && mouse.x <= x + iconSize &&
            mouse.y >= y && mouse.y <= y + iconSize && ab.def) {
            drawTooltip(ab);
        }
    }
}
```

### 2.6 Tooltip

```cpp
void HUD::drawTooltip(const AbilityInstance& ab) {
    ImGui::BeginTooltip();
    ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s", ab.def->name.c_str());
    ImGui::TextWrapped("%s", ab.def->description.c_str());
    ImGui::Separator();
    ImGui::Text("Cooldown: %.1fs", ab.def->cooldown);
    ImGui::Text("Cost: %.0f %s", ab.def->cost,
                ab.def->costType == ResourceType::MANA ? "mana" : "resource");
    ImGui::Text("Range: %.0f", ab.def->range);
    if (ab.level > 0)
        ImGui::Text("Level: %d", ab.level);
    else
        ImGui::TextDisabled("Not learned");
    ImGui::EndTooltip();
}
```

### 2.7 Stats panel (right side of bottom bar)

```cpp
void HUD::drawStatPanel(const CombatStatsComponent& stats) {
    float x = m_screenW * 0.5f + 180.0f;
    float y = m_screenH - 120.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 160, y + 96),
                      IM_COL32(20, 20, 30, 200), 6.0f);

    auto statLine = [&](float cy, const char* label, float val, ImU32 color) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s: %.0f", label, val);
        dl->AddText(ImVec2(x + 8, cy), color, buf);
    };

    statLine(y + 4,  "AD",  stats.attackDamage,  IM_COL32(255, 150, 50, 255));
    statLine(y + 18, "AP",  stats.abilityPower,  IM_COL32(100, 150, 255, 255));
    statLine(y + 32, "ARM", stats.armor,         IM_COL32(255, 200, 50, 255));
    statLine(y + 46, "MR",  stats.magicResist,   IM_COL32(200, 100, 255, 255));
    statLine(y + 60, "AS",  stats.attackSpeed,    IM_COL32(255, 255, 100, 255));
    statLine(y + 74, "MS",  stats.moveSpeed,      IM_COL32(150, 255, 150, 255));
}
```

### 2.8 XP bar (thin bar above the ability bar)

```cpp
void HUD::drawXPBar(const CombatStatsComponent& stats) {
    float barW = 340.0f, barH = 6.0f;
    float x = (m_screenW - barW) * 0.5f;
    float y = m_screenH - 128.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barW, y + barH),
                      IM_COL32(20, 20, 20, 200), 2.0f);

    // Placeholder: level / 18 as progress
    float frac = std::clamp(static_cast<float>(stats.level) / 18.0f, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(x + 1, y + 1),
                      ImVec2(x + 1 + (barW - 2) * frac, y + barH - 1),
                      IM_COL32(80, 50, 200, 255), 1.0f);

    char lvl[16];
    snprintf(lvl, sizeof(lvl), "Lv %d", stats.level);
    dl->AddText(ImVec2(x + barW + 4, y - 2), IM_COL32(200, 180, 255, 255), lvl);
}
```

### 2.9 Minimap placeholder

```cpp
void HUD::drawMinimap() {
    float size = 180.0f;
    float x = 10.0f;
    float y = m_screenH - size - 10.0f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Dark background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size),
                      IM_COL32(10, 15, 10, 220), 4.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + size, y + size),
                IM_COL32(60, 80, 60, 255), 4.0f, 0, 2.0f);

    // "MINIMAP" placeholder text
    const char* label = "MINIMAP";
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(x + (size - ts.x) * 0.5f, y + (size - ts.y) * 0.5f),
                IM_COL32(60, 80, 60, 150), label);
}
```

### 2.10 Main `draw()` function

```cpp
void HUD::draw(const Scene& scene, entt::entity player) {
    auto& reg = scene.getRegistry();
    auto* stats = reg.try_get<CombatStatsComponent>(player);
    auto* book  = reg.try_get<AbilityBookComponent>(player);
    if (!stats) return;

    drawMinimap();
    drawHealthBar(*stats);
    drawResourceBar(*stats);
    drawXPBar(*stats);
    if (book) drawAbilityBar(*book);
    drawStatPanel(*stats);
}
```

---

## Part 3 — Integration into Renderer

### 3.1 Add HUD member

**File:** `src/renderer/Renderer.h`

```cpp
#include "hud/HUD.h"
// ...
HUD m_hud;
entt::entity m_playerEntity = entt::null;
```

### 3.2 Initialise

**File:** `src/renderer/Renderer.cpp`, at the end of `buildScene()` (after character entity is created)

```cpp
m_playerEntity = character;
m_hud.init(static_cast<float>(m_swapchain->getExtent().width),
           static_cast<float>(m_swapchain->getExtent().height));
```

### 3.3 Handle resize

**File:** `src/renderer/Renderer.cpp`, in `recreateSwapchain()`

```cpp
m_hud.resize(static_cast<float>(m_swapchain->getExtent().width),
             static_cast<float>(m_swapchain->getExtent().height));
```

### 3.4 Draw HUD each frame

**File:** `src/renderer/Renderer.cpp`

The HUD must be drawn between `ImGui::NewFrame()` and `ImGui::Render()`. Currently, `DebugOverlay::beginFrame()` calls `NewFrame()` and builds the debug window; `endFrame()` calls `ImGui::Render()`.

Insert the HUD draw call between `beginFrame()` and `endFrame()`:

```cpp
m_overlay->beginFrame();      // calls ImGui::NewFrame() + builds debug window

// Draw game HUD (uses ImGui's background draw list, so it renders behind
// the debug overlay window but on top of the game scene)
if (m_mobaMode && m_playerEntity != entt::null)
    m_hud.draw(m_scene, m_playerEntity);

m_overlay->endFrame();        // calls ImGui::Render()
```

This works because `ImGui::GetBackgroundDrawList()` is a global draw list rendered behind all ImGui windows. The HUD elements use this list, so they appear behind the debug overlay (which uses a window) but on top of the game scene.

### 3.5 Click indicator rendering

Already covered in Part 1 above. The key integration point:

- **Spawn:** in the right-click handler (`Renderer.cpp` ~line 292)
- **Update:** in `drawFrame()` before recording
- **Render:** add `drawCircle()` calls to `m_debugRenderer` before `m_debugRenderer.render()` in the HDR pass

### 3.6 CMakeLists.txt

Add to the `Glory` target source list:
```cmake
src/hud/HUD.cpp
```

No new shaders needed — everything renders through Dear ImGui and the existing `DebugRenderer`.

---

## Part 4 — Detailed File Change Summary

| File | Change | Lines (est.) |
|---|---|---|
| `src/hud/HUD.h` | **New file** — class declaration | ~40 |
| `src/hud/HUD.cpp` | **New file** — all draw functions using ImGui `GetBackgroundDrawList()` | ~250 |
| `src/renderer/Renderer.h` | Add `HUD m_hud`, `entt::entity m_playerEntity`, `std::optional<ClickIndicator> m_clickIndicator`, `ClickIndicator` struct | ~15 |
| `src/renderer/Renderer.cpp` | Spawn click indicator on right-click, update lifetime, draw circle via DebugRenderer; init/resize HUD; call `m_hud.draw()` between overlay begin/end; store `m_playerEntity` | ~30 |
| `src/nav/DebugRenderer.cpp` | (Optional) Enable alpha blending on the debug line pipeline if not already enabled | ~5 |
| `CMakeLists.txt` | Add `src/hud/HUD.cpp` to sources | ~1 |

**Total new code:** ~340 lines  
**Modified code:** ~50 lines  
**No new shaders, no new Vulkan pipelines, no new descriptor sets.**

---

## Part 5 — Implementation Order

### Phase 1: Click indicator (fastest visible result)
1. Add `ClickIndicator` struct + `m_clickIndicator` to `Renderer.h`
2. Spawn in right-click handler
3. Tick lifetime in `drawFrame()`
4. Draw green circle in `DebugRenderer` before flush
5. (Optional) Enable alpha blending on debug pipeline
6. **Test:** right-click on terrain → green ring shrinks and fades

### Phase 2: HUD skeleton
1. Create `src/hud/HUD.h` and `src/hud/HUD.cpp`
2. Implement `drawHealthBar()` and `drawResourceBar()` only
3. Wire into Renderer (init, resize, draw between ImGui begin/end)
4. Add to `CMakeLists.txt`
5. **Test:** HP and mana bars appear at bottom-center, correct values

### Phase 3: Ability bar
1. Implement `drawAbilityBar()` with cooldown sweeps and key labels
2. Implement `drawTooltip()` for hover info
3. **Test:** Q/W/E/R icons show, cooldowns animate after casting, tooltips appear on hover

### Phase 4: Stats + XP + minimap
1. Implement `drawStatPanel()` — AD, AP, armor, MR, AS, MS
2. Implement `drawXPBar()` — level display
3. Implement `drawMinimap()` — placeholder rectangle
4. **Test:** full bottom bar layout looks correct at 720p and 1080p

### Phase 5: Polish
1. Tune colors, sizes, and positions for 16:9 aspect ratios
2. Add subtle background panel behind entire bottom bar for cohesion
3. Add floating damage/heal numbers (optional — `DebugRenderer::drawText()` or ImGui foreground list)
4. Animate HP bar changes (smooth lerp from old to new value)
5. Add sound trigger hooks for low-HP warning threshold

---

## Part 6 — Design Decisions & Rationale

| Decision | Rationale |
|---|---|
| **ImGui `GetBackgroundDrawList()` for HUD** | Zero new Vulkan pipelines needed; renders behind debug windows; GPU cost negligible; already initialised |
| **DebugRenderer for click indicator** | World-space circles already work; renders in HDR pass so it respects depth/lighting; no new shaders |
| **No separate HUD render pass** | ImGui already renders in the post-process pass; adding another pass is unnecessary overhead |
| **`std::optional<ClickIndicator>`** | At most one click indicator exists at a time; avoids vector overhead |
| **Per-frame `drawCircle()` instead of persistent geometry** | Click indicator is transient (0.5 s); no need for GPU buffer management |
| **Y-X-Z rotation order** | Already fixed in `getModelMatrix()` — HUD doesn't need any special rotation handling |
| **Cooldown shown as top-down wipe** | Standard MOBA convention (dark overlay descends as cooldown progresses); easy to implement with `AddRectFilled` height proportional to remaining fraction |

---

## Part 7 — Future Extensions (not in current scope)

- **Minimap rendering:** Render top-down orthographic view of terrain + entity positions into a small offscreen framebuffer, display as ImGui texture
- **Item shop UI:** ImGui window with grid layout, drag-and-drop
- **Scoreboard (Tab key):** ImGui table overlay with KDA, items, gold for all players
- **Buff/debuff icons:** Row of small icons above HP bar with duration timers (read from `StatusEffectsComponent`)
- **Floating combat text:** World-space billboarded numbers for damage/heals (requires text rendering pipeline or ImGui foreground draw list with world-to-screen projection)
- **Ping system:** Alt-click to place map pings (world-space icon + minimap marker)
