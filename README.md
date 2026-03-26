# Glory

![CI Status](https://github.com/yet26169-stack/glory/actions/workflows/ci.yml/badge.svg)

A simple, pragmatic MOBA game built on a custom Vulkan 1.3 engine in C++20. One map, one champion, minion waves, towers, jungle camps, abilities, and a HUD — everything needed for a playable game, nothing more.

## What's in the box

- **Vulkan 1.3 renderer** — toon shading, HDR tone mapping, dynamic shadows, SSAO, bloom
- **GPU-driven pipeline** — indirect draw, Hi-Z occlusion culling, bindless textures (4096 slots)
- **MOBA gameplay** — minion waves, tower/inhibitor/nexus structures, jungle camps, auto-attacks
- **Abilities** — data-driven ability system with projectiles, AoE, status effects (Lua scripted)
- **VFX** — GPU particles, trails, explosions, shield bubbles, cone effects, distortion
- **Navigation** — Recast/Detour pathfinding with flow fields for minion steering
- **Networking** — deterministic lockstep with rollback over ENet
- **HUD** — health bars, ability bar, minimap, scoreboard, floating damage numbers
- **Audio** — 3D spatial audio via miniaudio
- **Replay** — deterministic replay recording and playback

## Dependencies

| Dependency | Purpose |
|------------|---------|
| [Vulkan SDK](https://vulkan.lunarg.com/) ≥ 1.3 | Graphics API |
| glfw3 | Windowing & input |
| glm | Math library |
| spdlog | Logging |
| glslc (in Vulkan SDK) | Shader compilation |

Bundled in `extern/`: EnTT, meshoptimizer, TinyGLTF, stb, ImGui, miniaudio, ENet, sol2, Lua 5.4.

## Build

```bash
cmake -B build
cmake --build build --parallel
```

Cook optimized assets (optional):
```bash
cmake --build build --target cook_assets
```

## Run

```bash
./build/Glory
```

### Flags

| Flag | Description |
|------|-------------|
| `--server` | Host a game server |
| `--connect <ip>` | Connect to a server |
| `--port <n>` | Network port (default: 7777) |
| `--players <n>` | Expected player count |

### Controls

| Key | Action |
|-----|--------|
| Right-click | Move / attack target |
| Q / W / E / R | Abilities |
| Tab | Scoreboard |
| Space | Center camera on champion |
| Mouse wheel | Zoom |
| F1 | Toggle debug overlay |
| Esc | Menu |

## Tests

12 tests covering combat, abilities, animation, fog of war, navigation, physics, replay, and more:

```bash
cd build && ctest --output-on-failure
```

## Project Structure

```
src/
  ability/       Ability system, projectiles, status effects
  animation/     Skeletal animation player, CPU/GPU skinning
  assets/        Asset registry, cooked asset loader
  audio/         Spatial audio engine (miniaudio)
  camera/        Isometric camera controller
  combat/        Combat state machine, structures, economy, hero registry
  core/          Application loop, simulation loop, system scheduler, game state
  fog/           Fog of War visibility and rendering
  hud/           ImGui game HUD (health bars, abilities, minimap, scoreboard)
  input/         Input manager and targeting
  map/           Map data loading and lane definitions
  nav/           Recast/Detour pathfinding, flow fields, lane followers
  network/       Lockstep netcode, input sync, state snapshots
  physics/       Collision detection and physics integration
  renderer/      Vulkan 1.3 renderer, render graph, post-processing, VFX renderers
  replay/        Deterministic replay recorder and player
  scene/         ECS scene graph (EnTT) and components
  scripting/     Lua script engine and ability bindings
  terrain/       Terrain heightmaps
  vfx/           GPU particle system, trails, mesh effects, VFX sequencer
  window/        GLFW window management
shaders/         56 GLSL shaders (compiled to SPIR-V at build time)
assets/          JSON ability/VFX definitions, textures, models
docs/            Architecture, gameplay, and implementation plan
tests/           19 test files (12 active CTest targets)
```
