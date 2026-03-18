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

- **`src/`** — Core engine source code
  - `ability/` — Ability system, cooldowns, effects, projectiles, status effects
  - `animation/` — Skeletal animation, retargeting, blend trees
  - `assets/` — Asset loading and management utilities
  - `audio/` — 3D spatial audio engine (miniaudio integration)
  - `camera/` — Isometric camera and frustum math
  - `combat/` — Combat system, economy, structures, hero registry, minion waves
  - `core/` — App loop, simulation loop, threading, RNG, logging, profiler, allocators
  - `fog/` — Fog of War gameplay and visibility logic
  - `hud/` — UI components: health bars, floating text, minimap, scoreboard
  - `input/` — Input handling, mouse targeting, keyboard mapping
  - `map/` — Map loading (JSON), lane waypoints, symmetry logic
  - `math/` — Deterministic fixed-point math (`Fixed64`, `FixedVec3`, CORDIC trig)
  - `nav/` — Navigation: pathfinding (Recast/Detour), flow fields, splines, lane following
  - `network/` — Lockstep networking (ENet), rollback, input synchronization
  - `physics/` — Simple collision detection and rigid body physics
  - `renderer/` — Vulkan 1.3 pipeline, GPU culling, shadows, post-processing (Bloom, SSAO, SSR)
  - `replay/` — Replay recording, serialization, and playback
  - `scene/` — ECS component definitions and scene management
  - `scripting/` — Lua scripting integration (sol2 bindings)
  - `terrain/` — Terrain rendering and heightmap queries
  - `vfx/` — Visual effects: GPU particles, trails, mesh effects, composite sequencer
  - `window/` — GLFW window management and Vulkan surface integration

- **`tests/`** — Unit test suite (15+ test files for core simulation and math)
- **`assets/`** — JSON data, textures, and models
- **`shaders/`** — GLSL shader source (compiled to SPIR-V during build)
- **`extern/`** — Third-party libraries (EnTT, GLFW, GLM, spdlog, etc.)
- **`docs/`** — Architecture deep dives and design specifications
- **`tools/`** — Asset cooking and development tools
