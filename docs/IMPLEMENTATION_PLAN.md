# Glory — Implementation Plan

Roadmap for making a playable MOBA game. Prioritized by what blocks gameplay.

---

## Status Legend

✅ Done | 🔧 In Progress | ⬜ Pending

---

## Completed Work

### Engine Foundation (✅ Done)

All 15 renderer phases complete: Vulkan 1.3 instance, swapchain, VMA buffers, camera, model loading, textures, lighting, ECS, materials, deferred infrastructure, shadows (PCF 2048²), HDR + ACES tone mapping, ImGui overlay, frustum culling.

### Renderer Decomposition (✅ Done)

Extracted from the original 3,758-line `Renderer.cpp`:
- `SimulationLoop.h/.cpp` — fixed-timestep accumulator, 15-system tick order
- `SceneBuilder.h/.cpp` — scene construction (map tiles, champion, structures)
- `BoneSlotPool.h/.cpp` — bone slot allocation with free-queue recycling
- `RendererRecord.cpp` — command recording and pipeline creation

Renderer.cpp is now ~1,100 lines.

### Dead Code Cleanup (✅ Done)

Removed: 10 procedural texture factories (~820 lines), SSR system (SSRPass + ssr.comp), impostor system (ImpostorSystem + 4 shaders), demo entities (spawnTestEnemy + TestDummyTag), 2 unreferenced shaders (depth_only.vert, flow_field.comp), stale includes.

### P0 Bug Fixes (✅ Done)

- Vulkan queue family ownership: `VK_SHARING_MODE_CONCURRENT` in Image.cpp and Buffer.cpp
- Pipeline barrier stage fix: `FRAGMENT_SHADER_BIT` → `BOTTOM_OF_PIPE_BIT` in Texture.cpp
- VMA flush after staging uploads (all paths)
- Map asset filename typos fixed (read_team_tower_2 → red_team_tower_2, etc.)
- Z-up axis heuristic for GLB models (auto -90° X rotation)
- GLB textures wired through mapAssets system in SceneBuilder

### Core Systems (✅ Done)

| System | Status |
|--------|--------|
| 30Hz fixed-timestep simulation | ✅ Done |
| Vulkan 1.3 backend | ✅ Done |
| Shadow mapping (PCF, 2048²) | ✅ Done |
| Hi-Z occlusion culling | ✅ Done |
| Bloom, SSAO, tone mapping | ✅ Done |
| Recast/Detour navigation + flow fields | ✅ Done |
| Ability system (data-driven, Lua scripted) | ✅ Done |
| Deterministic lockstep netcode | ✅ Done |
| GPU particle system | ✅ Done |
| Skeletal animation (CPU + GPU skinning) | ✅ Done |
| Replay system | ✅ Done |
| Spatial audio | ✅ Done |

---

## P1 — Gameplay Polish (next up)

These are the remaining items needed for a playable MOBA match.

### Gameplay

| Feature | Status | Notes |
|---------|--------|-------|
| Auto-attack system tuning | 🔧 In Progress | Target acquisition, damage timing, attack-move |
| Minion wave balance | 🔧 In Progress | Spawn timing, gold rewards, aggro rules |
| Structure health/damage | 🔧 In Progress | Tower shot cycle, inhibitor respawn |
| Jungle camp respawn timers | 🔧 In Progress | Camp AI, leash range, patience reset |
| Fog of War team visibility | ⬜ Pending | Separate FoW masks per team, minimap integration |
| Economy system | ⬜ Pending | Gold income, last-hit rewards, XP/level scaling |
| Full ability kit (QWER) | ⬜ Pending | 4 abilities per champion, cooldowns, mana costs |

### Animation

| Feature | Status | Notes |
|---------|--------|-------|
| Animation state machine | ⬜ Pending | Idle→Walk→Cast→Die transitions |
| Root motion extraction | ⬜ Pending | Movement driven by animation data |
| Animation events | ⬜ Pending | Footstep sounds, hitbox activation, VFX spawn |

### Performance

| Feature | Status | Notes |
|---------|--------|-------|
| GPU skinning for minions | ⬜ Pending | Move 24+ minions from CPU to GPU compute skinning |
| Multi-threaded command recording | ⬜ Pending | Vulkan secondary command buffers |
| Cooked asset pipeline | ⬜ Pending | GLB → binary format at build time |

---

## P2 — VFX & Polish

| Feature | Status | Notes |
|---------|--------|-------|
| Soft particles | ⬜ Pending | Depth-test fade at geometry intersection |
| Atlas sprite animation | ⬜ Pending | Animated sprite sheets for VFX |
| Water rendering | ⬜ Pending | Shaders exist (`water.vert/frag`), need integration |
| VFX test coverage | ⬜ Pending | Unit tests for particle lifecycle, emitter loading |

---

## P3 — Multiplayer & Competitive

| Feature | Status | Notes |
|---------|--------|-------|
| 5v5 matchmaking | ⬜ Pending | Server-authoritative with ENet |
| Spectator mode | ⬜ Pending | Delayed state stream from server |
| Dragon / Baron objectives | ⬜ Pending | Unique monster AI + global buffs |
| Item shop UI | ⬜ Pending | Gold economy → stat upgrades |

---

## Key Architecture Decisions

**GLB Loading Pipeline:**
```
SceneBuilder::build()
├─ Model::loadFromGLB()           → mesh vertex data (parallel, per file)
├─ Scene::addMesh(model)          → uint32_t meshIdx
├─ Model::loadGLBTextures()       → GLBTexture[] with materialIndex
├─ Scene::addTexture()            → uint32_t texIdx
├─ BindlessDescriptors::register  → bindless array slot
└─ Entity creation: MeshComponent + MaterialComponent (per-submesh textures)
```

**Shader Binding Layout:**

| Binding | Set | Purpose |
|---------|-----|---------|
| 0 | 0 | Camera/transform UBO |
| 2 | 0 | Light UBO (4 lights, fog params) |
| 3 | 0 | Shadow map |
| 5 | 0 | Toon ramp (256×1) |
| 6 | 0 | Fog of War (512×512 R8) |
| 7 | 0 | Scene SSBO (GpuObjectData[]) |
| 0 | 1 | Bindless textures (sampler2D[4096]) |

**Performance Budget (1080p, 60 FPS):**

| System | Budget | Current |
|--------|--------|---------|
| Shadow pass | 1.5 ms | ~0.5 ms |
| Scene geometry | 3.0 ms | ~1.0 ms |
| Post-process | 0.5 ms | ~0.3 ms |
| CPU simulation (30 Hz) | 2.0 ms | ~0.3 ms |
| **Total frame** | **16.6 ms** | **~3 ms** |
