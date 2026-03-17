# Vulkan VFX TDD for MOBA — Technical Design Document

**Engine:** Glory (Vulkan / C++20 / entt ECS)  
**Status:** Implemented

---

## 1. Overview

This document describes the GPU-accelerated visual-effects (VFX) pipeline and the data-driven ability system that feeds it. The design separates game logic from rendering through a lock-free SPSC event queue, so the two threads never share mutable state.

---

## 2. Architecture

```
InputSystem ──────────────────────────────────────────────────►
                                                              AbilitySystem
                                                                │  (READY→CASTING→EXECUTING→ON_COOLDOWN)
                                                                │
                                                                ▼
                                                        VFXEventQueue  (SPSC ring buffer, 256 slots)
                                                                │
                                                                ▼
VFXRenderer::processQueue()  ──────►  ParticleSystem (per effect, GPU SSBO)
VFXRenderer::update()        ──────►  CPU emission (spawn GpuParticle entries)
VFXRenderer::dispatchCompute()──────► compute shader (particle_sim.comp)
                                                barrier (compute → vertex)
VFXRenderer::render()        ──────►  particle.vert / particle.frag
```

### 2.1  Per-frame update order

| Step | Call | Stage |
|------|------|-------|
| 1 | `AbilitySystem::update(reg, dt)` | Game logic |
| 2 | `VFXRenderer::processQueue(queue)` | Render thread |
| 3 | `VFXRenderer::update(dt)` | CPU emission |
| 4 | `VFXRenderer::dispatchCompute(cmd)` | **Outside** render pass |
| 5 | `VFXRenderer::barrierComputeToGraphics(cmd)` | Pipeline barrier |
| 6 | `VFXRenderer::render(cmd, viewProj, camRight, camUp)` | **Inside** render pass |

---

## 3. GPU Particle Layout

Each particle is exactly **64 bytes** (4 × `vec4`) to satisfy SSBO alignment:

```glsl
struct Particle {
    vec4 posLife;   // xyz = world pos,  w = remaining lifetime (s)
    vec4 velAge;    // xyz = velocity,   w = age (s)
    vec4 color;     // rgba (alpha fades to 0 at death)
    vec4 params;    // x=size, y=rotation, z=atlasFrame, w=active(1)/dead(0)
};
```

---

## 4. VFX Event Queue

`VFXEventQueue` is a power-of-two SPSC ring buffer (256 slots) with cache-line-aligned atomics:

```cpp
struct VFXEvent {
    VFXEventType type;         // Spawn / Destroy / Move
    uint32_t     handle;       // output on Spawn, input on Destroy/Move
    char         effectID[48]; // maps to an EmitterDef
    glm::vec3    position;
    glm::vec3    direction;
    float        scale;
    float        lifetime;     // <0 = use EmitterDef::duration
};
```

The **AbilitySystem** (game thread) pushes to the queue.  
The **VFXRenderer** (render thread) pops it at the start of each frame.

---

## 5. Compute Shader — `particle_sim.comp`

- **Local size:** 64 threads (warp/wavefront-aligned for AMD & NVIDIA)
- **Inputs:** `ParticleSSBO` (binding 0), push constants `{dt, gravity, count}`
- **Algorithm:**
  1. Skip dead particles (early exit on `params.w < 0.5`)
  2. Decrement `posLife.w` (lifetime), increment `velAge.w` (age)
  3. If lifetime ≤ 0 → mark dead (`params.w = 0`)
  4. Apply gravity: `velAge.y -= gravity * dt`
  5. Euler integrate: `posLife.xyz += velAge.xyz * dt`
  6. Fade alpha linearly: `color.a = lifeFrac`

---

## 6. Billboard Vertex Shader — `particle.vert`

No vertex buffer is bound. The vertex shader reads the SSBO directly:

```glsl
uint pi = gl_VertexIndex / 6u;   // particle index
uint vi = gl_VertexIndex % 6u;   // vertex within the billboard quad
```

Each billboard expands in camera space using `camRight` and `camUp` vectors (push constants). Dead particles output `gl_Position.z = 2.0` (clipped silently).

**Push constant layout (96 bytes, within Vulkan's 128-byte guarantee):**

| Field | Size |
|-------|------|
| `viewProj` (mat4) | 64 bytes |
| `camRight` (vec4) | 16 bytes |
| `camUp`    (vec4) | 16 bytes |

---

## 7. Descriptor Set Layout

Both pipelines share a **single** descriptor set layout:

| Binding | Type | Stages |
|---------|------|--------|
| 0 | `STORAGE_BUFFER` | Compute (RW) + Vertex (R) |
| 1 | `COMBINED_IMAGE_SAMPLER` | Fragment |

One descriptor set per active emitter; shared pool pre-allocates 32 sets.

---

## 8. Memory Strategy

Particle SSBOs use `VMA_MEMORY_USAGE_CPU_TO_GPU` (persistent mapping):

- **CPU writes:** `ParticleSystem::spawnParticle()` writes new entries directly
- **GPU writes:** compute shader simulates all particles in-place
- **GPU reads:** vertex shader reads from SSBO (no vertex buffer)

On Apple Silicon (MoltenVK, unified memory) this incurs zero copy overhead.  
On discrete GPUs the buffer lives in BAR/ReBAR memory for direct CPU+GPU access.

---

## 9. Ability System State Machine

```
READY ──(key + validation)──► CASTING
CASTING ──(timer + no channel)──► EXECUTING
CASTING ──(timer + channel)──► CHANNELING
CHANNELING ──(elapsed)──► EXECUTING
CHANNELING ──(hard CC / cancel)──► INTERRUPTED
EXECUTING ──(effects dispatched)──► ON_COOLDOWN
INTERRUPTED ──(75% cooldown)──► ON_COOLDOWN
ON_COOLDOWN ──(timer ≤ 0)──► READY
```

### 9.1  Pre-cast validation checks

1. `level > 0` (ability learned)
2. Phase is `READY` and cooldown is 0
3. `StatusEffectsComponent::canCast()` — not stunned / silenced / suppressed
4. `ResourceComponent::current >= costPerLevel[level-1]`

### 9.2  Damage formula (standard MOBA)

```cpp
float effectiveResist = resistance * (1.0f - percentPen) - flatPen;
effectiveResist = max(0.0f, effectiveResist);
float multiplier = 100.0f / (100.0f + effectiveResist);
return rawDamage * multiplier;
```

---

## 10. Data-Driven Emitter JSON

```json
{
    "id":              "vfx_fireball_cast",
    "textureAtlas":    "textures/particles/fire.png",
    "maxParticles":    256,
    "emitRate":        80.0,
    "burstCount":      20.0,
    "looping":         false,
    "duration":        0.8,
    "lifetimeMin":     0.4,
    "lifetimeMax":     0.9,
    "initialSpeedMin": 3.0,
    "initialSpeedMax": 8.0,
    "sizeMin":         0.3,
    "sizeMax":         0.7,
    "spreadAngle":     35.0,
    "gravity":         2.0,
    "colorOverLifetime": [
        {"time": 0.0, "color": [1.0, 0.6, 0.1, 1.0]},
        {"time": 0.5, "color": [1.0, 0.2, 0.0, 0.8]},
        {"time": 1.0, "color": [0.3, 0.1, 0.0, 0.0]}
    ]
}
```

Emitter JSON files live in `ASSET_DIR/vfx/` and are hot-loaded at startup.  
Ability JSON files (`ASSET_DIR/abilities/`) reference effects via `"castVFX"`, `"projectileVFX"`, and `"impactVFX"`.

---

## 11. CC Priority Hierarchy

| Priority | CC Type | Prevents | Notes |
|----------|---------|----------|-------|
| 1 (highest) | Suppress | Everything | Cannot be cleansed |
| 2 | Stun | Move, Cast, Attack | Reduced by tenacity |
| 3 | Knockup | Move, Cast, Attack | Not reduced by tenacity |
| 4 | Root | Movement only | Can still cast/attack |
| 5 | Silence | Ability casts | — |
| 6 | Slow | Reduces speed | Multiplicative stacking |

---

## 12. File Reference

| Path | Purpose |
|------|---------|
| `src/vfx/VFXTypes.h` | `GpuParticle`, `VFXEvent`, `EmitterDef` |
| `src/vfx/VFXEventQueue.h` | Lock-free SPSC ring buffer (256 slots) |
| `src/vfx/ParticleSystem.h/.cpp` | Single active effect (GPU SSBO + CPU spawner) |
| `src/vfx/VFXRenderer.h/.cpp` | Compute + graphics pipelines, emitter pool |
| `src/ability/AbilityTypes.h` | Enums, `AbilityDefinition`, `EffectDef`, `ScalingFormula` |
| `src/ability/AbilityComponents.h` | ECS components: `AbilityBookComponent`, `StatusEffectsComponent`, `ProjectileComponent`, `StatsComponent` |
| `src/ability/AbilitySystem.h/.cpp` | State machine, damage math, VFX event emission |
| `shaders/particle_sim.comp` | GPU particle simulation (compute, local_size=64) |
| `shaders/particle.vert` | Billboard vertex shader (SSBO-driven, no vertex buffer) |
| `shaders/particle.frag` | Particle fragment shader (atlas sampling, alpha blend) |

---

## 13. Roadmap

- [ ] `ProjectileSystem` — spatial collision using nav grid
- [ ] Targeting indicators (AoE circle, skillshot direction arrow)
- [ ] Soft particles (depth-buffer fade at surface intersections)
- [ ] Additive blending variant for fire/magic effects
- [ ] GPU indirect draw with alive-list compaction (eliminate dead-particle draw calls)
- [ ] Champion kit JSON files for 3+ champions (Q/W/E/R + passive)
- [ ] Audio event bridge (same SPSC pattern as VFX queue)
