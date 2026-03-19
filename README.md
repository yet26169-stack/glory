# Glory Engine

![CI Status](https://github.com/donkey-ux/glory/actions/workflows/ci.yml/badge.svg)

A custom Vulkan 1.3 MOBA game engine written in C++20, inspired by League of Legends and StarCraft II. Features a stylized toon-shading pipeline, GPU-driven rendering, ECS architecture, deterministic lockstep netcode, and Lua scripting.

## Features

- **Vulkan 1.3 renderer** — HDR pipeline, bloom, SSAO, tone mapping, dynamic shadows
- **Toon shading** — multi-band ramp lighting, Sobel inking, rim highlights
- **GPU-driven rendering** — indirect draw, Hi-Z occlusion culling, bindless textures
- **ECS (EnTT)** — parallel system scheduling with dependency DAG
- **Deterministic netcode** — lockstep with rollback over ENet, fixed-point math
- **Lua scripting** — ability system powered by sol2/Lua 5.4
- **3D spatial audio** — miniaudio integration with priority voice limiting
- **Flow field navigation** — GPU-accelerated pathfinding for minion waves
- **VFX system** — particles, trails, distortion, shield bubbles, explosions
- **HUD** — floating damage numbers, health bars, ability cooldowns, minimap, scoreboard

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

Debug build:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
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

```bash
cd build && ctest --output-on-failure
```

## Project Structure

```
src/
  ability/       AbilitySystem, ProjectileSystem
  animation/     AnimationPlayer, CPUSkinning
  assets/        AssetRegistry, CookedLoader
  audio/         AudioEngine (engine+handle+resource mgr), GameAudioEvents, miniaudio_impl
  camera/        Camera
  combat/        CombatSystem (combat+structures+respawn), NPCSystem (waves+behavior), EconomySystem, HeroRegistry, GpuCollisionSystem
  core/          Application, SimulationLoop, FramePacer, Systems (scheduler+gameplay), GameState, Log, Allocators
  fog/           FogSystem, FogOfWarGameplay
  hud/           HUD, HUDWidgets (floatingtext+healthbar+abilitybar+feed+respawn), Minimap, Scoreboard, PerfOverlay
  input/         InputManager
  map/           MapLoader
  nav/           NavMeshBuilder, PathfindingSystem, FlowField, DynamicObstacle, DebugRenderer
  network/       Netcode (inputsync+snapshot+gameloop), Transport, LobbySystem
  physics/       PhysicsSystem
  renderer/      Renderer, RenderGraph, Context, Device, Swapchain, Buffer, Image, Mesh, Model, Texture, Pipeline, Descriptors, PostProcessPasses, SpecializedRenderers, + others
  replay/        Replay (recorder+player)
  scene/         Scene
  scripting/     ScriptEngine, LuaBindings
  terrain/       IsometricCamera
  vfx/           VFXRenderer (renderer+trail+mesh), VFXLoader (loader+factory), ParticleSystem, CompositeVFXSequencer
  window/        Window
docs/
  ARCHITECTURE.md          Engine structure, Vulkan pipeline, VFX system, camera/physics
  GAMEPLAY.md              Ability system, units, NPCs, map, HUD
  IMPLEMENTATION_PLAN.md   Prioritized roadmap P0–P4
tests/
  19 test files covering: ability, animation, audio, combat, determinism, fixedpoint, flowfield,
  fog, jungle, maploader, minion, mirror, nav, networking, physics, replay, snapshot, structure, terrain
```

- **`assets/`** — JSON data, textures, and models
- **`shaders/`** — GLSL shader source (compiled to SPIR-V during build)
- **`extern/`** — Third-party libraries (EnTT, GLFW, GLM, spdlog, etc.)
- **`tools/`** — Asset cooking and development tools
