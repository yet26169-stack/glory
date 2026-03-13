# Glory Engine — Advanced VFX System Implementation Specification

> **Author perspective:** Senior Vulkan game engine developer & real-time VFX engineer  
> **Target audience:** Implementation agent with full repository write access  
> **Repository:** `donkey-ux/glory` (commit `65a61d4`)  
> **Date:** 2026-03-12

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Existing Architecture Audit](#2-existing-architecture-audit)
3. [Feature 1 — HDR Offscreen Rendering & Bloom](#3-feature-1--hdr-offscreen-rendering--bloom)
4. [Feature 2 — Trail / Ribbon Particle Renderer](#4-feature-2--trail--ribbon-particle-renderer)
5. [Feature 3 — Ground Decal System](#5-feature-3--ground-decal-system)
6. [Feature 4 — Mesh-Based VFX Effects](#6-feature-4--mesh-based-vfx-effects)
7. [Feature 5 — Composite VFX Sequencer](#7-feature-5--composite-vfx-sequencer)
8. [Feature 6 — Distortion / Refraction Effects](#8-feature-6--distortion--refraction-effects)
9. [Feature 7 — Soft Particles (Depth Fade)](#9-feature-7--soft-particles-depth-fade)
10. [Feature 8 — Additive Blend Pipeline for VFXRenderer](#10-feature-8--additive-blend-pipeline-for-vfxrenderer)
11. [Feature 9 — GPU Indirect Draw (Alive-List Compaction)](#11-feature-9--gpu-indirect-draw-alive-list-compaction)
12. [Render Loop Integration Order](#12-render-loop-integration-order)
13. [File Placement Conventions](#13-file-placement-conventions)
14. [Vulkan Constraints & Safety Rules](#14-vulkan-constraints--safety-rules)
15. [Testing & Validation Strategy](#15-testing--validation-strategy)

---

## 1. Executive Summary

The Glory engine is a MOBA (League of Legends–style) game built on a custom Vulkan 1.0+ renderer. It currently has a working GPU-compute particle system, four procedural-geometry VFX renderers (cone, explosion, shield bubble, sprite-atlas), and a data-driven ability pipeline with SPSC event queuing. This document specifies **nine additive subsystems** that bring the VFX pipeline to League-of-Legends visual complexity. Each feature is self-contained: it plugs into the existing architecture without rewriting any existing code.

### Priority Order (implement in this sequence)

| Priority | Feature | Impact | Est. Effort |
|----------|---------|--------|-------------|
| P0 | HDR + Bloom | Every existing effect looks 10× better | 3–5 days |
| P1 | Trail / Ribbon Renderer | Enables projectile trails, tethers, lasers | 2–3 days |
| P2 | Ground Decal System | AoE indicators, lingering pools, scorch marks | 2–3 days |
| P3 | Mesh-Based VFX | Slash arcs, weapon trails, spinning rings | 3–5 days |
| P4 | Composite VFX Sequencer | Multi-layer ability choreography | 1–2 days |
| P5 | Distortion / Refraction | Heat haze, portal warp, water ripple | 2–3 days |
| P6 | Soft Particles | Particles fade at terrain intersection | 0.5–1 day |
| P7 | Additive Blend for VFXRenderer | Fire / magic glow particles | 0.5–1 day |
| P8 | GPU Indirect Draw | Performance: skip dead particles | 1–2 days |

---

## 2. Existing Architecture Audit

### 2.1 Repository Source Map

```
src/
├── ability/
│   ├── AbilitySystem.h/.cpp      ← State machine, JSON loader, VFX event emitter
│   ├── AbilityTypes.h            ← AbilityDefinition, ProjectileDef, EffectDef, Stats
│   ├── AbilityComponents.h       ← ECS components (AbilityBookComponent, ProjectileComponent)
│   └── ProjectileSystem.h/.cpp   ← Moves projectiles, syncs trail VFX, collision
├── vfx/
│   ├── VFXTypes.h                ← GpuParticle (64B), EmitterDef, VFXEvent, EmitterParams
│   ├── VFXEventQueue.h           ← Lock-free SPSC<256> ring buffer
│   ├── ParticleSystem.h/.cpp     ← Single emitter: SSBO, UBO, descriptor set, CPU spawn
│   └── VFXRenderer.h/.cpp        ← Pool of ParticleSystems, compute+render pipelines
├── renderer/
│   ├── Renderer.h/.cpp           ← Main render loop, owns all subsystems
│   ├── ShieldBubbleRenderer.h/.cpp   ← Two-pass glass sphere (Fresnel)
│   ├── ConeAbilityRenderer.h/.cpp    ← Three-pass cone (energy + grid + lightning)
│   ├── ExplosionRenderer.h/.cpp      ← Two-pass explosion (shockwave disk + fireball sphere)
│   ├── SpriteEffectRenderer.h/.cpp   ← Atlas-based billboard animations (alpha + additive)
│   ├── ClickIndicatorRenderer.h/.cpp ← Click feedback ring
│   ├── Device.h/.cpp             ← VkDevice, VMA allocator, queues
│   ├── Buffer.h/.cpp             ← VMA-backed VkBuffer wrapper
│   ├── Texture.h/.cpp            ← VMA-backed VkImage + sampler
│   ├── Swapchain.h/.cpp          ← VkSwapchainKHR management
│   ├── Pipeline.h/.cpp           ← Forward render pass + framebuffers (swapchain-direct)
│   └── ...
shaders/
├── particle_sim.comp             ← GPU particle physics (gravity, drag, wind, color curves)
├── particle.vert                 ← Billboard expansion from SSBO (6 verts/particle)
├── particle.frag                 ← Atlas sampling, premultiplied alpha
├── cone_ability.vert             ← Procedural cone mesh decode
├── cone_energy.frag              ← Animated wave-front energy sweep
├── cone_grid.frag                ← Scrolling wireframe overlay
├── cone_lightning.vert/.frag     ← CPU-jittered line-strip lightning
├── explosion_disk.vert           ← Expanding polar disk mesh
├── explosion_shockwave.frag      ← Multi-ring + 8-spike radial pattern
├── explosion_sphere.vert         ← Animated sphere (radius scale over time)
├── explosion_fireball.frag       ← Domain-warped FBM plasma + Fresnel rim
├── shield_bubble.vert/.frag      ← Sphere mesh + Fresnel glass + Blinn-Phong
├── sprite_effect_billboard.vert  ← Camera-facing quad with atlas UV
├── sprite_effect_additive.frag   ← Additive texture sampling
├── triangle.vert/.frag           ← Forward PBR (main scene geometry)
├── skinned.vert                  ← Skeletal animation vertex shader
├── grid.vert/.frag               ← Debug grid overlay
├── debug.vert/.frag              ← Nav-mesh / debug line rendering
└── click_indicator.vert/.frag    ← Click-to-move ring animation
```

### 2.2 Current GPU Particle Pipeline (VFXRenderer)

**Descriptor Set Layout** (shared by compute + render):
- **Binding 0:** `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` — particle SSBO (`COMPUTE | VERTEX`)
- **Binding 1:** `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` — atlas texture (`FRAGMENT`)
- **Binding 2:** `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` — `EmitterParams` UBO (`COMPUTE | VERTEX`)

**Compute Pipeline:**
- Shader: `particle_sim.comp` (workgroup size 64)
- Push constants: `SimPC { float dt; float gravity; uint32_t count; float _pad; }` (16 bytes)
- Dispatches `ceil(maxParticles / 64)` workgroups per emitter

**Render Pipeline:**
- Shaders: `particle.vert` + `particle.frag`
- Push constants: `RenderPC { mat4 viewProj; vec4 camRight; vec4 camUp; }` (96 bytes)
- No vertex buffer — positions from SSBO. 6 vertices per particle (2 triangles).
- Blend: standard alpha (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`)
- Depth: test ON, write OFF

**GpuParticle struct** (64 bytes, must match GLSL exactly):
```c
struct GpuParticle {
    vec4 posLife;   // xyz=position, w=remaining_lifetime
    vec4 velAge;    // xyz=velocity, w=age
    vec4 color;     // rgba
    vec4 params;    // x=size, y=rotation, z=angularVel, w=packed(frame+active)
};
```

**EmitterParams UBO** (≤512 bytes):
```c
struct EmitterParams {
    vec4     wind_dt;        // xyz=wind*strength, w=dt
    vec4     phys;           // x=gravity, y=drag, z=alphaCurve, w=count
    vec4     size;           // x=sizeMin, y=sizeMax, z=sizeEnd, w=reserved
    uint     colorKeyCount;
    float    _pad[3];
    GpuColorKey colorKeys[8]; // each: vec4 color + float time + float[3] pad
};
```

### 2.3 Current Render Pass

**CRITICAL:** The engine currently renders **directly to swapchain images** via a single forward render pass (`Pipeline.h`). There is no offscreen HDR target. The render pass format is the swapchain format (typically `VK_FORMAT_B8G8R8A8_SRGB` or `VK_FORMAT_B8G8R8A8_UNORM`).

This means Feature 1 (HDR + Bloom) fundamentally changes the rendering flow and must be implemented first.

### 2.4 SPSC Event Queue Bridge

The `VFXEventQueue` (typedef for `SPSCQueue<256>`) bridges the game thread to the render thread. Events:
```c
enum class VFXEventType : uint8_t { Spawn, Destroy, Move };
struct VFXEvent {
    VFXEventType type;
    uint32_t     handle;        // output for Spawn, input for Destroy/Move
    char         effectID[48];  // EmitterDef id
    glm::vec3    position;
    glm::vec3    direction;
    float        scale;
    float        lifetime;      // <0 = use EmitterDef.duration
};
```

Two separate queues exist: `m_vfxQueue` (AbilitySystem→VFXRenderer) and `m_combatVfxQueue` (CombatSystem→VFXRenderer).

### 2.5 Existing Renderer Subsystems (all in `Renderer.h`)

| Subsystem | Header | Init Pattern | Data Delivery |
|-----------|--------|-------------|--------------|
| `ShieldBubbleRenderer` | `src/renderer/ShieldBubbleRenderer.h` | `init(Device&, VkRenderPass)` | Push constants only (112B) |
| `ConeAbilityRenderer` | `src/renderer/ConeAbilityRenderer.h` | `init(Device&, VkRenderPass)` | Push constants only (128B) |
| `ExplosionRenderer` | `src/renderer/ExplosionRenderer.h` | `init(Device&, VkRenderPass)` | Push constants only (112B) |
| `SpriteEffectRenderer` | `src/renderer/SpriteEffectRenderer.h` | `init(Device&, VkRenderPass)` | Push constants (112B) + descriptor set (sampler) |
| `VFXRenderer` | `src/vfx/VFXRenderer.h` | ctor(Device&, VkRenderPass) | Descriptor sets (SSBO + sampler + UBO) + push constants |

All subsystems follow the pattern:
1. `init()` or constructor creates pipelines from SPIR-V at `SHADER_DIR`
2. `update(dt)` advances CPU state
3. `render(cmd, viewProj, ...)` records draw commands into the active render pass

### 2.6 AbilityDefinition VFX Fields

```c
struct AbilityDefinition {
    // ...
    std::vector<std::string> castVFX;        // particle emitters at cast position
    std::vector<std::string> projectileVFX;  // emitters attached to projectile
    std::vector<std::string> impactVFX;      // emitters at impact position
    std::string castSFX;
    std::string impactSFX;
    // ...
};
```

Currently these only reference particle `EmitterDef` IDs. The composite sequencer (Feature 5) will extend this to reference trails, decals, mesh effects, and screen effects.

---

## 3. Feature 1 — HDR Offscreen Rendering & Bloom

### 3.1 Why This Must Be First

Every VFX subsystem currently writes to an 8-bit swapchain framebuffer. This means:
- Fragment shaders that output values > 1.0 are clamped (no overbright glow)
- Bloom is impossible without an HDR intermediate
- Additive blending saturates to white at 1.0 instead of creating natural glow falloff

### 3.2 Architecture Change

```
BEFORE:
  Render Pass → Swapchain Framebuffer (8-bit)

AFTER:
  Render Pass 1 (Scene + VFX) → HDR Color Attachment (R16G16B16A16_SFLOAT)
                                 + Depth Attachment (existing)
  Bloom passes (compute or graphics):
    HDR → Bright-pass extraction → Gaussian blur → Composite
  Render Pass 2 (Tone-map + Composite) → Swapchain Framebuffer (8-bit)
```

### 3.3 New Files

| File | Purpose |
|------|---------|
| `src/renderer/HDRFramebuffer.h/.cpp` | Manages the offscreen HDR color image + depth image |
| `src/renderer/BloomPass.h/.cpp` | Bright-pass extraction, separable Gaussian blur, composite |
| `src/renderer/ToneMapPass.h/.cpp` | Full-screen triangle that reads HDR + bloom → writes to swapchain |
| `shaders/bloom_extract.frag` | `if (luminance > threshold) output color; else discard` |
| `shaders/bloom_blur.frag` | Separable Gaussian (horizontal and vertical passes) |
| `shaders/tonemap.vert` | Full-screen triangle (3 vertices, no VBO) |
| `shaders/tonemap.frag` | ACES/Reinhard tone mapping + bloom composite + gamma |

### 3.4 HDRFramebuffer Specification

```cpp
// src/renderer/HDRFramebuffer.h
class HDRFramebuffer {
public:
    void init(const Device& device, uint32_t width, uint32_t height,
              VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
              VkFormat depthFormat = VK_FORMAT_D32_SFLOAT);

    void recreate(uint32_t width, uint32_t height); // on swapchain resize

    VkRenderPass    renderPass()  const; // color + depth, LOAD_OP_CLEAR
    VkFramebuffer   framebuffer() const;
    VkImageView     colorView()   const; // for bloom input sampling
    VkImageView     depthView()   const; // for soft particles (Feature 7)
    VkSampler       sampler()     const; // linear, clamp-to-edge

    void destroy();

private:
    // VMA-allocated images: m_colorImage, m_depthImage
    // VkImageViews, VkSampler, VkRenderPass, VkFramebuffer
};
```

**Render pass configuration:**
- Color attachment: `R16G16B16A16_SFLOAT`, `LOAD_OP_CLEAR`, `STORE_OP_STORE`, final layout `SHADER_READ_ONLY_OPTIMAL`
- Depth attachment: `D32_SFLOAT`, `LOAD_OP_CLEAR`, `STORE_OP_STORE`, final layout `DEPTH_STENCIL_READ_ONLY_OPTIMAL`
- Subpass: color attachment 0 + depth-stencil attachment

### 3.5 BloomPass Specification

```cpp
// src/renderer/BloomPass.h
class BloomPass {
public:
    void init(const Device& device, VkImageView hdrColorView, VkSampler sampler,
              uint32_t width, uint32_t height);

    // Records bright-pass extract + N blur passes into the command buffer.
    // Call OUTSIDE the scene render pass, AFTER the HDR pass has finished.
    void dispatch(VkCommandBuffer cmd);

    VkImageView bloomResultView() const; // blurred bright image for composite

    void recreate(VkImageView hdrColorView, uint32_t width, uint32_t height);
    void destroy();

private:
    // Two half-res images for ping-pong blur
    // Bright-pass extract pipeline (graphics or compute)
    // Horizontal + vertical Gaussian blur pipelines
    // Descriptor sets: input image → output image
    float m_threshold    = 1.0f;  // luminance threshold for bloom
    int   m_blurPasses   = 5;     // number of ping-pong iterations
};
```

**Blur implementation:**
- Downsample HDR to half resolution on extraction
- 5-tap or 9-tap separable Gaussian
- 5 ping-pong passes (H → V → H → V → H) for wide diffuse bloom
- Final result at half resolution is sufficient; ToneMapPass samples with bilinear

### 3.6 ToneMapPass Specification

```cpp
// src/renderer/ToneMapPass.h
class ToneMapPass {
public:
    void init(const Device& device, VkRenderPass swapchainRenderPass,
              VkImageView hdrView, VkImageView bloomView, VkSampler sampler);

    // Draw a full-screen triangle that reads HDR + bloom and writes tone-mapped
    // result to the current swapchain framebuffer.
    void render(VkCommandBuffer cmd, float exposure, float bloomStrength);

    void destroy();
};
```

### 3.7 Tone-Map Shader

```glsl
// shaders/tonemap.frag
#version 450

layout(set=0, binding=0) uniform sampler2D hdrColor;
layout(set=0, binding=1) uniform sampler2D bloomColor;

layout(push_constant) uniform ToneMapPC {
    float exposure;
    float bloomStrength;
    float _pad[2];
} pc;

layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;

// ACES filmic tone mapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x*(a*x+b))/(x*(c*x+d)+e), 0.0, 1.0);
}

void main() {
    vec3 hdr   = texture(hdrColor,   uv).rgb;
    vec3 bloom = texture(bloomColor, uv).rgb;

    vec3 combined = hdr + bloom * pc.bloomStrength;
    combined *= pc.exposure;

    vec3 mapped = ACESFilm(combined);

    // sRGB gamma (if swapchain is UNORM; skip if swapchain is SRGB)
    // mapped = pow(mapped, vec3(1.0 / 2.2));

    outColor = vec4(mapped, 1.0);
}
```

### 3.8 Integration into Renderer.cpp

**Changes to `Renderer.h`:**
```cpp
// Add new members:
std::unique_ptr<HDRFramebuffer> m_hdrFB;
std::unique_ptr<BloomPass>      m_bloom;
std::unique_ptr<ToneMapPass>    m_toneMap;
```

**Changes to `Renderer::Renderer()`:**
```cpp
// AFTER creating m_device and m_swapchain:
m_hdrFB = std::make_unique<HDRFramebuffer>();
m_hdrFB->init(*m_device, extent.width, extent.height);

// REPLACE: m_pipeline = std::make_unique<Pipeline>(*m_device, *m_swapchain, ...);
// The scene Pipeline now uses m_hdrFB->renderPass() instead of the swapchain render pass.
m_pipeline = std::make_unique<Pipeline>(*m_device, *m_swapchain,
                                         m_descriptors->getLayout(),
                                         m_hdrFB->renderPass()); // ← HDR render pass

// ALL existing renderer inits now receive m_hdrFB->renderPass():
m_shieldBubble->init(*m_device, m_hdrFB->renderPass());
m_coneEffect->init(*m_device, m_hdrFB->renderPass());
m_explosionRenderer->init(*m_device, m_hdrFB->renderPass());
// ... etc

// After all scene renderers:
m_bloom = std::make_unique<BloomPass>();
m_bloom->init(*m_device, m_hdrFB->colorView(), m_hdrFB->sampler(),
              extent.width, extent.height);

// ToneMap renders into swapchain render pass (need a simple swapchain-only render pass)
m_toneMap = std::make_unique<ToneMapPass>();
m_toneMap->init(*m_device, m_swapchainRenderPass,
                m_hdrFB->colorView(), m_bloom->bloomResultView(), m_hdrFB->sampler());
```

**Changes to `recordCommandBuffer()`:**
```cpp
void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, float dt) {
    // ═══════════ PHASE 1: COMPUTE (outside any render pass) ═══════════
    m_vfxRenderer->dispatchCompute(cmd);
    m_vfxRenderer->barrierComputeToGraphics(cmd);

    // ════════��══ PHASE 2: HDR SCENE RENDER PASS ═══════════
    // Begin HDR render pass (writes to R16G16B16A16_SFLOAT)
    VkRenderPassBeginInfo hdrRP{};
    hdrRP.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    hdrRP.renderPass  = m_hdrFB->renderPass();
    hdrRP.framebuffer = m_hdrFB->framebuffer();
    hdrRP.renderArea  = {{0,0}, extent};
    VkClearValue clears[2];
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    hdrRP.clearValueCount = 2;
    hdrRP.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &hdrRP, VK_SUBPASS_CONTENTS_INLINE);

    // All existing draw calls go here (scene, VFX, etc.)
    // ... opaque geometry ...
    // ... ConeAbilityRenderer, ExplosionRenderer, ShieldBubbleRenderer ...
    // ... VFXRenderer::render() ...
    // ... SpriteEffectRenderer ...
    // ... DebugRenderer, grid ...

    vkCmdEndRenderPass(cmd);

    // ═══════════ PHASE 3: BLOOM (outside render pass) ═══════════
    // Transition HDR color: SHADER_READ_ONLY → SHADER_READ_ONLY (already correct)
    m_bloom->dispatch(cmd);

    // ═══════════ PHASE 4: TONE-MAP TO SWAPCHAIN ═══════════
    VkRenderPassBeginInfo swapRP{};
    swapRP.renderPass  = m_swapchainRenderPass;
    swapRP.framebuffer = m_swapchainFramebuffers[imageIndex];
    // ... clear, begin
    vkCmdBeginRenderPass(cmd, &swapRP, VK_SUBPASS_CONTENTS_INLINE);

    m_toneMap->render(cmd, /*exposure=*/1.0f, /*bloomStrength=*/0.3f);

    // ImGui renders on top of tone-mapped result
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRenderPass(cmd);
}
```

### 3.9 Swapchain Resize Handling

In `recreateSwapchain()`, add:
```cpp
m_hdrFB->recreate(newExtent.width, newExtent.height);
m_bloom->recreate(m_hdrFB->colorView(), newExtent.width, newExtent.height);
// ToneMap descriptor sets need updating with new image views
```

---

## 4. Feature 2 — Trail / Ribbon Particle Renderer

### 4.1 Problem

Projectile flight trails (Ezreal Q, Ahri charm tether), laser beams (Lux R), and energy ribbons require **connected geometry** that follows a path over time. The current particle system only produces **independent point-billboards** — each `GpuParticle` is a standalone 64-byte struct with no connectivity to neighbors.

### 4.2 Data Model

```cpp
// src/vfx/TrailTypes.h
#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

static constexpr uint32_t MAX_TRAIL_POINTS    = 64;
static constexpr uint32_t MAX_ACTIVE_TRAILS   = 32;
static constexpr uint32_t INVALID_TRAIL_HANDLE = 0;

// GPU-side trail point (matches GLSL)
struct alignas(16) GpuTrailPoint {
    glm::vec4 posWidth;    // xyz = world position, w = half-width at this point
    glm::vec4 colorAge;    // rgb = color, a = normalized age (0=newest, 1=oldest)
};
static_assert(sizeof(GpuTrailPoint) == 32, "GpuTrailPoint must be 32 bytes");

// Per-trail metadata (stored in a UBO or push constant)
struct TrailParams {
    uint32_t  pointCount;      // current number of valid points
    uint32_t  headIndex;       // ring buffer write position
    float     fadeSpeed;       // how fast old points age (1.0 = 1 second to full fade)
    float     widthStart;      // width at the head (newest)
    float     widthEnd;        // width at the tail (oldest)
    glm::vec4 colorStart;
    glm::vec4 colorEnd;
};

// CPU-side trail definition
struct TrailDef {
    std::string id;
    float maxLength       = 3.0f;    // seconds of trail history
    float emitInterval    = 0.016f;  // seconds between new points (~60 Hz)
    float widthStart      = 0.5f;
    float widthEnd        = 0.05f;
    float fadeSpeed       = 2.0f;
    glm::vec4 colorStart  = {1,1,1,1};
    glm::vec4 colorEnd    = {1,1,1,0};
    std::string textureAtlas = ""; // "" = solid color
    bool additive         = true;
};

} // namespace glory
```

### 4.3 TrailRenderer Class

```cpp
// src/vfx/TrailRenderer.h
class TrailRenderer {
public:
    TrailRenderer(const Device& device, VkRenderPass renderPass);
    ~TrailRenderer();

    void registerTrail(TrailDef def);

    // Spawn a trail attached to a moving object. Returns handle.
    uint32_t spawn(const std::string& trailDefId, glm::vec3 startPos);

    // Update position of the trail head. Call every frame while the source moves.
    void updateHead(uint32_t handle, glm::vec3 newHeadPos);

    // Detach trail from source — it will finish fading out on its own.
    void detach(uint32_t handle);

    // Per-frame bookkeeping: age all points, insert new points, remove dead trails.
    void update(float dt);

    // Record draw commands (inside render pass, after opaque, depth write OFF).
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                const glm::vec3& camRight, const glm::vec3& camUp);

private:
    // Per-trail: SSBO of GpuTrailPoint[MAX_TRAIL_POINTS] (ring buffer)
    // Graphics pipeline: trail_ribbon.vert + trail_ribbon.frag
    // Descriptor set: binding 0 = trail SSBO, binding 1 = atlas sampler
    // Draw: vkCmdDraw((pointCount - 1) * 6, 1, 0, 0)  — 2 triangles per segment
};
```

### 4.4 Trail Vertex Shader

```glsl
// shaders/trail_ribbon.vert
#version 450

struct TrailPoint {
    vec4 posWidth;  // xyz=pos, w=halfWidth
    vec4 colorAge;  // rgb=color, a=age (0=new, 1=old)
};

layout(std430, set=0, binding=0) readonly buffer TrailSSBO {
    TrailPoint points[];
};

layout(push_constant) uniform TrailPC {
    mat4  viewProj;
    vec4  camRight;   // xyz = camera right vector
    vec4  camUp;      // xyz = camera up vector
    uint  pointCount;
    uint  headIndex;
    float widthStart;
    float widthEnd;
} pc;

layout(location=0) out vec4 fragColor;
layout(location=1) out vec2 fragUV;

void main() {
    // Each segment between point[i] and point[i+1] is a quad (6 vertices)
    uint segIndex  = uint(gl_VertexIndex) / 6u;
    uint vertInSeg = uint(gl_VertexIndex) % 6u;

    if (segIndex >= pc.pointCount - 1) {
        gl_Position = vec4(0, 0, 2, 1); // degenerate
        return;
    }

    // Ring buffer unwrap
    uint capacity = pc.pointCount; // simplified: assume linear for now
    uint iA = segIndex;
    uint iB = segIndex + 1;

    TrailPoint pA = points[iA];
    TrailPoint pB = points[iB];

    // Select which end of the segment this vertex belongs to
    // Vertices 0,1,2 → triangle 1; 3,4,5 → triangle 2
    bool isB = (vertInSeg == 1 || vertInSeg == 2 || vertInSeg == 4);
    TrailPoint p = isB ? pB : pA;

    // Side: +1 or -1 (expand perpendicular to segment direction)
    float side = 1.0;
    if (vertInSeg == 0 || vertInSeg == 3 || vertInSeg == 5) side = -1.0;
    // Alternate for proper winding:
    // 0=-1, 1=+1, 2=-1, 3=-1, 4=+1, 5=+1
    float sides[6] = float[6](-1, 1, -1, -1, 1, 1);
    side = sides[vertInSeg];

    // Direction between consecutive points for perpendicular expansion
    vec3 segDir = normalize(pB.posWidth.xyz - pA.posWidth.xyz);
    // Use camera-facing expansion (billboard-style ribbon)
    vec3 viewDir = normalize(cross(segDir, pc.camRight.xyz));
    // If viewDir is degenerate, fall back to camUp
    if (length(viewDir) < 0.001) viewDir = pc.camUp.xyz;

    float width = p.posWidth.w;
    vec3 worldPos = p.posWidth.xyz + viewDir * side * width;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
    fragColor = p.colorAge;

    // UV: x = 0 or 1 (side), y = age fraction
    float u = (side + 1.0) * 0.5;
    float v = isB ? pB.colorAge.a : pA.colorAge.a;
    fragUV = vec2(u, v);
}
```

### 4.5 Trail Fragment Shader

```glsl
// shaders/trail_ribbon.frag
#version 450

layout(set=0, binding=1) uniform sampler2D trailAtlas;

layout(location=0) in vec4 fragColor;
layout(location=1) in vec2 fragUV;

layout(location=0) out vec4 outColor;

void main() {
    vec4 tex = texture(trailAtlas, fragUV);
    vec4 c = fragColor * tex;

    // Fade alpha based on age (fragColor.a carries the age fraction)
    float ageFade = 1.0 - fragColor.a;
    c.a *= ageFade;

    if (c.a < 0.005) discard;
    outColor = c;
}
```

### 4.6 Integration with ProjectileSystem

In `ProjectileSystem::update()`, after moving each projectile:
```cpp
if (pc.trailHandle != INVALID_TRAIL_HANDLE) {
    trailRenderer.updateHead(pc.trailHandle, transform.position);
}
```

On projectile spawn (in `AbilitySystem::spawnProjectile()`):
```cpp
if (!def.projectileTrailDef.empty()) {
    uint32_t trailH = trailRenderer.spawn(def.projectileTrailDef, startPos);
    pc.trailHandle = trailH;
}
```

On projectile destroy:
```cpp
trailRenderer.detach(pc.trailHandle);
```

---

## 5. Feature 3 — Ground Decal System

### 5.1 Problem

AoE ability indicators (Lux E circle, Morgana W pool), lingering ground effects (Singed Q poison trail), and impact scorch marks need textures or procedural patterns **projected onto terrain**. The existing `explosion_shockwave.frag` does a hardcoded flat disk; a general-purpose system is needed.

### 5.2 Approach: Projected Quad

The simplest approach for a flat terrain game (Y = ground plane):
1. Render a quad on the Y=0.01 plane (slightly above ground to avoid z-fighting)
2. Scale/rotate it to match the decal's world-space footprint
3. Sample a decal texture with per-instance UV animation

This avoids depth-buffer reconstruction complexity and works perfectly for the isometric MOBA camera.

### 5.3 New Files

| File | Purpose |
|------|---------|
| `src/renderer/GroundDecalRenderer.h/.cpp` | Manages decal instances, pipeline, rendering |
| `shaders/ground_decal.vert` | Transform unit quad to world-space decal footprint |
| `shaders/ground_decal.frag` | Sample decal texture, apply fade/animation |

### 5.4 GroundDecalRenderer Specification

```cpp
// src/renderer/GroundDecalRenderer.h
class GroundDecalRenderer {
public:
    struct DecalDef {
        std::string id;
        std::string texturePath;  // "" = procedural circle
        float duration = 3.0f;
        float fadeInTime = 0.1f;
        float fadeOutTime = 0.5f;
        float rotationSpeed = 0.0f;  // rad/s (spinning circle indicator)
        glm::vec4 color = {1,1,1,1};
        bool additive = false;
    };

    void init(const Device& device, VkRenderPass renderPass);
    void registerDecal(DecalDef def);

    // Spawn a ground decal. Returns handle for early removal.
    uint32_t spawn(const std::string& decalDefId, glm::vec3 center,
                   float radius, float rotation = 0.0f);

    void update(float dt);

    // Render BEFORE transparent VFX, AFTER opaque geometry.
    // Depth test ON, depth write OFF, alpha blend.
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime);

    void destroy(uint32_t handle);
    void destroyAll();

    static constexpr int MAX_DECALS = 64;

private:
    struct DecalPC {
        glm::mat4 viewProj;  // 64B
        glm::vec3 center;    // 12B
        float     radius;    //  4B
        float     rotation;  //  4B
        float     alpha;     //  4B
        float     elapsed;   //  4B
        float     appTime;   //  4B
        glm::vec4 color;     // 16B
    };  // = 112B

    // Unit quad VBO (4 vertices), draw as triangle strip
    // Per-decalDef: descriptor set with texture sampler
    // Two pipelines: alpha-blend and additive-blend
};
```

### 5.5 Ground Decal Vertex Shader

```glsl
// shaders/ground_decal.vert
#version 450

layout(location=0) in vec2 inPos;  // unit quad: (-1,-1) to (1,1)

layout(push_constant) uniform DecalPC {
    mat4  viewProj;
    vec3  center;
    float radius;
    float rotation;
    float alpha;
    float elapsed;
    float appTime;
    vec4  color;
} pc;

layout(location=0) out vec2 fragUV;

void main() {
    // Rotate the quad
    float c = cos(pc.rotation);
    float s = sin(pc.rotation);
    vec2 rotated = vec2(inPos.x * c - inPos.y * s,
                        inPos.x * s + inPos.y * c);

    // Scale to radius and place on ground plane
    vec3 worldPos = pc.center + vec3(rotated.x * pc.radius, 0.01, rotated.y * pc.radius);
    gl_Position = pc.viewProj * vec4(worldPos, 1.0);

    // UV: map from [-1,1] to [0,1]
    fragUV = inPos * 0.5 + 0.5;
}
```

---

## 6. Feature 4 — Mesh-Based VFX Effects

### 6.1 Problem

Slash arcs (auto-attack swooshes), spinning weapon shapes, expanding ring meshes, and other geometric VFX require **pre-modeled meshes** with vertex-shader animation and stylized fragment shaders. The current system only supports procedurally-generated cones/disks/spheres.

### 6.2 New Files

| File | Purpose |
|------|---------|
| `src/vfx/MeshEffect.h` | `MeshEffectDef`, `MeshEffectInstance` data types |
| `src/vfx/MeshEffectRenderer.h/.cpp` | Loads VFX meshes, manages pipelines, renders |
| `shaders/mesh_effect.vert` | Scale/rotate/deform mesh vertices over lifetime |
| `shaders/mesh_effect_energy.frag` | Procedural energy/glow fragment shader |
| `shaders/mesh_effect_slash.frag` | Directional slash arc with gradient fade |

### 6.3 MeshEffectRenderer Specification

```cpp
// src/vfx/MeshEffectRenderer.h
class MeshEffectRenderer {
public:
    struct MeshEffectDef {
        std::string id;
        std::string meshPath;       // glTF / OBJ file in assets/vfx/meshes/
        std::string vertShader;     // SPIR-V path (default: mesh_effect.vert.spv)
        std::string fragShader;     // SPIR-V path (default: mesh_effect_energy.frag.spv)
        float duration     = 0.5f;
        float scaleStart   = 0.1f;
        float scaleEnd     = 1.0f;
        float alphaStart   = 1.0f;
        float alphaEnd     = 0.0f;
        float rotationSpeed = 0.0f; // rad/s
        glm::vec4 colorStart = {1,1,1,1};
        glm::vec4 colorEnd   = {1,1,1,0};
        bool additive = true;
    };

    void init(const Device& device, VkRenderPass renderPass);

    void registerDef(MeshEffectDef def);

    void spawn(const std::string& defId, glm::vec3 position,
               glm::vec3 direction, float scale = 1.0f);

    void update(float dt);

    void render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                const glm::vec3& cameraPos, float appTime);

    void destroy();

    static constexpr int MAX_INSTANCES = 32;

private:
    struct MeshEffectPC {
        glm::mat4 viewProj;      // 64B
        glm::mat4 model;         // 64B — contains position, rotation, scale
    };  // 128B (Vulkan minimum guarantee)

    // Cached pipeline per unique (vert, frag) pair
    // Loaded mesh data: VBO + IBO per mesh
    // Active instances with elapsed timers
};
```

### 6.4 Mesh Effect Vertex Shader

```glsl
// shaders/mesh_effect.vert
#version 450

layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUV;

layout(push_constant) uniform MeshEffectPC {
    mat4 viewProj;
    mat4 model;
} pc;

layout(location=0) out vec3 fragWorldPos;
layout(location=1) out vec3 fragNormal;
layout(location=2) out vec2 fragUV;

void main() {
    vec4 worldPos = pc.model * vec4(inPos, 1.0);
    gl_Position = pc.viewProj * worldPos;
    fragWorldPos = worldPos.xyz;
    fragNormal = mat3(pc.model) * inNormal;
    fragUV = inUV;
}
```

---

## 7. Feature 5 — Composite VFX Sequencer

### 7.1 Problem

A League-level ability fires 3–8 different VFX layers with staggered timings: cast particles, cone/shockwave, projectile trail, impact burst, ground decal, screen flash. Currently, `AbilitySystem` only references `castVFX` / `projectileVFX` / `impactVFX` as particle emitter IDs. There is no way to trigger different VFX subsystem types from a single ability definition.

### 7.2 Data Model

```cpp
// src/vfx/CompositeVFXDef.h
#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace glory {

enum class VFXLayerType : uint8_t {
    PARTICLE,       // → VFXRenderer (existing)
    TRAIL,          // → TrailRenderer (Feature 2)
    GROUND_DECAL,   // → GroundDecalRenderer (Feature 3)
    MESH_EFFECT,    // → MeshEffectRenderer (Feature 4)
    SHOCKWAVE,      // → ExplosionRenderer (existing)
    CONE,           // → ConeAbilityRenderer (existing)
    SHIELD,         // → ShieldBubbleRenderer (existing)
    SPRITE_EFFECT,  // → SpriteEffectRenderer (existing)
    SCREEN_SHAKE,   // → Camera system
    SCREEN_FLASH,   // → ToneMapPass (temporary exposure boost)
};

struct VFXLayer {
    float          delay      = 0.0f;    // seconds after trigger to fire
    VFXLayerType   type       = VFXLayerType::PARTICLE;
    std::string    effectRef;             // ID in the relevant subsystem
    float          duration   = -1.0f;   // override duration (-1 = use default)
    float          scale      = 1.0f;
    glm::vec4      color      = {1,1,1,1};

    // Positional mode
    enum class Anchor { CASTER, TARGET, PROJECTILE, WORLD };
    Anchor         anchor     = Anchor::CASTER;
    glm::vec3      offset     = {0,0,0};
};

struct CompositeVFXDef {
    std::string            id;
    std::vector<VFXLayer>  layers;
};

} // namespace glory
```

### 7.3 CompositeVFXSequencer Class

```cpp
// src/vfx/CompositeVFXSequencer.h
class CompositeVFXSequencer {
public:
    void loadDirectory(const std::string& dirPath); // *.json composite defs

    // Trigger a composite effect. Returns a handle for early cancellation.
    uint32_t trigger(const std::string& compositeId,
                     glm::vec3 casterPos,
                     glm::vec3 targetPos,
                     glm::vec3 direction);

    void cancel(uint32_t handle);

    // Per-frame: check layer delays, fire pending layers to subsystem dispatchers.
    void update(float dt,
                VFXEventQueue& particleQueue,
                TrailRenderer& trails,
                GroundDecalRenderer& decals,
                MeshEffectRenderer& meshFX,
                ExplosionRenderer& explosions,
                ConeAbilityRenderer& cones,
                SpriteEffectRenderer& sprites);

private:
    struct ActiveComposite {
        uint32_t     handle;
        const CompositeVFXDef* def;
        float        elapsed = 0.0f;
        glm::vec3    casterPos, targetPos, direction;
        std::vector<bool> fired; // one bool per layer
    };

    std::unordered_map<std::string, CompositeVFXDef> m_defs;
    std::vector<ActiveComposite> m_active;
    uint32_t m_nextHandle = 1;
};
```

### 7.4 JSON Format

```json
{
  "id": "composite_ultimate_r",
  "layers": [
    { "delay": 0.0,  "type": "PARTICLE",      "effectRef": "vfx_r_cast_sparks", "anchor": "CASTER" },
    { "delay": 0.0,  "type": "MESH_EFFECT",   "effectRef": "mesh_blade_ring",   "anchor": "CASTER", "scale": 2.0 },
    { "delay": 0.05, "type": "SHOCKWAVE",     "effectRef": "default",           "anchor": "CASTER" },
    { "delay": 0.0,  "type": "SCREEN_FLASH",  "effectRef": "white_flash",       "duration": 0.15 },
    { "delay": 0.1,  "type": "PARTICLE",      "effectRef": "vfx_r_debris",      "anchor": "CASTER" },
    { "delay": 0.3,  "type": "GROUND_DECAL",  "effectRef": "decal_scorchmark",  "anchor": "CASTER", "duration": 5.0 }
  ]
}
```

### 7.5 Integration with AbilitySystem

Extend `AbilityDefinition`:
```cpp
struct AbilityDefinition {
    // ... existing fields ...

    // NEW: composite VFX references (replace raw castVFX/impactVFX for complex abilities)
    std::string compositeCastVFX;      // composite ID triggered at cast time
    std::string compositeImpactVFX;    // composite ID triggered at impact
};
```

In `AbilitySystem::executeAbility()`:
```cpp
if (!def.compositeCastVFX.empty()) {
    m_compositeSequencer.trigger(def.compositeCastVFX, casterPos, targetPos, direction);
} else {
    // fallback to existing castVFX particle emitters
    for (auto& vfxId : def.castVFX) emitVFX(vfxId, casterPos, direction);
}
```

---

## 8. Feature 6 — Distortion / Refraction Effects

### 8.1 Approach

After the HDR scene pass (Feature 1), copy the HDR color buffer to a separate `sceneColorCopy` image. Distortion shaders sample this copy with UV offsets.

### 8.2 New Shader

```glsl
// shaders/distortion.frag
#version 450

layout(set=0, binding=0) uniform sampler2D sceneColor; // copy of HDR before distortion

layout(push_constant) uniform DistortionPC {
    mat4  viewProj;
    vec3  center;       // world-space center of distortion
    float radius;
    float strength;     // UV offset multiplier
    float elapsed;
    vec2  screenSize;   // viewport dimensions
} pc;

layout(location=0) in vec3 fragWorldPos;
layout(location=0) out vec4 outColor;

void main() {
    vec2 screenUV = gl_FragCoord.xy / pc.screenSize;
    vec2 toCenter = screenUV - 0.5; // simplified; real version projects center to screen

    // Radial distortion: offset UV away from center
    float dist = length(fragWorldPos - pc.center);
    float falloff = 1.0 - smoothstep(0.0, pc.radius, dist);
    float wave = sin(dist * 10.0 - pc.elapsed * 5.0) * 0.5 + 0.5;

    vec2 offset = normalize(toCenter) * wave * falloff * pc.strength * 0.02;
    vec3 distorted = texture(sceneColor, screenUV + offset).rgb;

    outColor = vec4(distorted, falloff * 0.3);
}
```

### 8.3 Implementation Notes

- The scene color copy is created with `vkCmdCopyImage` or `vkCmdBlitImage` after the HDR render pass ends but before distortion objects render
- Distortion objects render in a **separate sub-pass** or a second render pass that writes to the same HDR target
- Alternatively: use a two-pass approach where distortion is composited during tone-mapping

---

## 9. Feature 7 — Soft Particles (Depth Fade)

### 9.1 Change to particle.frag

Once the depth buffer is available as a sample-able texture (from HDR framebuffer, Feature 1):

```glsl
// Add to particle.frag:
layout(set=0, binding=3) uniform sampler2D depthBuffer; // HDR depth attachment

layout(push_constant) uniform RenderPC {
    mat4  viewProj;
    vec4  camRight;
    vec4  camUp;
    vec2  screenSize;   // NEW: viewport width/height
    float nearPlane;    // NEW: camera near plane
    float farPlane;     // NEW: camera far plane
} pc;

// In main():
vec2 screenUV = gl_FragCoord.xy / pc.screenSize;
float sceneDepth = texture(depthBuffer, screenUV).r;

// Linearize depths
float particleLinear = pc.nearPlane * pc.farPlane /
    (pc.farPlane - gl_FragCoord.z * (pc.farPlane - pc.nearPlane));
float sceneLinear = pc.nearPlane * pc.farPlane /
    (pc.farPlane - sceneDepth * (pc.farPlane - pc.nearPlane));

float softFade = smoothstep(0.0, 0.5, sceneLinear - particleLinear);
outColor.a *= softFade;
```

### 9.2 Descriptor Set Change

Add **binding 3** to the VFXRenderer descriptor set layout:
```cpp
bindings[3].binding         = 3;
bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
bindings[3].descriptorCount = 1;
bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
```

Update the `RenderPC` push constant struct to include `screenSize`, `nearPlane`, `farPlane`. **IMPORTANT:** This increases push constant size. Verify it stays within 128 bytes (Vulkan minimum guaranteed).

---

## 10. Feature 8 — Additive Blend Pipeline for VFXRenderer

### 10.1 Change

Create a second graphics pipeline in `VFXRenderer::createRenderPipeline()` identical to the existing one except for the blend state:

```cpp
VkPipelineColorBlendAttachmentState additiveBlend{};
additiveBlend.blendEnable         = VK_TRUE;
additiveBlend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
additiveBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;  // ← ADDITIVE
additiveBlend.colorBlendOp        = VK_BLEND_OP_ADD;
additiveBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
additiveBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
additiveBlend.alphaBlendOp        = VK_BLEND_OP_ADD;
additiveBlend.colorWriteMask      = 0xF;
```

### 10.2 EmitterDef Extension

Add field:
```cpp
enum class BlendMode : uint8_t { ALPHA, ADDITIVE };
BlendMode blendMode = BlendMode::ALPHA;
```

And in JSON:
```json
{ "blendMode": "additive" }
```

### 10.3 Render Change

In `VFXRenderer::render()`, sort effects by blend mode and bind the appropriate pipeline:

```cpp
// First pass: alpha-blended particles
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_renderPipeline);
for (auto& ps : m_effects) {
    if (ps.blendMode() == BlendMode::ADDITIVE) continue;
    // ... draw ...
}

// Second pass: additive particles
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_additivePipeline);
for (auto& ps : m_effects) {
    if (ps.blendMode() != BlendMode::ADDITIVE) continue;
    // ... draw ...
}
```

---

## 11. Feature 9 — GPU Indirect Draw (Alive-List Compaction)

### 11.1 Problem

Currently, `VFXRenderer::render()` draws `maxParticles * 6` vertices per emitter, even if most particles are dead. Dead particles are culled in the vertex shader by placing them at `(0, 0, 2, 1)` (behind clip), but the GPU still invokes the vertex shader for them.

### 11.2 Solution

Add a second compute pass **after** `particle_sim.comp` that counts alive particles and writes a `VkDrawIndirectCommand`:

```glsl
// shaders/particle_compact.comp
#version 450
layout(local_size_x = 64) in;

struct Particle {
    vec4 posLife;
    vec4 velAge;
    vec4 color;
    vec4 params;
};

layout(std430, set=0, binding=0) readonly buffer ParticleIn { Particle particles[]; };

// Draw indirect command buffer
layout(std430, set=0, binding=4) buffer DrawCmd {
    uint vertexCount;    // = aliveCount * 6
    uint instanceCount;  // = 1
    uint firstVertex;    // = 0
    uint firstInstance;  // = 0
};

layout(push_constant) uniform CompactPC {
    uint totalCount;
} pc;

shared uint localAlive;

void main() {
    if (gl_LocalInvocationIndex == 0) localAlive = 0;
    barrier();

    uint i = gl_GlobalInvocationID.x;
    if (i < pc.totalCount && particles[i].params.w >= 0.1)
        atomicAdd(localAlive, 1);

    barrier();
    if (gl_LocalInvocationIndex == 0) {
        atomicAdd(vertexCount, localAlive * 6);
    }
}
```

**Note:** This is a simplified version. A production implementation would also build an alive-index list and use it in the vertex shader for dense packing. The simpler approach above just provides an accurate vertex count for `vkCmdDrawIndirect` while still relying on the vertex shader's dead-particle cull.

### 11.3 Render Change

Replace `vkCmdDraw(maxParticles * 6, 1, 0, 0)` with:
```cpp
vkCmdDrawIndirect(cmd, ps.indirectBuffer(), 0, 1, sizeof(VkDrawIndirectCommand));
```

---

## 12. Render Loop Integration Order

After all features are implemented, `recordCommandBuffer()` should follow this exact order:

```
OUTSIDE ANY RENDER PASS:
  1. VFXRenderer::dispatchCompute(cmd)        — particle simulation
  2. [Optional] particle_compact.comp          — alive count (Feature 9)
  3. VFXRenderer::barrierComputeToGraphics(cmd)

HDR RENDER PASS (R16G16B16A16_SFLOAT + D32_SFLOAT):
  4. Opaque scene geometry (depth write ON)
     - Static meshes (triangle.vert + triangle.frag)
     - Skinned meshes (skinned.vert + triangle.frag)
  5. Ground decals (depth test ON, depth write OFF, alpha blend)
     - GroundDecalRenderer::render()
  6. Procedural VFX geometry (depth test ON, depth write OFF)
     - ConeAbilityRenderer::render()
     - ExplosionRenderer::render()
     - ShieldBubbleRenderer::render()
     - MeshEffectRenderer::render()
  7. Trail ribbons (depth test ON, depth write OFF, additive or alpha)
     - TrailRenderer::render()
  8. GPU particles — alpha blended (depth test ON, depth write OFF)
     - VFXRenderer::render() [alpha subset]
  9. GPU particles — additive (depth test ON, depth write OFF)
     - VFXRenderer::render() [additive subset]
  10. Sprite effects
     - SpriteEffectRenderer::render()
  11. Distortion pass (reads scene color copy)
     - DistortionRenderer::render()
  12. Debug / UI overlays
     - ClickIndicatorRenderer::render()
     - DebugRenderer::render()
     - Grid (if enabled)

END HDR RENDER PASS

BLOOM PASSES (compute or separate render passes):
  13. BloomPass::dispatch(cmd)

SWAPCHAIN RENDER PASS:
  14. ToneMapPass::render(cmd)
  15. ImGui::Render()

END SWAPCHAIN RENDER PASS
```

---

## 13. File Placement Conventions

Follow existing project patterns:

| Content | Directory | Naming |
|---------|-----------|--------|
| C++ headers | `src/<module>/` | `PascalCase.h` |
| C++ sources | `src/<module>/` | `PascalCase.cpp` |
| GLSL shaders | `shaders/` | `snake_case.{vert,frag,comp}` |
| VFX JSON definitions | `assets/vfx/` | `snake_case.json` |
| Ability JSON definitions | `assets/abilities/` | `snake_case.json` |
| Composite VFX JSON | `assets/vfx/composites/` | `snake_case.json` |
| VFX mesh assets | `assets/vfx/meshes/` | `snake_case.glb` |
| Decal textures | `assets/textures/decals/` | `snake_case.png` |
| Trail textures | `assets/textures/trails/` | `snake_case.png` |

All shaders are compiled to SPIR-V with `glslangValidator` or `glslc` and placed alongside the source as `*.spv`. The Makefile already handles this compilation step.

---

## 14. Vulkan Constraints & Safety Rules

### 14.1 Push Constant Size

The Vulkan spec guarantees a minimum of **128 bytes** for push constants. All existing renderers respect this:
- `ConePC`: exactly 128 bytes ✓
- `ShieldPC`: 112 bytes ✓
- `ExplosionPC`: 112 bytes ✓
- `SpriteEffect PushConstants`: 112 bytes ✓
- `VFXRenderer RenderPC`: 96 bytes ✓

**Rule:** Every new push constant struct MUST be ≤128 bytes. Use `static_assert`.

### 14.2 Descriptor Set Budget

`VFXRenderer` pre-allocates `MAX_CONCURRENT_EMITTERS (64)` descriptor sets. New renderers should pre-allocate their own pools sized to their `MAX_INSTANCES` constant with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`.

### 14.3 Memory Barriers

- Compute → Vertex: Use `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT → VK_PIPELINE_STAGE_VERTEX_SHADER_BIT` with `SHADER_WRITE → SHADER_READ`. (Already done in `barrierComputeToGraphics()`.)
- Image layout transitions: When transitioning the HDR image from `COLOR_ATTACHMENT_OPTIMAL` to `SHADER_READ_ONLY_OPTIMAL` for bloom sampling, use `VkImageMemoryBarrier`.

### 14.4 Deferred Deletion

Follow the existing `GRAVEYARD_DELAY = 3` pattern from `VFXRenderer`. Any GPU resource (buffer, image, descriptor set) that might be in-flight must not be destroyed for `MAX_FRAMES_IN_FLIGHT + 1` frames.

### 14.5 VMA Allocation Patterns

- **SSBO (GPU read/write, CPU write for spawn):** `VMA_MEMORY_USAGE_CPU_TO_GPU` (existing pattern)
- **UBO (CPU write per frame):** `VMA_MEMORY_USAGE_CPU_TO_GPU` with persistent mapping
- **Staging textures:** `VMA_MEMORY_USAGE_CPU_ONLY` → copy → `VMA_MEMORY_USAGE_GPU_ONLY`
- **HDR render targets:** `VMA_MEMORY_USAGE_GPU_ONLY`

### 14.6 SPIR-V Compilation

All shaders reference `SHADER_DIR` (set via CMake `add_definitions(-DSHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/")`). SPIR-V files are loaded at runtime via `VFXRenderer::readSPV()` (static method reading `.spv` files).

**Add compilation rules to the Makefile** for every new `.vert`, `.frag`, `.comp` file:
```makefile
shaders/%.vert.spv: shaders/%.vert
	glslangValidator -V $< -o $@
```

---

## 15. Testing & Validation Strategy

### 15.1 Visual Verification (Manual)

For each feature, create a test ability JSON that exercises it:

| Feature | Test Ability | Expected Visual |
|---------|-------------|----------------|
| HDR + Bloom | Fire any existing particle emitter with color > 1.0 | Glow halo around bright particles |
| Trail Renderer | Fire Q skillshot | Glowing ribbon follows projectile path |
| Ground Decal | Cast E (point AoE) | Circle indicator on ground, fades after duration |
| Mesh Effect | Auto-attack | Crescent slash arc mesh sweeps in attack direction |
| Composite Sequencer | Cast R (ultimate) | Multi-layered explosion with staggered timings |
| Distortion | Walk through portal zone | Background behind portal warps/shimmers |
| Soft Particles | Fire particles near terrain | Particles smoothly fade at ground intersection |
| Additive Pipeline | Fire magic particles | Glow intensifies where particles overlap |

### 15.2 Vulkan Validation Layers

Always run with `VK_LAYER_KHRONOS_validation` enabled during development. Check for:
- `VUID-vkCmdDraw-*` errors from incorrect pipeline/descriptor binding
- `VUID-VkImageMemoryBarrier-*` errors from incorrect layout transitions
- `VUID-vkCmdPushConstants-size-*` from push constants exceeding range

### 15.3 Performance Targets

For a MOBA with 10 champions, each using 4 abilities with VFX:
- Target: **≤2ms GPU time** for all VFX combined at 1080p
- Bloom should cost ≤0.5ms (half-res blur)
- Particle compute should scale with alive count, not max count (Feature 9)
- Profile with `VK_EXT_debug_utils` timestamp queries

### 15.4 Memory Budget

| Resource | Budget |
|----------|--------|
| HDR color buffer (1080p) | 1920×1080×8B = ~16 MB |
| Bloom ping-pong (half-res) | 960×540×8B × 2 = ~8 MB |
| Particle SSBOs (64 emitters × 2048 × 64B) | ~8 MB |
| Trail SSBOs (32 trails × 64 points × 32B) | ~64 KB |
| Ground decal textures | ~4 MB (atlas) |
| VFX mesh VBOs | ~2 MB |
| **Total VFX VRAM** | **~38 MB** |

---

## End of Specification

This document provides complete implementation specifications for nine VFX subsystems. Each feature is designed to plug into the existing Glory architecture without modifying working code. Implement in priority order (P0→P8). Every new renderer follows the established pattern: `init(Device&, RenderPass)`, `update(dt)`, `render(cmd, viewProj, ...)`.