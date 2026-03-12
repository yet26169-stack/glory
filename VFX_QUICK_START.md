# Glory VFX System - Quick Start Guide

## TL;DR: Adding New VFX in 5 Minutes

### 1. Create a Particle Effect JSON
File: `assets/vfx/vfx_my_effect.json`
```json
{
  "id": "vfx_my_effect",
  "maxParticles": 128,
  "emitRate": 50.0,
  "burstCount": 10.0,
  "looping": false,
  "duration": 1.0,
  "lifetimeMin": 0.5,
  "lifetimeMax": 1.5,
  "initialSpeedMin": 2.0,
  "initialSpeedMax": 8.0,
  "sizeMin": 0.25,
  "sizeMax": 0.75,
  "spreadAngle": 30.0,
  "gravity": 3.0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 1.0, 1.0, 1.0]},
    {"time": 1.0, "color": [1.0, 1.0, 1.0, 0.0]}
  ]
}
```

### 2. Reference it in an Ability JSON
File: `assets/abilities/my_ability.json`
```json
{
  "id": "my_ability",
  "castVFX": "vfx_my_effect",
  "impactVFX": "vfx_my_effect"
}
```

### 3. That's it! 
The system loads JSONs at startup and handles the rest:
- `VFXRenderer::loadEmitterDirectory()` loads all `.json` files
- `AbilitySystem` fires events when abilities execute
- GPU compute shader simulates particles
- Billboard vertex shader renders them

---

## Understanding the Pipeline

```
Game Thread (AbilitySystem)
    ↓
    queue.push(VFXEvent)  [SPSC lock-free queue]
    ↓
Render Thread (VFXRenderer)
    ↓
    processQueue()         [Spawn/Destroy/Move]
    ↓
    update(dt)             [CPU emission]
    ↓
    dispatchCompute()      [GPU simulation]
    ↓
    barrierComputeToGraphics()
    ↓
    render()               [Draw billboards]
```

---

## Key Parameters Explained

| Parameter | Range | Effect |
|-----------|-------|--------|
| `emitRate` | 0–100 | Particles per second |
| `burstCount` | 0–1000 | Instant particles on spawn |
| `looping` | true/false | Repeat emission or one-shot |
| `duration` | 0–10 | How long emitter stays active (sec) |
| `lifetimeMin/Max` | 0–10 | Individual particle lifespan (sec) |
| `initialSpeedMin/Max` | 0–20 | Velocity magnitude (m/s) |
| `sizeMin/Max` | 0–2 | Billboard size (world units) |
| `spreadAngle` | 0–180 | Cone spread (0=straight up, 180=all directions) |
| `gravity` | 0–10 | Downward acceleration (m/s²) |

---

## Common Effect Recipes

### Fireball Projectile
```json
{
  "spreadAngle": 80,
  "emitRate": 80,
  "looping": true,
  "gravity": 0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 0.8, 0.3, 1.0]},
    {"time": 1.0, "color": [1.0, 0.2, 0.0, 0.0]}
  ]
}
```

### Shield Burst
```json
{
  "burstCount": 30,
  "emitRate": 0,
  "spreadAngle": 180,
  "gravity": 0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [0.3, 0.7, 1.0, 0.8]},
    {"time": 1.0, "color": [0.1, 0.3, 1.0, 0.0]}
  ]
}
```

### Explosion (Impact)
```json
{
  "burstCount": 120,
  "emitRate": 0,
  "spreadAngle": 180,
  "gravity": 5,
  "lifetimeMin": 0.5,
  "lifetimeMax": 1.2,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 0.9, 0.4, 1.0]},
    {"time": 0.3, "color": [1.0, 0.3, 0.0, 0.9]},
    {"time": 1.0, "color": [0.0, 0.0, 0.0, 0.0]}
  ]
}
```

### Healing Aura (Looping)
```json
{
  "emitRate": 20,
  "looping": true,
  "duration": 100,
  "spreadAngle": 360,
  "gravity": 0,
  "initialSpeedMin": 0.5,
  "initialSpeedMax": 1.5,
  "colorOverLifetime": [
    {"time": 0.0, "color": [0.0, 1.0, 0.0, 0.5]},
    {"time": 1.0, "color": [0.0, 1.0, 0.0, 0.0]}
  ]
}
```

---

## Debug Overlay Controls

### DebugRenderer (Immediate-Mode Shapes)
```cpp
// In Renderer code:
m_debugRenderer.drawLine(p1, p2, color);           // Line
m_debugRenderer.drawCircle(center, radius, color); // Circle
m_debugRenderer.drawAABB(min, max, color);         // Cube
m_debugRenderer.drawSphere(center, radius, color); // Sphere
m_debugRenderer.render(cmd, viewProj);             // Render accumulated
```

### ClickIndicatorRenderer (Sprite UI)
```cpp
m_clickIndicatorRenderer->render(cmd, viewProj, 
    center, animTime, size, tint);
```

### Show/Hide Grid
```cpp
m_showGrid = !m_showGrid;  // Toggle infinite grid
```

---

## Fog Adjustment

### Current Settings (Default)
```
fogColor: (0.6, 0.65, 0.75)    // Blueish-gray
fogDensity: 0.03               // Strength of fog
fogStart: 5 meters             // Near edge
fogEnd: 50 meters              // Far edge
```

### To Modify Fog
In `src/renderer/Descriptors.h`, lines 36-39:
```cpp
LightUBO light{};
light.fogColor = glm::vec3(0.6f, 0.65f, 0.75f);
light.fogDensity = 0.05f;  // Higher = denser fog
light.fogStart = 10.0f;
light.fogEnd = 100.0f;
m_descriptors->updateLightBuffer(frameIndex, light);
```

---

## Adding Attack Slashes & Shields

### Slash VFX (Particle-Based)
```json
{
  "id": "vfx_attack_slash",
  "maxParticles": 32,
  "burstCount": 8,
  "emitRate": 0,
  "looping": false,
  "duration": 0.3,
  "lifetimeMin": 0.2,
  "lifetimeMax": 0.3,
  "initialSpeedMin": 8.0,
  "initialSpeedMax": 15.0,
  "sizeMin": 0.4,
  "sizeMax": 0.8,
  "spreadAngle": 15,
  "gravity": 0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 1.0, 1.0, 1.0]},
    {"time": 0.5, "color": [1.0, 0.5, 0.0, 0.5]},
    {"time": 1.0, "color": [1.0, 0.0, 0.0, 0.0]}
  ]
}
```

### Shield VFX (Protective Barrier)
```json
{
  "id": "vfx_shield_proc",
  "maxParticles": 64,
  "burstCount": 25,
  "emitRate": 0,
  "looping": false,
  "duration": 0.6,
  "lifetimeMin": 0.4,
  "lifetimeMax": 0.6,
  "initialSpeedMin": 3.0,
  "initialSpeedMax": 6.0,
  "sizeMin": 0.5,
  "sizeMax": 1.2,
  "spreadAngle": 180,
  "gravity": 0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [0.3, 0.7, 1.0, 0.8]},
    {"time": 0.5, "color": [0.2, 0.5, 0.8, 0.4]},
    {"time": 1.0, "color": [0.1, 0.3, 1.0, 0.0]}
  ]
}
```

---

## Projectile VFX (Three-Stage)

### Stage 1: Cast (at caster)
```json
{"id": "vfx_arrow_cast", "burstCount": 5, ...}
```

### Stage 2: Flight (follows projectile)
```json
{"id": "vfx_arrow_flight", "emitRate": 30, "looping": true, ...}
```

### Stage 3: Impact (at target)
```json
{"id": "vfx_arrow_impact", "burstCount": 40, ...}
```

**In ability JSON**:
```json
{
  "castVFX": "vfx_arrow_cast",
  "projectileVFX": "vfx_arrow_flight",
  "impactVFX": "vfx_arrow_impact"
}
```

---

## Performance Tips

- **Max 2048 particles per emitter** (configurable in VFXTypes.h)
- **Max 32 concurrent emitters** (configurable)
- **GPU simulation** → scales to thousands of particles
- **Lock-free queue** → zero mutex overhead
- **Atlas textures** → reuse across effects

---

## Troubleshooting

**Particles not rendering?**
- Check descriptor set binding 1 (atlas texture)
- Verify EmitterDef id matches JSON file
- Check VFXRenderer loaded emitter directory

**Fog looks wrong?**
- Increase `fogDensity` for heavier fog
- Adjust `fogColor` to match sky
- Check `fogStart`/`fogEnd` range

**VFX events dropped?**
- Queue capacity is 256 events
- Check game frame rate (update loop may be too slow)
- Verify AbilitySystem calls `update()` each frame

**Compute shader errors?**
- Ensure particle count passed to compute matches SSBO size
- Check push constant struct alignment
- Verify dt is positive (not negative or zero)

---

## References

**Full Documentation**: See `RENDERING_VFX_SYSTEM_DEEP_DIVE.md`

**Source Files**:
- `src/vfx/VFXRenderer.h/cpp` — Master VFX orchestrator
- `src/vfx/ParticleSystem.h/cpp` — Single emitter instance
- `src/vfx/VFXTypes.h` — Data structures
- `src/vfx/VFXEventQueue.h` — Lock-free queue
- `shaders/particle_sim.comp` — GPU simulation
- `shaders/particle.vert/frag` — Billboard rendering

**Assets**:
- `assets/vfx/` — Particle effect definitions (JSON)
- `assets/abilities/` — Ability definitions (JSON)
- `assets/textures/particles/` — Particle atlases (PNG)

