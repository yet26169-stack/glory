# Glory Engine — AAA MOBA/RTS Implementation Plan

> **Authoritative Source:** This document is the sole engineering specification for making the Glory Engine production-ready for a AAA MOBA/RTS title. All agents must read Phases -1 and -0.5 in full before modifying any code.

---

## Table of Contents

- [Phase -1: Codebase Audit](#phase--1-codebase-audit)
- [Phase -0.5: Renderer Decomposition](#phase--05-renderer-decomposition)
- [Phase 0.1: Fixed-Point Deterministic Math](#phase-01-fixed-point-deterministic-math)
- [Phase 0.2: Simulation Tick Decoupling](#phase-02-simulation-tick-decoupling)
- [Phase 0.3: State Snapshots and Checksums](#phase-03-state-snapshots-and-checksums)
- [Phase 1.1: Networking — Lock-Step with Rollback](#phase-11-networking--lock-step-with-rollback)
- [Phase 2.1: Flow Fields and Steering](#phase-21-flow-fields-and-steering)
- [Phase 3: Fog of War (GPU Compute Extension)](#phase-3-fog-of-war-gpu-compute-extension)
- [Phase 4.1: Optimize Existing Multi-Threaded Recording](#phase-41-optimize-existing-multi-threaded-recording)
- [Phase 4.2: Hi-Z Occlusion Culling](#phase-42-hi-z-occlusion-culling)
- [Phase 4.3: Bindless Resources](#phase-43-bindless-resources)
- [Phase 4.4: Visibility Buffer](#phase-44-visibility-buffer)
- [Phase 4.5: Pipeline Libraries](#phase-45-pipeline-libraries)
- [Phase 4.6: VMA Configuration Optimization](#phase-46-vma-configuration-optimization)
- [Phase 5: Async Compute Pipeline](#phase-5-async-compute-pipeline)
- [Phase 6: Polish — Cooked Assets, Debug Tools, Test Suite](#phase-6-polish--cooked-assets-debug-tools-test-suite)
- [Appendix A: File Structure](#appendix-a-file-structure)
- [Appendix B: Dependency Graph](#appendix-b-dependency-graph)
- [Appendix C: Third-Party Dependencies](#appendix-c-third-party-dependencies)

---

## Phase -1: Codebase Audit

> **MANDATORY PREREQUISITE.** No implementation agent may skip this phase. The audit findings below document the actual repository state and prevent agents from duplicating existing work or creating conflicting file structures.

### Actual `src/` Directory Structure

```
src/
├── ability/
│   ├── AbilityComponents.h         (CombatStatsComponent, StatusEffectsComponent, etc.)
│   ├── AbilityDef.h/.cpp           (AbilityDef struct, JSON deserialization)
│   ├── AbilitySystem.h/.cpp        (ability activation, cooldown gating, effect dispatch)
│   ├── AbilityTypes.h              (enums: AbilityTargetType, DamageType, etc.)
│   ├── CooldownSystem.h/.cpp       (per-entity cooldown tick)
│   ├── EffectSystem.h/.cpp         (pending effect application pipeline)
│   ├── ProjectileSystem.h/.cpp     (projectile movement, collision, hit callbacks)
│   ├── StatusEffectSystem.h/.cpp   (DoT/HoT/buff/debuff duration + tick)
│   └── VFXEventQueue.h             (ring buffer for renderer VFX notifications)
├── audio/
│   └── AudioEngine.h/.cpp
├── camera/
│   └── Camera.h/.cpp
├── core/
│   ├── Application.h/.cpp
│   ├── Log.h/.cpp
│   └── Profiler.h/.cpp             (GLORY_PROFILE_SCOPE macro, per-frame timing)
├── editor/
│   ├── DebugOverlay.h              (ImGui debug overlay — GPU + CPU timings)
│   └── DebugOverlay.cpp
├── fog/
│   └── FogSystem.h/.cpp            *** ALREADY EXISTS — do NOT recreate ***
├── input/
│   ├── InputManager.h/.cpp
│   └── TargetingSystem.h           (click-to-target raycast)
├── map/
│   ├── MapLoader.h/.cpp            (JSON map file → MapData)
│   ├── MapSymmetry.h               (lane mirror utilities)
│   └── MapTypes.h                  (MapData, LaneData, EntityType, TeamID)
├── nav/
│   ├── DebugRenderer.h/.cpp        (draw lane splines + nav debug lines)
│   ├── LaneFollower.h              *** EXISTING — preserve for lane minions ***
│   ├── SpawnSystem.h               (wave timer, spawn point selection)
│   ├── SplineUtil.h                (catmull-rom evaluation, arc-length param)
│   └── (no FlowField yet — to be added in Phase 2.1)
├── physics/
│   └── PhysicsEngine.h/.cpp
├── renderer/
│   ├── Bloom.h/.cpp
│   ├── Buffer.h/.cpp               (VMA-backed buffer allocation)
│   ├── CascadeShadow.h/.cpp        (3-cascade CSM)
│   ├── ClickIndicatorRenderer.h/.cpp
│   ├── ComputeSkinner.h/.cpp       (GPU compute skinning, >50 entity threshold)
│   ├── Context.h/.cpp              (VkInstance, validation layers)
│   ├── Descriptors.h/.cpp          (descriptor pool + layout management)
│   ├── Device.h/.cpp               (physical/logical device, queue families)
│   ├── Frustum.h                   (AABB + plane frustum test)
│   ├── GBuffer.h/.cpp              (position/normal/albedo G-buffer)
│   ├── GLBLoader.cpp               (tinygltf → internal Mesh/Skeleton, meshopt_simplify)
│   ├── GpuCuller.h/.cpp            (GPU frustum culling compute, indirect draw gen)
│   ├── GpuProfiler.h/.cpp          (VkQueryPool timestamp queries)
│   ├── Image.h/.cpp                (VkImage lifecycle + layout transitions)
│   ├── Material.h/.cpp
│   ├── Mesh.h/.cpp
│   ├── Model.h/.cpp
│   ├── ParticleSystem.h/.cpp       (GPU particle simulation via particle_sim.comp)
│   ├── Pipeline.h/.cpp
│   ├── PostProcess.h/.cpp          (HDR + FXAA + ACES tone map + specialization variants)
│   ├── Renderer.h/.cpp             (170,473 bytes — monolithic, see Phase -0.5)
│   ├── SSAO.h/.cpp
│   ├── ShadowMap.h/.cpp
│   ├── StaticSkinnedMesh.h         (shared GPU vertex buffer for skinned meshes)
│   ├── Swapchain.h/.cpp
│   ├── Sync.h/.cpp
│   ├── Texture.h/.cpp
│   ├── TextureStreamer.h/.cpp      (async stb_image + mip gen via transfer queue)
│   └── VkCheck.h                   (VK_CHECK macro)
├── scene/
│   ├── Components.h                (TagComponent, TransformComponent, MeshComponent,
│   │                                MaterialComponent, LightComponent, RotateComponent,
│   │                                ColorComponent, OrbitComponent, CharacterComponent,
│   │                                LODComponent, ProjectileComponent)
│   ├── Scene.h/.cpp                (EnTT registry wrapper, update loop)
│   └── (AbilityComponents.h in ability/ also adds components to same registry)
├── terrain/
│   ├── IsometricCamera.h/.cpp
│   ├── TerrainSystem.h/.cpp        (CDLOD, heightmap, GetHeightAt())
│   ├── TerrainTextures.h/.cpp
│   └── TerrainVertex.h
├── window/
│   └── Window.h/.cpp
└── main.cpp
```

Additional directories whose headers are included by `Renderer.h` but stored outside the canonical tree (possibly `src/` subdirectories not yet visible, or generated):
- `combat/AutoAttackSystem.h` — target acquisition, attack cycle, damage application
- `hud/HUD.h`, `hud/MinionHealthBars.h` — screen-space HUD overlays
- `minion/MinionSystem.h`, `minion/MinionComponents.h`, `minion/MinionConfig.h`
- `structure/StructureSystem.h`, `structure/StructureComponents.h`
- `jungle/JungleSystem.h`, `jungle/JungleComponents.h`

### Existing External Dependencies (`extern/`)

| Library | Path | Status |
|---------|------|--------|
| EnTT | `extern/entt/` | **ALREADY INTEGRATED** |
| ImGui | `extern/imgui/` | **ALREADY INTEGRATED** |
| nlohmann/json | `extern/nlohmann/` | **ALREADY INTEGRATED** |
| stb | `extern/stb/` | **ALREADY INTEGRATED** |
| TinyGLTF | `extern/tinygltf/` | **ALREADY INTEGRATED** |
| TinyOBJ | `extern/tinyobj/` | **ALREADY INTEGRATED** |
| VMA | `extern/vma/` | **ALREADY INTEGRATED** — used in Buffer.cpp, ParticleSystem.cpp, etc. |

### Existing Shaders (`shaders/`)

```
shaders/
├── bloom_blur.frag
├── bloom_extract.frag
├── debug.frag / debug.vert
├── deferred.frag / deferred.vert
├── fog.frag / fog.vert          *** FoW shaders ALREADY EXIST — do NOT recreate ***
├── gbuffer.frag / gbuffer.vert
├── grid.frag / grid.vert
├── particle.frag / particle.vert
├── particle_sim.comp            (existing GPU particle simulation)
├── postprocess.frag / postprocess.vert
├── shadow.frag / shadow.vert
├── sky.frag
├── ssao.frag / ssao_blur.frag
├── terrain.frag / terrain.vert
├── triangle.frag / triangle.vert
└── water.frag / water.vert
```

### Existing Tests (`tests/`)

```
tests/
├── test_ability.cpp
├── test_fog.cpp
├── test_maploader.cpp
├── test_mirror.cpp
├── test_nav.cpp
└── test_terrain.cpp
```

### Existing Documentation

- `ABILITIES.md` — ability system design
- `CAMERA_PHYSICS.md` — camera physics notes
- `CHARACTER_TEST.md` — character test setup
- `MAP_PLAN.md` — map design spec
- `OPTIMIZATION_PLAN.md` — performance optimization history
- `PLAN.md` — original renderer build plan
- `agent.md` — agent operating instructions

### Key Constants and Limits (for reference in later phases)

From `Renderer.h`:
- `FIXED_DT = 1.0f / 30.0f` — 30 Hz fixed simulation tick
- `MAX_FRAMES_IN_FLIGHT = 2` — double-buffered flight
- `INITIAL_INSTANCE_CAPACITY = 1024`
- `m_workerCount` — per-thread command pool count (multi-threaded recording already exists)

From `Descriptors.h`:
- `MAX_SKINNED_CHARS = 32` (was bumped to 128 per OPTIMIZATION_PLAN.md)

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase -0.5: Renderer Decomposition

> **MANDATORY PREREQUISITE.** `src/renderer/Renderer.cpp` is **170,473 bytes** (3,758 lines). It contains the entire game loop, scene construction, simulation stepping, minion management, bone slot pooling, and Vulkan command recording. No agent can reliably modify it without this decomposition.

### What to Extract

**Target:** Extract the fixed-timestep simulation loop into `src/core/SimulationLoop.h/.cpp`.

The simulation loop in `Renderer.cpp` (`recordCommandBuffer`, lines ~389–575) follows this exact order:

```
Simulation tick (FIXED_DT = 1/30 s):
  1. m_input->update(FIXED_DT)
  2. m_scene.update(FIXED_DT, m_currentFrame)        — character movement + animation
  3. m_projectileSystem.update(m_scene, FIXED_DT)
  4. [if mobaMode && !customMap]
     a. m_minionSystem.update(registry, FIXED_DT, m_gameTime, heightFn)
     b. m_structureSystem.update(registry, FIXED_DT, m_gameTime)
     c. m_jungleSystem.update(registry, FIXED_DT, m_gameTime, heightFn)
     d. m_autoAttackSystem.update(registry, m_minionSystem, FIXED_DT)
     e. Process structure death events (inhibitor → super minions, nexus → game over)
     f. Process jungle death events
  5. [if mobaMode && customMap] m_minionSystem + m_autoAttackSystem only
  6. Bone slot cleanup for destroyed minion entities
  7. Assign render components to newly spawned minion entities
  8. m_particles->update(FIXED_DT)
```

### Extraction Steps

#### Step 1: Create `src/core/SimulationLoop.h`

```cpp
#pragma once
#include "scene/Scene.h"
#include "ability/ProjectileSystem.h"
#include "minion/MinionSystem.h"
#include "structure/StructureSystem.h"
#include "jungle/JungleSystem.h"
#include "combat/AutoAttackSystem.h"
#include "renderer/ParticleSystem.h"
#include <functional>

namespace glory {

using HeightQueryFn = std::function<float(float, float)>;

struct SimulationContext {
    Scene*             scene            = nullptr;
    InputManager*      input            = nullptr;
    ProjectileSystem*  projectileSystem = nullptr;
    MinionSystem*      minionSystem     = nullptr;
    StructureSystem*   structureSystem  = nullptr;
    JungleSystem*      jungleSystem     = nullptr;
    AutoAttackSystem*  autoAttackSystem = nullptr;
    ParticleSystem*    particles        = nullptr;
    HeightQueryFn      heightFn;
    float*             gameTime         = nullptr;
    bool               mobaMode         = true;
    bool               customMap        = false;
};

class SimulationLoop {
public:
    static constexpr float FIXED_DT  = 1.0f / 30.0f;
    static constexpr float MAX_DELTA = 0.25f;
    static constexpr int   MAX_STEPS = 8;

    void tick(SimulationContext& ctx, float deltaTime);

    float getAccumulator() const { return m_accumulator; }
    float getAlpha()       const { return m_alpha; }  // interpolation factor [0,1]

private:
    float m_accumulator = 0.0f;
    float m_alpha       = 0.0f;
};

} // namespace glory
```

#### Step 2: Create `src/core/SimulationLoop.cpp`

Lift the `while (m_simAccumulator >= FIXED_DT)` block verbatim from `Renderer.cpp`, replacing member-variable references with `ctx.*` accessors. Keep the bone slot cleanup logic in `Renderer.cpp` for now (it touches renderer-private state); `SimulationLoop` calls a callback for post-tick renderer bookkeeping.

#### Step 3: Modify `Renderer.cpp`

Replace the inlined simulation loop with:
```cpp
SimulationContext simCtx;
simCtx.scene            = &m_scene;
simCtx.input            = m_input.get();
simCtx.projectileSystem = &m_projectileSystem;
simCtx.minionSystem     = &m_minionSystem;
simCtx.structureSystem  = &m_structureSystem;
simCtx.jungleSystem     = &m_jungleSystem;
simCtx.autoAttackSystem = &m_autoAttackSystem;
simCtx.particles        = m_particles.get();
simCtx.heightFn         = heightFn;
simCtx.gameTime         = &m_gameTime;
simCtx.mobaMode         = m_mobaMode;
simCtx.customMap        = m_customMap;

m_simLoop.tick(simCtx, deltaTime);
```

Add `SimulationLoop m_simLoop;` as a private member in `Renderer.h`.

#### Step 4: CMake

```cmake
# CMakeLists.txt — add to glory_SOURCES:
src/core/SimulationLoop.cpp
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 0.1: Fixed-Point Deterministic Math

> **Goal:** Replace `float` simulation math with deterministic fixed-point arithmetic so replays and network clients always produce identical game states regardless of CPU/compiler.

### Platform-Portable Implementation

```cpp
// src/math/FixedPoint.h
#pragma once
#include <cstdint>
#include <cmath>
#if defined(_MSC_VER)
  #include <intrin.h>   // _umul128, _div128 — must be at file scope on MSVC
#endif

namespace glory {

class Fixed64 {
public:
    static constexpr int    FRAC_BITS = 16;
    static constexpr int64_t ONE      = int64_t(1) << FRAC_BITS;

    int64_t raw = 0;

    constexpr Fixed64() = default;
    explicit constexpr Fixed64(int64_t r) : raw(r) {}
    static constexpr Fixed64 fromInt(int32_t v) { return Fixed64(int64_t(v) << FRAC_BITS); }
    static constexpr Fixed64 fromFloat(float f) { return Fixed64(int64_t(f * ONE)); }

    float toFloat() const { return float(raw) / float(ONE); }

    Fixed64 operator+(Fixed64 o) const { return Fixed64(raw + o.raw); }
    Fixed64 operator-(Fixed64 o) const { return Fixed64(raw - o.raw); }
    Fixed64 operator-()          const { return Fixed64(-raw); }

    Fixed64 operator*(Fixed64 o) const {
#if defined(__GNUC__) || defined(__clang__)
        __int128 temp = static_cast<__int128>(raw) * o.raw;
        return Fixed64(static_cast<int64_t>(temp >> FRAC_BITS));
#elif defined(_MSC_VER)
        // _umul128 operates on unsigned 64-bit values; reconstruct the signed
        // Q48.16 result by shifting the 128-bit product right by FRAC_BITS.
        // hi:lo = |raw| * |o.raw|; apply sign correction afterwards.
        bool negative = (raw < 0) != (o.raw < 0);
        uint64_t absA = static_cast<uint64_t>(raw  < 0 ? -raw  : raw);
        uint64_t absB = static_cast<uint64_t>(o.raw < 0 ? -o.raw : o.raw);
        uint64_t hi;
        uint64_t lo = _umul128(absA, absB, &hi);
        // Shift the 128-bit product right by FRAC_BITS
        uint64_t result = (hi << (64 - FRAC_BITS)) | (lo >> FRAC_BITS);
        return Fixed64(negative ? -static_cast<int64_t>(result)
                                :  static_cast<int64_t>(result));
#else
        // Portable fallback (may overflow for values outside ~±32767)
        return Fixed64((raw * o.raw) >> FRAC_BITS);
#endif
    }

    Fixed64 operator/(Fixed64 o) const {
#if defined(__GNUC__) || defined(__clang__)
        __int128 n = static_cast<__int128>(raw) << FRAC_BITS;
        return Fixed64(static_cast<int64_t>(n / o.raw));
#elif defined(_MSC_VER)
        // Shift dividend left by FRAC_BITS into hi:lo, then divide.
        // _div128(hi, lo, divisor, remainder) — all signed 64-bit.
        int64_t hi  = raw >> (64 - FRAC_BITS);
        uint64_t lo = static_cast<uint64_t>(raw) << FRAC_BITS;
        int64_t rem;
        int64_t q = _div128(hi, static_cast<int64_t>(lo), o.raw, &rem);
        return Fixed64(q);
#else
        return Fixed64((raw << FRAC_BITS) / o.raw);
#endif
    }

    bool operator==(Fixed64 o) const { return raw == o.raw; }
    bool operator!=(Fixed64 o) const { return raw != o.raw; }
    bool operator< (Fixed64 o) const { return raw <  o.raw; }
    bool operator<=(Fixed64 o) const { return raw <= o.raw; }
    bool operator> (Fixed64 o) const { return raw >  o.raw; }
    bool operator>=(Fixed64 o) const { return raw >= o.raw; }

    Fixed64& operator+=(Fixed64 o) { raw += o.raw; return *this; }
    Fixed64& operator-=(Fixed64 o) { raw -= o.raw; return *this; }
    Fixed64& operator*=(Fixed64 o) { *this = *this * o; return *this; }
    Fixed64& operator/=(Fixed64 o) { *this = *this / o; return *this; }
};

struct FixedVec3 {
    Fixed64 x, y, z;
    FixedVec3 operator+(const FixedVec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    FixedVec3 operator-(const FixedVec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    FixedVec3 operator*(Fixed64 s)          const { return {x*s, y*s, z*s}; }
};

// Toggle between float (fast dev) and Fixed64 (deterministic prod) via CMake
#ifdef GLORY_DETERMINISTIC
    using SimFloat = Fixed64;
    using SimVec3  = FixedVec3;
#else
    using SimFloat = float;
    using SimVec3  = glm::vec3;
#endif

} // namespace glory
```

### CMake Integration

```cmake
# CMakeLists.txt
option(GLORY_DETERMINISTIC "Use fixed-point math for deterministic simulation" OFF)
if(GLORY_DETERMINISTIC)
    target_compile_definitions(glory PRIVATE GLORY_DETERMINISTIC)
    message(STATUS "Glory: deterministic fixed-point simulation ENABLED")
endif()

# Add to glory_SOURCES:
src/math/FixedPoint.cpp
src/math/DetRNG.cpp
src/math/MurmurHash3.cpp
```

### Sub-Phase Breakdown

Each sub-phase ends with a full build + test gate.

#### Phase 0.1a — Create FixedPoint library (standalone, no integration)

Files to create:
- `src/math/FixedPoint.h` — `Fixed64`, `FixedVec3`, `SimFloat`/`SimVec3` typedefs
- `src/math/FixedPoint.cpp` — out-of-line sqrt, atan2 approximations
- `tests/test_fixedpoint.cpp` — unit tests for arithmetic, overflow guards, determinism
- `CMake:` add `src/math/FixedPoint.cpp` to `glory_SOURCES`; add `tests/test_fixedpoint.cpp` to test target

```cpp
// tests/test_fixedpoint.cpp (minimal example)
#include "math/FixedPoint.h"
#include <cassert>

int main() {
    using namespace glory;
    Fixed64 a = Fixed64::fromFloat(1.5f);
    Fixed64 b = Fixed64::fromFloat(2.0f);
    assert((a + b).toFloat() == 3.5f);
    assert((a * b).toFloat() == 3.0f);
    assert((b / a).toFloat() > 1.33f && (b / a).toFloat() < 1.34f);

    // Determinism: identical fixed-point inputs must always produce the same raw bits.
    // Do NOT mix float arithmetic on the right-hand side — float ops are non-deterministic
    // across compilers/platforms and fromFloat() rounding introduces unpredictable skew.
    Fixed64 x = Fixed64::fromInt(3);   // exact: 3.0
    Fixed64 y = Fixed64::fromInt(7);   // exact: 7.0
    Fixed64 r1 = x * y;
    Fixed64 r2 = Fixed64::fromInt(3) * Fixed64::fromInt(7);
    assert(r1.raw == r2.raw);          // same inputs → bitwise identical output
    assert(r1.toFloat() == 21.0f);     // sanity check the value
    return 0;
}
```

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1b — Create Sim* components (add to `Components.h` alongside existing float components)

The existing `TransformComponent` (glm::vec3) remains as the **render-only interpolated view**. Add new simulation-authoritative components:

```cpp
// Append to src/scene/Components.h (inside namespace glory)
#ifdef GLORY_DETERMINISTIC
#include "math/FixedPoint.h"
#endif

// NEW simulation-authoritative components
struct SimPosition { SimVec3 value{}; };
struct SimVelocity { SimVec3 value{}; };
struct SimRotation { SimFloat yaw{}; };   // Y-axis rotation (MOBA top-down)
struct VisionComponent {
    SimFloat sightRadius{};
    uint8_t  teamID = 0;
};
struct FlowFieldAgent {
    uint32_t flowFieldID       = 0;
    SimFloat separationRadius{};
};
```

- `TransformComponent` (glm::vec3) = render interpolation only — **do not remove**
- `SimPosition` / `SimVelocity` = simulation authority
- A `SimToRenderSyncSystem` (Phase 0.1c) writes `SimPosition` → `TransformComponent` each frame

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1c — Create `SimToRenderSyncSystem`

```cpp
// src/core/SimToRenderSyncSystem.h
#pragma once
#include <entt.hpp>
#include "scene/Components.h"

namespace glory {

class SimToRenderSyncSystem {
public:
    // Called once per render frame (after simulation ticks, before rendering)
    // alpha = interpolation factor within the current fixed tick (0..1)
    static void sync(entt::registry& reg, float alpha);
};

} // namespace glory
```

```cpp
// src/core/SimToRenderSyncSystem.cpp
#include "core/SimToRenderSyncSystem.h"

namespace glory {

void SimToRenderSyncSystem::sync(entt::registry& reg, float alpha) {
    (void)alpha; // future: interpolate between prev/curr SimPosition
    auto view = reg.view<SimPosition, TransformComponent>();
    for (auto e : view) {
        auto& sim = view.get<SimPosition>(e);
        auto& xf  = view.get<TransformComponent>(e);
#ifdef GLORY_DETERMINISTIC
        xf.position = glm::vec3(sim.value.x.toFloat(),
                                sim.value.y.toFloat(),
                                sim.value.z.toFloat());
#else
        xf.position = sim.value;
#endif
    }
}

} // namespace glory
```

`CMake:` add `src/core/SimToRenderSyncSystem.cpp` to `glory_SOURCES`

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1d — Migrate `ProjectileSystem` to use `SimFloat`

`src/ability/ProjectileSystem.h/.cpp` is the most isolated system (no AI dependencies). Replace `float` velocity math with `SimFloat`. `ProjectileComponent::velocity` stays `glm::vec3` for the render layer; add `SimVelocity` component alongside for simulation.

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1e — Migrate `MinionSystem`

Replace internal position / velocity floats with `SimFloat`. Minion lane-following via `LaneFollower` (spline evaluation) must be ported to `SimFloat` arithmetic.

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1f — Migrate `AbilitySystem` + `EffectSystem` + `StatusEffectSystem` + `CooldownSystem`

All four systems interact with `CombatStatsComponent` in `AbilityComponents.h`. Migrate damage/heal/cooldown values to `SimFloat`.

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1g — Migrate `StructureSystem` + `JungleSystem`

Tower targeting range, inhibitor health, jungle respawn timers.

**Build Gate:** `cmake --build build/ --parallel && cd build/ && ctest --output-on-failure`

#### Phase 0.1h — Migrate `AutoAttackSystem`

Attack range check, damage application. At end of this sub-phase, all simulation math uses `SimFloat` and the build passes with `-DGLORY_DETERMINISTIC=ON`.

**Build Gate (both modes):**
```sh
cmake -DGLORY_DETERMINISTIC=OFF -B build_float  && cmake --build build_float  --parallel
cmake -DGLORY_DETERMINISTIC=ON  -B build_det    && cmake --build build_det    --parallel
cd build_float && ctest --output-on-failure
cd build_det   && ctest --output-on-failure
```

---

## Phase 0.2: Simulation Tick Decoupling

> **Goal:** Formalise the simulation execution order as a typed ECS system pipeline, replacing the ad-hoc function calls currently scattered through `Renderer.cpp` (extracted in Phase -0.5).

### Deterministic Random Number Generator

```cpp
// src/math/DetRNG.h
#pragma once
#include <cstdint>

namespace glory {

// xoshiro256** — passes PractRand, period 2^256-1, no platform-dependent FP
class DetRNG {
public:
    explicit DetRNG(uint64_t seed);
    uint64_t next();
    // Returns uniform integer in [0, n)
    uint64_t nextInt(uint64_t n);
    // Returns fixed-point value in [0, 1)
    // SimFloat nextFixed();  // enable after Phase 0.1
private:
    uint64_t s[4];
};

} // namespace glory
```

`CMake:` add `src/math/DetRNG.cpp` to `glory_SOURCES`

### Correct System Execution Order

The following order is derived from the actual `Renderer.cpp` simulation loop discovered in Phase -1, extended with missing systems identified by the codebase audit:

```
Tick N (FIXED_DT = 1/30 s):
  1.  InputIngestionSystem      — consume player commands queued since last tick
  2.  SceneUpdateSystem         — character movement, facing, animation state transitions
                                   (wraps existing m_scene.update())
  3.  AbilitySystem             — process ability activations, gate on cooldowns/resources
  4.  ProjectileSystem          — advance projectile positions, check collision spheres
  5.  MinionSystem              — AI state transitions, lane following, aggro, targeting
  6.  StructureSystem           — tower attack cycle, inhibitor/nexus health, death events
  7.  JungleSystem              — camp spawning, monster AI, respawn timers, death events
  8.  AutoAttackSystem          — target acquisition, attack swing timing, damage application
  9.  StatusEffectSystem        — DoT/HoT ticks, buff/debuff duration countdown
  10. CooldownSystem            — decrement ability cooldown timers
  11. EffectSystem              — flush pending effect applications (queued by ability/autoatk)
  12. MovementSystem            — apply SimVelocity to SimPosition
  13. CollisionResolutionSystem — resolve entity overlaps via spatial hash grid
  14. DeathProcessingSystem     — remove dead entities, trigger gold/exp rewards,
                                   notify MinionSystem/StructureSystem
  15. StateChecksumSystem       — compute MurmurHash3 over all Sim* components
                                   → store in StateChecksum for Phase 0.3 sync
```

### `MurmurHash3` Checksum

```cpp
// src/math/MurmurHash3.h
#pragma once
#include <cstdint>
#include <cstddef>

namespace glory {
void MurmurHash3_x64_128(const void* key, int len, uint32_t seed, void* out);
uint64_t hashSimState(const entt::registry& reg); // iterates all Sim* components
} // namespace glory
```

`CMake:` add `src/math/MurmurHash3.cpp` to `glory_SOURCES`

### `StateChecksumSystem`

```cpp
// src/core/StateChecksumSystem.h
#pragma once
#include <cstdint>
#include <entt.hpp>

namespace glory {

struct StateChecksum {
    uint64_t hash    = 0;
    uint32_t tickN   = 0;
};

class StateChecksumSystem {
public:
    StateChecksum compute(const entt::registry& reg, uint32_t tick);
};

} // namespace glory
```

### New Test File

```
CMake: Add tests/test_simulation_loop.cpp to test target
```

```cpp
// tests/test_simulation_loop.cpp (minimal)
#include "core/SimulationLoop.h"
#include "core/StateChecksumSystem.h"
#include <cassert>

int main() {
    // Two fresh registries ticked identically must produce identical checksums
    entt::registry reg1, reg2;
    // ... populate identically ...
    StateChecksumSystem scs;
    auto h1 = scs.compute(reg1, 0);
    auto h2 = scs.compute(reg2, 0);
    assert(h1.hash == h2.hash);
    return 0;
}
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 0.3: State Snapshots and Checksums

> **Goal:** Capture a full serialisable simulation state for rollback-netcode and replay.

### `StateSnapshot`

```cpp
// src/core/StateSnapshot.h
#pragma once
#include <cstdint>
#include <vector>
#include <entt.hpp>

namespace glory {

// Binary blob of all Sim* component data for one tick
struct StateSnapshot {
    uint32_t              tick     = 0;
    uint64_t              checksum = 0;
    std::vector<uint8_t>  data;    // zstd-compressed after Phase 1.1

    void capture(const entt::registry& reg, uint32_t tickN);
    void restore(entt::registry& reg) const;
    bool verify(const entt::registry& reg) const; // recompute hash, compare
};

// Ring buffer of recent snapshots for rollback window (default: 8 ticks)
class SnapshotBuffer {
public:
    static constexpr int WINDOW = 8;
    void push(StateSnapshot snap);
    const StateSnapshot* get(uint32_t tick) const;
private:
    StateSnapshot m_buf[WINDOW];
    uint32_t      m_head = 0;
};

} // namespace glory
```

`CMake:` add `src/core/StateSnapshot.cpp` to `glory_SOURCES`

```
CMake: Add tests/test_snapshot.cpp to test target
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 1.1: Networking — Lock-Step with Rollback

> **Prerequisite:** Phase 0.3 must be complete (snapshots + checksums).

### Transport Layer (`ENet`)

```cmake
# CMakeLists.txt
add_subdirectory(extern/enet)          # reliable UDP
target_link_libraries(glory PRIVATE enet)
```

Check advisory database before integrating ENet — see Appendix C.

### Delta Compression (`zstd`)

```cmake
# CMakeLists.txt
find_package(zstd REQUIRED)
target_link_libraries(glory PRIVATE zstd::zstd)
```

### File Structure

```
src/net/
├── NetTypes.h          (InputFrame, TickConfirmation, DesyncReport)
├── GameServer.h/.cpp   (authoritative server — apply inputs, broadcast state)
├── GameClient.h/.cpp   (predict, rollback on mismatch, interpolate)
└── Transport.h/.cpp    (ENet wrapper — connect, send, poll)
```

### `InputFrame`

```cpp
// src/net/NetTypes.h
#pragma once
#include <cstdint>
#include "math/FixedPoint.h"

namespace glory {

struct InputFrame {
    uint32_t  tick;
    uint16_t  playerID;
    uint8_t   buttons;           // bitmask: move | attack | ability1..6
    uint8_t   abilityIndex;      // which ability (0..5)
    SimFloat  targetX, targetZ;  // world-space click position (fixed-point)
    uint64_t  checksum;          // StateChecksum at this tick (for desync detection)
};

struct TickConfirmation {
    uint32_t tick;
    uint64_t authoritativeChecksum;
};

struct DesyncReport {
    uint32_t tick;
    uint16_t playerID;
    uint64_t clientChecksum;
    uint64_t serverChecksum;
};

} // namespace glory
```

### Lock-Step Protocol

```
Client:
  1. Each tick: read local input, pack into InputFrame, send to server
  2. Predict simulation forward (apply own input immediately)
  3. On TickConfirmation: compare checksum; if mismatch → rollback to snapshot, re-simulate

Server:
  1. Wait for InputFrames from all connected players (max 16 ms)
  2. Missing frames → use last known input (input extrapolation)
  3. Step simulation, compute checksum, broadcast TickConfirmation
  4. On DesyncReport: broadcast authoritative snapshot to all clients
```

```
CMake: Add src/net/GameServer.cpp, GameClient.cpp, Transport.cpp to glory_SOURCES
CMake: Add tests/test_networking.cpp to test target
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 2.1: Flow Fields and Steering

> **Critical:** The existing `src/nav/` directory implements **lane-based pathfinding** for minions via `LaneFollower.h` + `SplineUtil.h`. These files must **NOT** be removed or replaced. Flow fields are added **alongside** the lane system for hero movement and jungle monster roaming.

### What to Preserve

```
src/nav/
├── DebugRenderer.h/.cpp    EXISTING — preserve
├── LaneFollower.h          EXISTING — preserve (lane minions follow splines)
├── SpawnSystem.h           EXISTING — preserve
└── SplineUtil.h            EXISTING — preserve (catmull-rom spline evaluation)
```

**Do NOT create `src/navigation/` — all nav code lives in `src/nav/`.**

### What to Add

```
src/nav/
├── FlowField.h/.cpp              NEW
├── HierarchicalFlowField.h/.cpp  NEW
└── SteeringSystem.h/.cpp         NEW
```

`CMake:` add `src/nav/FlowField.cpp`, `src/nav/HierarchicalFlowField.cpp`, `src/nav/SteeringSystem.cpp` to `glory_SOURCES`

### Flow Field Architecture

```cpp
// src/nav/FlowField.h
#pragma once
#include "math/FixedPoint.h"
#include <cstdint>
#include <vector>

namespace glory {

struct FlowCell {
    int8_t  dx, dz;       // best direction to goal, [-1, 0, +1]
    uint16_t cost;        // Dijkstra cost from goal (65535 = impassable)
};

class FlowField {
public:
    static constexpr int CELL_SIZE = 2; // world units per cell

    void build(uint32_t gridW, uint32_t gridH,
               const std::vector<uint8_t>& costMap, // 0=passable, 255=wall
               uint32_t goalX, uint32_t goalZ);

    glm::vec2 getDirection(float worldX, float worldZ) const;
    bool      isBuilt() const { return m_built; }

    uint32_t id = 0; // matches FlowFieldAgent::flowFieldID

private:
    uint32_t              m_w = 0, m_h = 0;
    std::vector<FlowCell> m_cells;
    bool                  m_built = false;
};

} // namespace glory
```

```cpp
// src/nav/HierarchicalFlowField.h
#pragma once
#include "nav/FlowField.h"
#include <vector>
#include <memory>

namespace glory {

// Two-level hierarchy: coarse 8×8 cluster grid, fine per-cluster flow fields
// Reduces build time from O(N²) to O(N) for large maps (>200×200 cells)
class HierarchicalFlowField {
public:
    static constexpr int CLUSTER_SIZE = 8; // cells per cluster side

    void build(uint32_t gridW, uint32_t gridH,
               const std::vector<uint8_t>& costMap,
               float goalWorldX, float goalWorldZ);

    glm::vec2 getDirection(float worldX, float worldZ) const;

private:
    uint32_t m_gridW = 0, m_gridH = 0;
    FlowField m_coarseField;
    std::vector<std::unique_ptr<FlowField>> m_fineFields;
};

} // namespace glory
```

### SteeringSystem

```cpp
// src/nav/SteeringSystem.h
#pragma once
#include <entt.hpp>
#include "nav/FlowField.h"
#include "scene/Components.h"

namespace glory {

class SteeringSystem {
public:
    // Applied to: hero group movement, jungle monsters, RTS-style group commands.
    // NOT applied to lane minions (those use LaneFollower).
    void update(entt::registry& reg,
                const std::vector<FlowField*>& activeFields,
                SimFloat dt);

private:
    static SimFloat separation(entt::registry& reg, entt::entity self,
                               SimFloat radius);
};

} // namespace glory
```

### Application Scope

| Entity Type | Pathfinding | Rationale |
|---|---|---|
| Lane minion (melee/caster/siege) | `LaneFollower` | Predefined spline → deterministic, cheap |
| Super minion | `LaneFollower` | Same lane splines |
| Hero (player-controlled) | `FlowField` / `HierarchicalFlowField` | Group commands, dynamic obstacles |
| Jungle monster (roam) | `FlowField` | Small local flow fields |
| Jungler (player hero) | `FlowField` | Same hero system |

### Optional GPU Flow Field Integration

```glsl
// shaders/flow_field_integration.comp  (NEW — optional, for large RTS maps)
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, rg8_snorm) uniform image2D flowField;
layout(set = 0, binding = 1, r16ui)     uniform uimage2D costMap;
layout(push_constant) uniform PC {
    uvec2 goalCell;
    uvec2 gridSize;
} pc;
// Dijkstra wavefront integration — produces flow directions in one compute pass
```

```
CMake: Add tests/test_flowfield.cpp to test target
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 3: Fog of War (GPU Compute Extension)

> **Critical:** `src/fog/FogSystem.h/.cpp` **already exists**. `shaders/fog.frag` and `shaders/fog.vert` **already exist**. `tests/test_fog.cpp` tests the existing implementation. Do NOT recreate FoW from scratch.

### Phase 3.1 — Audit Existing `FogSystem`

Read `src/fog/FogSystem.h` and `shaders/fog.frag`/`fog.vert` in full. Document:
- How visibility is currently computed (CPU vs GPU)
- What data is uploaded to the GPU (texture dimensions, format)
- How `fog.frag` samples the fog texture
- What `test_fog.cpp` validates

Only proceed to Phase 3.2 after the audit is documented in a code comment at the top of `src/fog/FogSystem.cpp`.

### Phase 3.2 — Extend with GPU Compute Visibility Pipeline

Add `VisionUnit` SSBO upload + GPU compute dispatch to `FogSystem`.

```cpp
// Extend src/fog/FogSystem.h (MODIFY EXISTING)
struct VisionUnit {
    float  worldX, worldZ;   // SimPosition projected to float
    float  sightRadius;      // SimFloat.toFloat()
    uint32_t teamID : 8;
    uint32_t pad    : 24;
};

// Add to FogSystem class:
void uploadVisionUnits(VkCommandBuffer cmd, const std::vector<VisionUnit>& units);
void dispatchVisibility(VkCommandBuffer cmd, uint32_t frameIdx);
```

New shader:
```glsl
// shaders/fow_visibility.comp  (NEW)
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

struct VisionUnit { vec2 pos; float radius; uint teamID; };

layout(set = 0, binding = 0) buffer VisionSSBO { VisionUnit units[]; } visUnits;
layout(set = 0, binding = 1) uniform UBO { uint unitCount; uvec2 mapSize; float cellSize; } ubo;
layout(set = 0, binding = 2, r8) uniform image2D fogMap;      // team 0
layout(set = 0, binding = 3, r8) uniform image2D fogMapTeam1; // team 1

void main() {
    ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(cell, ivec2(ubo.mapSize)))) return;
    vec2 cellWorld = vec2(cell) * ubo.cellSize;
    float vis = 0.0;
    for (uint i = 0; i < ubo.unitCount; ++i) {
        if (visUnits.units[i].teamID != 0) continue;
        float d = length(cellWorld - visUnits.units[i].pos);
        if (d < visUnits.units[i].radius) { vis = 1.0; break; }
    }
    imageStore(fogMap, cell, vec4(vis));
}
```

`CMake:` add `shaders/fow_visibility.comp` to shader compilation target

### Phase 3.3 — VisionComponent → VisionUnit Packing

In `SimToRenderSyncSystem` (Phase 0.1c), after sync: iterate entities with `SimPosition` + `VisionComponent`, pack into `std::vector<VisionUnit>`, pass to `FogSystem::uploadVisionUnits()`.

### Phase 3.4 — Shroud Tracking (Persistent `shroudMap`)

Cells ever visible are marked in a separate persistent `shroudMap` (VK_FORMAT_R8_UNORM image, never cleared). The compute shader writes 1.0 to `shroudMap` wherever `fogMap` is 1.0. In `deferred.frag`:
```glsl
float fog    = texture(fogSampler, uv).r;     // current frame visibility
float shroud = texture(shroudSampler, uv).r;  // ever seen
// fog == 1: fully visible; shroud == 1 && fog == 0: darkened; else: black
```

### Phase 3.5 — Gaussian Blur Pass

```glsl
// shaders/fow_blur.comp  (NEW)
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, r8) uniform image2D fogIn;
layout(set = 0, binding = 1, r8) uniform image2D fogOut;
// 5-tap Gaussian kernel: [1,4,6,4,1]/16 horizontal + vertical
```

`CMake:` add `shaders/fow_blur.comp` to shader compilation target

### Phase 3.6 — Async CPU Readback

```cpp
// src/fog/FogReadback.h  (NEW)
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace glory {

// 2-frame latency VkFence-gated CPU readback of fogMap
// Used by: MinionSystem (vision queries), HUD minimap
class FogReadback {
public:
    void init(VkDevice device, VkPhysicalDevice physDevice,
              uint32_t mapW, uint32_t mapH, uint32_t framesInFlight);
    void requestReadback(VkCommandBuffer cmd, VkImage fogImage, uint32_t frameIdx);
    // Returns null if readback not ready yet
    const uint8_t* getReadback(uint32_t frameIdx) const;
    void destroy();
private:
    struct Frame { VkBuffer buf; void* mapped; VkFence fence; bool pending; };
    std::vector<Frame> m_frames;
    uint32_t m_w = 0, m_h = 0;
};

} // namespace glory
```

`CMake:` add `src/fog/FogReadback.cpp` to `glory_SOURCES`

### Phase 3.7 — Integrate into `deferred.frag`

Modify `shaders/deferred.frag` (EXISTING):
```glsl
// Add uniform samplers (new bindings in FogSystem descriptor set):
layout(set = 2, binding = 5) uniform sampler2D fogSampler;
layout(set = 2, binding = 6) uniform sampler2D shroudSampler;

// In main(), after lighting computation:
vec2 fogUV = (worldPos.xz / mapSize) * 0.5 + 0.5;
float fogVis    = texture(fogSampler,   fogUV).r;
float shroudVis = texture(shroudSampler, fogUV).r;
if (fogVis < 0.01) {
    if (shroudVis < 0.01) fragColor = vec4(0.0, 0.0, 0.0, 1.0); // never seen
    else fragColor.rgb *= 0.3;  // shroud: dim
}
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 4.1: Optimize Existing Multi-Threaded Recording

> **IMPORTANT:** Multi-threaded command recording **already exists** in `Renderer.h`:
> ```cpp
> uint32_t m_workerCount = 0;
> std::vector<VkCommandPool> m_threadCommandPools;
> std::vector<std::vector<VkCommandBuffer>> m_secondaryBuffers; // [frame][worker]
> void createThreadResources();
> void destroyThreadResources();
> ```
> **Do NOT create a new multi-threaded system.** Optimize the existing one.

### Optimization Tasks

#### 4.1a — Profile Before Changing

Use the existing `GpuProfiler` (already in `Renderer.h`) to measure per-worker draw count and GPU timeline overlap. Add CPU profiler scope around each secondary buffer submission.

#### 4.1b — Load Balancing by Draw Count

Current partitioning likely divides entities by chunk index (sequential). Replace with:

```cpp
// In Renderer.cpp recordCommandBuffer():
// Sort draw groups by mesh complexity (vertex count * instance count descending)
// Assign to workers using greedy bin-packing: always assign next group to least-loaded worker
struct WorkerLoad { uint32_t workerIdx; uint64_t vertexLoad; };
std::vector<WorkerLoad> loads(m_workerCount, {0, 0});
for (auto& group : sortedDrawGroups) {
    auto& least = *std::min_element(loads.begin(), loads.end(),
        [](auto& a, auto& b){ return a.vertexLoad < b.vertexLoad; });
    workerAssignments[least.workerIdx].push_back(&group);
    least.vertexLoad += group.instanceCount * meshVertexCount(group.meshIndex);
}
```

#### 4.1c — Task-Stealing Work Queue

```cpp
// src/core/WorkStealQueue.h  (NEW — if enkiTS not used)
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>

namespace glory {

class WorkStealQueue {
public:
    using Task = std::function<void()>;
    void push(Task t);
    bool trySteal(Task& out);
    bool tryPop(Task& out);
private:
    std::deque<Task> m_queue;
    mutable std::mutex m_mutex;
};

} // namespace glory
```

Alternatively, integrate `enkiTS` (see Appendix C) as a CMake subdirectory and replace custom thread pool.

`CMake:` add `src/core/WorkStealQueue.cpp` to `glory_SOURCES`

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 4.2: Hi-Z Occlusion Culling

> **Prerequisite:** Existing `GpuCuller` (`src/renderer/GpuCuller.h/.cpp`) handles GPU frustum culling. Extend it with hierarchical Z-buffer occlusion culling.

### `HiZBuffer`

```cpp
// src/renderer/HiZBuffer.h  (NEW)
#pragma once
#include "renderer/Image.h"
#include <vulkan/vulkan.h>

namespace glory {

class HiZBuffer {
public:
    void init(Device& device, uint32_t width, uint32_t height);
    void generate(VkCommandBuffer cmd, VkImageView depthView, uint32_t frameIdx);
    VkImageView getMipView(uint32_t mip) const;
    uint32_t    getMipCount() const { return m_mipCount; }
    void        destroy();

private:
    Image    m_hizImage;
    uint32_t m_mipCount = 0;
    std::vector<VkImageView> m_mipViews;
    VkPipeline       m_genPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_genLayout   = VK_NULL_HANDLE;
};

} // namespace glory
```

New shaders:

```glsl
// shaders/hiz_generate.comp  (NEW)
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D depthSrc;
layout(set = 0, binding = 1, r32f) uniform image2D hizMip;
layout(push_constant) uniform PC { uvec2 srcSize; } pc;
void main() {
    // 2×2 max reduction — conservative (occluder if ALL 4 samples occlude)
    ivec2 dst = ivec2(gl_GlobalInvocationID.xy);
    ivec2 src = dst * 2;
    float d = max(max(texelFetch(depthSrc, src,              0).r,
                      texelFetch(depthSrc, src+ivec2(1,0),  0).r),
               max(texelFetch(depthSrc, src+ivec2(0,1),  0).r,
                   texelFetch(depthSrc, src+ivec2(1,1),  0).r));
    imageStore(hizMip, dst, vec4(d));
}
```

```glsl
// shaders/hiz_cull.comp  (NEW)
#version 450
layout(local_size_x = 64) in;

struct OccludeeCandidates { vec4 aabbMin; vec4 aabbMax; uint drawID; uint pad[3]; };
layout(set=0, binding=0) readonly buffer Candidates { OccludeeCandidates data[]; } cands;
layout(set=0, binding=1) buffer DrawArgs { uint drawCount; uvec4 cmds[]; } drawArgs;
layout(set=0, binding=2) uniform sampler2D hizPyramid;
layout(push_constant) uniform PC { mat4 viewProj; uint count; } pc;

void main() {
    // Project AABB corners into NDC, sample Hi-Z pyramid at appropriate mip level
    // Discard if projected min depth > Hi-Z sample (occluded)
}
```

`CMake:` add `src/renderer/HiZBuffer.cpp` to `glory_SOURCES`

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 4.3: Bindless Resources

> **Goal:** Eliminate per-draw descriptor updates by indexing all textures, buffers, and meshes via a single large descriptor set.

### `BindlessAllocator`

```cpp
// src/renderer/BindlessAllocator.h  (NEW)
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace glory {

class BindlessAllocator {
public:
    static constexpr uint32_t MAX_TEXTURES = 4096;
    static constexpr uint32_t MAX_BUFFERS  = 1024;

    void init(VkDevice device, uint32_t framesInFlight);
    uint32_t allocTexture(VkImageView view, VkSampler sampler);
    uint32_t allocBuffer (VkBuffer    buf,  VkDeviceSize offset, VkDeviceSize range);
    void     freeTexture (uint32_t idx);
    void     freeBuffer  (uint32_t idx);

    VkDescriptorSetLayout getLayout() const { return m_layout; }
    VkDescriptorSet       getSet()    const { return m_set; }
    void                  destroy();

private:
    VkDevice              m_device    = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout    = VK_NULL_HANDLE;
    VkDescriptorSet       m_set       = VK_NULL_HANDLE;

    std::vector<uint32_t> m_freeTexSlots;
    std::vector<uint32_t> m_freeBufSlots;
};

} // namespace glory
```

Requires `VK_EXT_descriptor_indexing` (core in Vulkan 1.2). Enable in `Device.cpp`:
```cpp
VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
indexing.descriptorBindingPartiallyBound                = VK_TRUE;
indexing.runtimeDescriptorArray                          = VK_TRUE;
indexing.descriptorBindingSampledImageUpdateAfterBind    = VK_TRUE;
indexing.descriptorBindingStorageBufferUpdateAfterBind   = VK_TRUE;
// Chain into VkDeviceCreateInfo::pNext
```

`CMake:` add `src/renderer/BindlessAllocator.cpp` to `glory_SOURCES`

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 4.4: Visibility Buffer

> **Prerequisite:** Phase 4.3 (Bindless) must be complete — visibility buffer resolve requires bindless texture indexing.

### Architecture

The visibility buffer renders `(meshletID << 7 | primitiveID)` to a `VK_FORMAT_R32_UINT` attachment in a dedicated first pass. A compute resolve pass (`visbuf_resolve.comp`) reconstructs per-pixel material parameters without rasterising G-buffer intermediates.

```glsl
// shaders/visbuf_resolve.comp  (NEW)
#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(local_size_x = 8, local_size_y = 8) in;

layout(set=0, binding=0, r32ui) uniform uimage2D visBuffer;
layout(set=0, binding=1) uniform sampler2D textures[]; // bindless array

layout(set=0, binding=2) readonly buffer Meshlets  { /* meshlet data */ } meshlets;
layout(set=0, binding=3) readonly buffer Vertices  { /* vertex data  */ } vertices;
layout(set=0, binding=4) readonly buffer Indices   { /* index data   */ } indices;

layout(set=1, binding=0, rgba16f) uniform writeonly image2D gbufAlbedo;
layout(set=1, binding=1, rgba16f) uniform writeonly image2D gbufNormal;
layout(set=1, binding=2, rgba16f) uniform writeonly image2D gbufMaterial;

layout(push_constant) uniform PC { uvec2 resolution; mat4 invViewProj; } pc;

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(coord, ivec2(pc.resolution)))) return;

    uint vis = imageLoad(visBuffer, coord).r;
    if (vis == 0xFFFFFFFF) return; // skybox

    uint meshletID   = vis >> 7u;
    uint primitiveID = vis & 0x7Fu;

    // Fetch triangle vertices from bindless buffers
    // Compute barycentrics via partial derivatives
    // Fetch albedo/normal from bindless texture array using nonuniformEXT
    // Write to G-buffer images
}
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 4.5: Pipeline Libraries

> **Goal:** Reduce shader compilation stalls at startup and during dynamic state changes.

### `PipelineLibrary`

```cpp
// src/renderer/PipelineLibrary.h  (NEW)
#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <unordered_map>

namespace glory {

// Wraps VK_EXT_graphics_pipeline_library for pre-compiled pipeline fragments
class PipelineLibrary {
public:
    void init(VkDevice device);

    // Pre-compile vertex input + pre-raster stages at startup
    void precompileVertexInput(const std::string& key,
                               const VkPipelineVertexInputStateCreateInfo& vi,
                               const VkPipelineInputAssemblyStateCreateInfo& ia);
    void precompilePreRaster  (const std::string& key,
                               VkShaderModule vertShader,
                               const VkPipelineLayout& layout,
                               VkRenderPass renderPass);

    // Link final pipeline (fast — fragment + output stages only at runtime)
    VkPipeline link(const std::string& vertKey, VkShaderModule fragShader,
                    const VkPipelineColorBlendStateCreateInfo& blend,
                    const VkPipelineDepthStencilStateCreateInfo& ds);

    void destroy();

private:
    VkDevice m_device = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkPipeline> m_fragments;
    std::unordered_map<std::string, VkPipeline> m_linked;
};

} // namespace glory
```

Requires `VK_EXT_graphics_pipeline_library`. Enable in `Device.cpp` feature chain.

`CMake:` add `src/renderer/PipelineLibrary.cpp` to `glory_SOURCES`

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 4.6: VMA Configuration Optimization

> **IMPORTANT:** VMA is **already integrated** (`extern/vma/`, used in `Buffer.cpp`, `ParticleSystem.cpp`, etc.). **Do NOT add VMA again.** Configure it for optimal performance.

### Optimization Tasks

#### 4.6a — Linear Allocator for Per-Frame Staging Buffers

Per-frame instance buffers and staging uploads use many small short-lived allocations. Use `VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT`:

```cpp
// In Buffer.cpp or a new src/renderer/StagingAllocator.h
VmaPoolCreateInfo poolInfo{};
poolInfo.memoryTypeIndex = findMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
poolInfo.flags           = VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT;
poolInfo.blockSize       = 16 * 1024 * 1024; // 16 MB per block
VmaPool stagingPool;
vmaCreatePool(allocator, &poolInfo, &stagingPool);
```

#### 4.6b — Buddy Allocator for Long-Lived Resources

GPU-only meshes, textures, and compute output buffers benefit from buddy allocation (reduces fragmentation over many alloc/free cycles):

```cpp
poolInfo.flags = VMA_POOL_CREATE_BUDDY_ALGORITHM_BIT;
```

#### 4.6c — `VK_EXT_memory_budget` Monitoring

```cpp
// In DebugOverlay.cpp (EXISTING — MODIFY: add memory panel)
VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
VkPhysicalDeviceMemoryProperties2 props2{};
props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
props2.pNext = &budget;
vkGetPhysicalDeviceMemoryProperties2(physDevice, &props2);
// Display budget.heapBudget[i] / budget.heapUsage[i] in ImGui memory panel
```

#### 4.6d — `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`

Enable if buffer device address is needed for bindless (Phase 4.3) or ray tracing:

```cpp
VmaAllocatorCreateInfo allocatorInfo{};
allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
// Requires VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress = VK_TRUE
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 5: Async Compute Pipeline

> **Prerequisite:** `QueueManager` (new) must be created first to manage graphics + compute + transfer queue submission.

### `QueueManager`

```cpp
// src/renderer/QueueManager.h  (NEW)
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace glory {

// Manages submission to graphics, async compute, and transfer queues.
// The existing Device already finds a TRANSFER-only queue; this class
// adds submission coordination and timeline semaphore signalling.
class QueueManager {
public:
    void init(VkDevice device, uint32_t graphicsFamily, uint32_t computeFamily,
              uint32_t transferFamily, uint32_t framesInFlight);

    // Submit to async compute queue; signal timelineSemaphore at value=tick
    void submitCompute(VkCommandBuffer cmd, uint64_t signalValue);
    // Submit to graphics queue; wait on compute semaphore before vertex stage
    void submitGraphics(VkCommandBuffer cmd, VkSemaphore waitSemaphore,
                        uint64_t waitValue, VkFence signalFence);
    void destroy();

private:
    VkQueue  m_computeQueue  = VK_NULL_HANDLE;
    VkQueue  m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue  m_transferQueue = VK_NULL_HANDLE;
    VkSemaphore m_computeTimeline = VK_NULL_HANDLE; // VK_KHR_timeline_semaphore
};

} // namespace glory
```

`CMake:` add `src/renderer/QueueManager.cpp` to `glory_SOURCES`

### Async Compute Workloads

Once `QueueManager` is available, move these to async compute:

1. **`ComputeSkinner`** — currently dispatched on graphics queue before scene pass. Move to async compute queue; insert pipeline barrier in graphics queue to wait on compute semaphore before `VK_PIPELINE_STAGE_VERTEX_INPUT_BIT`.

2. **`GpuCuller`** — frustum cull dispatch. Overlap with previous frame's shadow pass.

3. **FoW visibility dispatch** (Phase 3.2) — run in parallel with G-buffer pass.

4. **`TextureStreamer`** — already uses dedicated transfer queue (no change needed).

### Timeline Semaphore Sequence

```
Frame N:
  Graphics Queue:  [Shadow Pass] ─────────────────→ [G-Buffer Pass] → [Lighting] → [Post]
  Compute Queue:   [ComputeSkinner (N)] → signal(N) ↗
                   [GpuCuller (N)]      → signal(N) ↗
                   [FoW Vis (N)]        → signal(N) ↗
  Transfer Queue:  [TextureStreamer] (independent, fires callback on completion)
```

**Build Gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Phase 6: Polish — Cooked Assets, Debug Tools, Test Suite

### Phase 6.1 — Cooked Asset Pipeline

```cpp
// src/asset/CookedLoader.h  (NEW)
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace glory {

// Binary cooked format: header + zstd-compressed mesh/texture blobs
// Avoids runtime GLB/OBJ parsing cost, enables basis_universal texture compression
struct CookedAssetHeader {
    uint32_t magic   = 0x474C5259; // "GLRY"
    uint32_t version = 1;
    uint32_t meshCount;
    uint32_t textureCount;
};

class CookedLoader {
public:
    bool loadMesh   (const std::string& path, /* out */ struct MeshData&);
    bool loadTexture(const std::string& path, /* out */ std::vector<uint8_t>& pixels,
                     uint32_t& w, uint32_t& h);
};

} // namespace glory
```

`CMake:` add `src/asset/CookedLoader.cpp` to `glory_SOURCES`

### Phase 6.2 — Debug Tools

Extend `src/editor/DebugOverlay.h/.cpp` (EXISTING) with panels for:
- Flow field visualiser (overlay arrows on terrain)
- FoW texture viewer (split-screen fog/shroud)
- Simulation tick budget (per-system ms cost from Profiler)
- VMA memory budget (from Phase 4.6c)
- Network latency / desync counter (from Phase 1.1)

### Phase 6.3 — Determinism Test

```cpp
// tests/test_determinism.cpp  (NEW)
// Run 300 simulation ticks on two isolated registries with identical inputs.
// Assert StateChecksum matches every tick.
// Run with -DGLORY_DETERMINISTIC=ON only.
```

`CMake:` add `tests/test_determinism.cpp` to test target (guarded by `GLORY_DETERMINISTIC`)

**Final Build Gate:**
```sh
cmake -DGLORY_DETERMINISTIC=ON -B build_det && cmake --build build_det --parallel
cd build_det && ctest --output-on-failure
cmake -DGLORY_DETERMINISTIC=OFF -B build/ && cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## Appendix A: File Structure

All files marked `(EXISTING — MODIFY)` must not be recreated. Files marked `(NEW)` must be added to `CMakeLists.txt` `glory_SOURCES` (or test target).

```
src/
├── core/
│   ├── Application.h/.cpp           (EXISTING — minor modifications)
│   ├── SimulationLoop.h/.cpp        (NEW — extracted from Renderer.cpp, Phase -0.5)
│   ├── SimToRenderSyncSystem.h/.cpp (NEW — Phase 0.1c)
│   ├── StateSnapshot.h/.cpp         (NEW — Phase 0.3)
│   ├── StateChecksumSystem.h/.cpp   (NEW — Phase 0.2)
│   ├── WorkStealQueue.h/.cpp        (NEW — Phase 4.1, optional if enkiTS used)
│   └── Profiler.h/.cpp              (EXISTING)
├── math/
│   ├── FixedPoint.h/.cpp            (NEW — Phase 0.1a)
│   ├── DetRNG.h/.cpp                (NEW — Phase 0.2)
│   └── MurmurHash3.h/.cpp           (NEW — Phase 0.2)
├── net/
│   ├── NetTypes.h                   (NEW — Phase 1.1)
│   ├── GameServer.h/.cpp            (NEW — Phase 1.1)
│   ├── GameClient.h/.cpp            (NEW — Phase 1.1)
│   └── Transport.h/.cpp             (NEW — Phase 1.1)
├── nav/
│   ├── DebugRenderer.h/.cpp         (EXISTING — preserve)
│   ├── LaneFollower.h               (EXISTING — preserve for lane minions)
│   ├── SpawnSystem.h                (EXISTING — preserve)
│   ├── SplineUtil.h                 (EXISTING — preserve)
│   ├── FlowField.h/.cpp             (NEW — Phase 2.1)
│   ├── HierarchicalFlowField.h/.cpp (NEW — Phase 2.1)
│   └── SteeringSystem.h/.cpp        (NEW — Phase 2.1)
├── fog/
│   ├── FogSystem.h/.cpp             (EXISTING — EXTEND with GPU compute, Phase 3)
│   └── FogReadback.h/.cpp           (NEW — Phase 3.6)
├── renderer/
│   ├── GpuCuller.h/.cpp             (EXISTING — MODIFY: add Hi-Z occlusion, Phase 4.2)
│   ├── HiZBuffer.h/.cpp             (NEW — Phase 4.2)
│   ├── BindlessAllocator.h/.cpp     (NEW — Phase 4.3)
│   ├── PipelineLibrary.h/.cpp       (NEW — Phase 4.5)
│   ├── QueueManager.h/.cpp          (NEW — Phase 5)
│   ├── GBuffer.h/.cpp               (EXISTING — MODIFY: integrate FoW sampling, Phase 3.7)
│   ├── Renderer.h/.cpp              (EXISTING — MODIFY: extract sim loop, Phase -0.5)
│   └── ComputeSkinner.h/.cpp        (EXISTING — MODIFY: move to async compute, Phase 5)
├── scene/
│   └── Components.h                 (EXISTING — MODIFY: add Sim* components, Phase 0.1b)
├── ability/
│   ├── AbilitySystem.h/.cpp         (EXISTING — MODIFY: use SimFloat, Phase 0.1f)
│   ├── ProjectileSystem.h/.cpp      (EXISTING — MODIFY: use SimFloat, Phase 0.1d)
│   ├── EffectSystem.h/.cpp          (EXISTING — MODIFY: use SimFloat, Phase 0.1f)
│   ├── CooldownSystem.h/.cpp        (EXISTING — Phase 0.1f)
│   └── StatusEffectSystem.h/.cpp    (EXISTING — MODIFY: use SimFloat, Phase 0.1f)
└── asset/
    └── CookedLoader.h/.cpp          (NEW — Phase 6.1)

shaders/
├── fog.frag                         (EXISTING — MODIFY: add shroud sampling, Phase 3.7)
├── fog.vert                         (EXISTING)
├── deferred.frag                    (EXISTING — MODIFY: add FoW sampling, Phase 3.7)
├── fow_visibility.comp              (NEW — Phase 3.2)
├── fow_blur.comp                    (NEW — Phase 3.5)
├── hiz_generate.comp                (NEW — Phase 4.2)
├── hiz_cull.comp                    (NEW — Phase 4.2)
├── flow_field_integration.comp      (NEW — Phase 2.1, optional GPU flow fields)
└── visbuf_resolve.comp              (NEW — Phase 4.4)

tests/
├── test_ability.cpp                 (EXISTING)
├── test_fog.cpp                     (EXISTING)
├── test_maploader.cpp               (EXISTING)
├── test_mirror.cpp                  (EXISTING)
├── test_nav.cpp                     (EXISTING)
├── test_terrain.cpp                 (EXISTING)
├── test_fixedpoint.cpp              (NEW — Phase 0.1a)
├── test_simulation_loop.cpp         (NEW — Phase 0.2)
├── test_snapshot.cpp                (NEW — Phase 0.3)
├── test_networking.cpp              (NEW — Phase 1.1)
├── test_flowfield.cpp               (NEW — Phase 2.1)
└── test_determinism.cpp             (NEW — Phase 6.3, GLORY_DETERMINISTIC only)
```

---

## Appendix B: Dependency Graph

```
Phase -1 (Codebase Audit — mandatory, no code changes)
    │
    ▼
Phase -0.5 (Renderer Decomposition — extract SimulationLoop from 170KB Renderer.cpp)
    │
    ▼
Phase 0.1a–h (FixedPoint — incremental, one system per sub-phase, both build modes tested)
    │
    ▼
Phase 0.2 (SimulationLoop + DetRNG + MurmurHash3 — formalise system order)
    │
    ▼
Phase 0.3 (Snapshots + Checksums — prerequisite for networking)
    │
    ├──────────────────────────────────┐
    ▼                                  ▼
Phase 1.1 (Networking ENet+zstd)    Phase 2.1 (Flow Fields in src/nav/,
                                               preserves LaneFollower)
                                               │
                                    Phase 3.1–3.7 (Extend existing FogSystem,
                                               GPU compute visibility)
                                               │
                                    Phase 4.1 (Optimize existing MT recording)
                                    Phase 4.2 (Hi-Z Occlusion — extends GpuCuller)
                                    Phase 4.3 (Bindless Resources — large refactor)
                                               │
                                    Phase 4.4 (Visibility Buffer — requires 4.3)
                                    Phase 4.5 (Pipeline Libraries — independent)
                                    Phase 4.6 (VMA Config — VMA already present)
                                               │
                                    Phase 5 (Async Compute — QueueManager + all
                                             compute workloads ready)
                                               │
                                    Phase 6 (Polish: Cooked Assets, Debug Tools,
                                             Determinism Test Suite)
```

---

## Appendix C: Third-Party Dependencies

| Library | Purpose | License | Integration | Status |
|---------|---------|---------|-------------|--------|
| EnTT | ECS framework | MIT | `extern/entt/` | **EXISTING** |
| ImGui | Debug UI | MIT | `extern/imgui/` | **EXISTING** |
| nlohmann/json | JSON parsing | MIT | `extern/nlohmann/` | **EXISTING** |
| stb | Image loading | Public Domain | `extern/stb/` | **EXISTING** |
| TinyGLTF | GLB loading | MIT | `extern/tinygltf/` | **EXISTING** |
| TinyOBJ | OBJ loading | MIT | `extern/tinyobj/` | **EXISTING** |
| VMA | Vulkan Memory Allocator | MIT | `extern/vma/` | **EXISTING — configure only (Phase 4.6)** |
| ENet | Reliable UDP networking | MIT | `add_subdirectory(extern/enet)` | **TO ADD — Phase 1.1** |
| zstd | Delta/snapshot compression | BSD | `find_package(zstd)` | **TO ADD — Phase 1.1** |
| MurmurHash3 | Fast non-crypto hash | Public Domain | Embedded `src/math/MurmurHash3.cpp` | **TO ADD — Phase 0.2** |
| basis_universal | Texture compression | Apache 2.0 | Offline cook tool only — not linked | **TO ADD — Phase 6.1** |
| enkiTS | Task scheduler (optional) | MIT | `add_subdirectory(extern/enkiTS)` | **TO ADD (optional) — Phase 4.1** |

### Security Advisory Check

Before integrating any new library, run the GitHub Advisory Database check for the specific version being pinned. Required checks before integration:

| Library | Check Required |
|---------|---------------|
| ENet | Yes — check for known CVEs in chosen version |
| zstd | Yes |
| enkiTS | Yes |

---

*Last updated: automatically generated from codebase audit of `/home/runner/work/glory/glory` — commit reflects actual `src/` structure, `extern/` contents, and `shaders/` inventory.*
