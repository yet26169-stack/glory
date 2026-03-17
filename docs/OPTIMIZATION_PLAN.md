# Glory Engine — Performance Optimization Plan

---

## ⚠️ CRITICAL: Minion Wave Spawn Crashes / 0.2 FPS

### Problem

When a minion wave spawns (24 minions), the app drops to **0.2 FPS** (CPU Simulation: 4,507 ms/frame) and eventually crashes. Root cause: all minions use **CPU skinning** (`DynamicMeshComponent`) with high-poly GLB meshes.

- Melee minion: **26,760 vertices**, 65 joints
- Caster minion: **29,663 vertices**, 41 joints
- 24 minions × ~28K verts = **~670K vertices** CPU-skinned + re-uploaded to GPU every frame

Each minion's `DynamicMesh::updateVertices` does `memcpy(44 bytes × vertexCount)` to a `CPU_TO_GPU` staging buffer per frame — totaling **~28 MB/frame** of CPU→GPU transfers.

### Fix Strategy (2 Phases)

#### Phase A: Mesh Decimation at Load Time (2 lines, immediate)

The engine already has `meshopt_simplify` integrated in `GLBLoader.cpp` (lines 620–678). All current minion template loads pass `targetReduction=0.0f` (no decimation). Change to `0.6`:

```cpp
// In Renderer.cpp buildScene(), minion template loading:
// OLD: loader.load("models/melee_minion/melee_minion_walking.glb", 0.0f);
// NEW: loader.load("models/melee_minion/melee_minion_walking.glb", 0.6f);
```

- Reduces 27K → ~10K vertices per minion
- Total: 24 × 10K = 240K verts/frame (3× reduction)
- CPU skinning still ~1,500 ms — not enough alone, but easy quick win

Parameters: `targetIndexCount = max(indices * 0.4, 900)`, `targetError = 0.02f`, minimum 300 triangles enforced.

#### Phase B: GPU Skinning for Minions (~200 lines, full fix)

Switch minions from `DynamicMeshComponent` (CPU) to `GPUSkinnedMeshComponent` (GPU compute shader). This eliminates ALL CPU vertex transforms and per-frame buffer uploads.

**Required changes:**

| File | Change |
|------|--------|
| `Descriptors.h:87` | `MAX_SKINNED_CHARS` 32 → **128** (2 MB/frame bone buffer, acceptable) |
| `Renderer.h` | Add `std::queue<uint32_t> m_freeBoneSlots` + `std::unordered_map<uint32_t,uint32_t> m_entityBoneSlot` for slot pooling |
| `Renderer.cpp` (buildScene) | Pre-fill `m_freeBoneSlots` with slots 0..127. Create shared `StaticSkinnedMesh` per minion type. |
| `Renderer.cpp` (minion spawn) | Replace `DynamicMeshComponent` with `GPUSkinnedMeshComponent`. Allocate bone slot from pool. Allocate per-entity output buffer (44B × vertexCount, GPU_ONLY). Add to `m_computeSkinEntries`. |
| `Renderer.cpp` (minion death) | Return bone slot to `m_freeBoneSlots`. Destroy output buffer. Remove from `m_computeSkinEntries`. |
| `Scene.cpp` | Write bone matrices to ring buffer for GPU-skinned minions via `writeBoneSlot(frameIdx, slot, matrices)` |
| `MinionSystem.cpp` | On minion death callback, release bone slot |

**How GPU skinning works in this engine:**
1. Bind-pose mesh stored in `StaticSkinnedMesh` (GPU_ONLY, uploaded once)
2. Each frame: animation system writes bone matrices to shared ring-buffer SSBO (slot × 256 bones × 64 bytes = 16 KB per entity)
3. `ComputeSkinner` dispatches `skinning.comp` per entity: reads bind-pose + bones → writes pre-skinned output buffer
4. Compute→vertex barrier inserted before scene pass
5. Rendering reads from pre-skinned output buffer (no vertex shader bone work)

**Capacity:** ComputeSkinner supports 256 batches/frame. 24 minions = 9% utilization.

**Expected result:** CPU skinning 4,507 ms → **0 ms**. GPU compute: 24 × ~0.05 ms = **~1.2 ms**. App should run at 60+ FPS.

### Key Constants Reference

| File | Constant | Current | Target | Purpose |
|------|----------|---------|--------|---------|
| `Descriptors.h:87` | `MAX_SKINNED_CHARS` | 32 | 128 | Concurrent skinned entities |
| `ComputeSkinner.h:24` | `COMPUTE_SKIN_THRESHOLD` | 50 | 1 | Always use compute path |
| `CPUSkinning.cpp:17` | Thread threshold | 50000 | N/A | Irrelevant after GPU migration |
| GLBLoader calls | `targetReduction` | 0.0f | 0.6f | Minion mesh decimation |

---

## Current State (updated)

### Completed optimizations
- [x] **Phase 1** — 11 FPS → 60+ FPS
  - Mesh decimation via meshoptimizer (492K → ~10K verts, 97% reduction)
  - Skip 200+ demo entities in MOBA mode
  - Light-frustum culling on shadow pass
  - SSAO, Bloom, GPU particles all skipped in MOBA mode
  - Minimal post-process (tone mapping + FXAA only)
  - Shadow frustum follows camera target; directional light added
  - Shader early-outs for disabled effects (chromatic aberration, bloom, etc.)
- [x] **Phase 2** — Scalable Architecture
  - GPU vertex shader skinning (`skinned.vert`, bone SSBO at binding 4)
  - Static device-local GPU mesh (`StaticSkinnedMesh`) — uploaded once, never per-frame
  - Ring-buffer bone SSBO (32 slots × 256 bones, `boneBaseIndex` push constant per entity)
  - Character skinned LOD (3 levels: 10K / 3K / 700 tris at 20m / 60m / ∞)
  - `SkinnedLODComponent` for GPU-skinned mesh LOD selection
  - Fixed-timestep simulation (30 Hz) decoupled from render frame
  - GPU timestamp query profiler (`GpuProfiler`) — Shadow/Scene/Post/ImGui pass timings shown in overlay
  - **Post-process specialization constants** — two pipeline variants compiled at startup:
    - `m_pipeline`: all 13 effects enabled (bloom, SSAO, chroma, god rays, DoF, auto-exp, heat, outline, sharpen, grain, dither, FXAA, tone map)
    - `m_mobaPipeline`: only FXAA + ACES tone map compiled in; all other branches eliminated by driver at pipeline creation
  - **CPU profiler integrated** — `Profiler` scopes on `Simulation` + `CmdRecord`, timings shown in overlay alongside GPU times
  - **Particle distance LOD** — emission rate scales quadratically from full at 30m to zero at 80m; `setCameraPosition()` called each frame
  - **Shadow frustum tightened to camera view** — `computeLightVP()` transforms 8 NDC corners through inverse camera VP, takes AABB in light space → exact fit, no wasted shadow map texels outside viewport

---

## Remaining Work

### Phase 2 (continued)
- [x] **SSAO + Bloom allocation guarded by `!m_mobaMode`** — 1×1 dummy images
  bound to descriptors when passes are skipped; `m_bloom`/`m_ssao` unique_ptrs
  never created in MOBA mode (saves ~60MB GPU memory + 3 render passes)

### Phase 3 — Production Scale
- [x] **GPU frustum culling compute shader** (`shaders/cull.comp`) + `GpuCuller`
  class: runs before scene pass, populates `VkDrawIndexedIndirectCommand[]` via
  atomic counter; one invocation per mesh object; `vkCmdDrawIndexedIndirectCount`
  ready; dispatched per-frame with CPU-side AABB + draw-arg upload
- [x] **Cascade Shadow Maps (CSM)** — `CascadeShadow` class: 3 cascades
  (10m/40m/120m splits), 2048×2048 depth array image; per-cascade frustum fit
  with bounding-sphere + texel-snap to prevent edge shimmer; front-face culling
  + depth bias in pipeline; activated in MOBA mode; subpass dependencies handle
  layout transitions without explicit barriers
- [x] **Dedicated transfer queue** — `Device::findQueueFamilies()` finds a
  TRANSFER-only queue family (AMD DMA engine / NVIDIA CE) when present;
  falls back to graphics family if unavailable; `Buffer.cpp` + `Texture.cpp`
  submit staging uploads to `getTransferQueue()` not `getGraphicsQueue()`;
  `multiDrawIndirect` device feature enabled; `VkPhysicalDeviceVulkan12Features`
  chain added with `drawIndirectCount = VK_TRUE`

### Phase 3 (remaining)
- [x] **Terrain CDLOD** — Multi-LOD index buffer generated at startup (3 levels: stride 1/2/4).
  Per-chunk LOD selected at render time based on horizontal camera distance
  (LOD0 ≤ 40m, LOD1 ≤ 80m, LOD2 beyond). Same vertex buffer reused across all
  LODs — only indices differ. ~75% index reduction at max range.
  `TERRAIN_LOD_RANGES[3]` constants in `TerrainSystem.h`.
- [x] **Async texture streaming** — `TextureStreamer` class: background worker
  thread decodes images with stb_image, allocates staging buffer, uploads via
  dedicated transfer queue, generates full mip chain with `vkCmdBlitImage`,
  waits on `VkFence` polled each frame in `tick()`. Ready callback fires on main
  thread to update bindless descriptor slot. Zero graphics-queue stalls.
- [x] **Compute skinning** — `ComputeSkinner` class + `skinning.comp` shader:
  activated when > 50 `GPUSkinnedMeshComponent` entities are visible. Pre-skins
  all characters in a single compute pass (local_size=64) into persistent
  device-local output buffers (STORAGE | VERTEX). Compute→vertex barrier inserted
  before scene pass. Falls back to vertex-shader bone SSBO path below threshold.
  One device-local output buffer allocated per character in `buildScene()`.
- [x] **GPU-driven draw merging** — `GpuCuller` extended with binding 3 (instance
  output SSBO). Compute shader writes per-surviving-instance data (model matrix +
  tint) in parallel with draw commands. `firstInstance` in each draw command maps
  directly to the instance buffer row. `instanceBuffer(frameIdx)` accessor
  exposes the VkBuffer for binding as vertex buffer at slot 1.
  All four outputs (cull + indirect draw + instance data + count) produced in one
  compute dispatch with zero CPU indirect buffer writes.

---

## Performance budget reference (1080p, 60 FPS target)

| System                  | Budget   | Current cost (estimated) |
|-------------------------|----------|--------------------------|
| Shadow pass             | 1.5 ms   | ~0.5 ms (1 char + terrain) |
| Scene geometry          | 3.0 ms   | ~1.0 ms (MOBA mode)      |
| Post-process            | 0.5 ms   | ~0.3 ms (tone map + FXAA)|
| CPU simulation (30 Hz)  | 2.0 ms   | ~0.3 ms                  |
| CPU draw recording      | 1.0 ms   | ~0.5 ms                  |
| **Total frame budget**  | **16.6ms** | **~3 ms (MOBA mode)**  |

GPU timings are now visible in the debug overlay (F1).

---

## Triangle budgets (full MOBA match target)

| Asset              | Triangles | Count | Total       |
|--------------------|-----------|-------|-------------|
| Player Champion    | 10,000    | 1     | 10,000      |
| Other Champions    | 5,000     | 9     | 45,000      |
| Minions            | 500       | 60    | 30,000      |
| Towers             | 3,000     | 11    | 33,000      |
| Terrain            | 65,000    | 1     | 65,000      |
| Trees/Rocks/Deco   | 200       | 500   | 100,000     |
| Particles/FX       | 50        | 200   | 10,000      |
| **TOTAL**          |           |       | **~300K**   |

Current single character: ~10K tris (LOD0). Was 244K tris — fixed.


## Current State: 11 FPS with 1 character on map

### Root Cause Analysis

After deep analysis of the entire codebase, here are the **exact bottlenecks**
ranked from most to least impactful:

---

## 🔴 CRITICAL — Fix These First (Expected: 11 FPS → 60+ FPS)

### 1. Character Mesh is Absurdly Over-Tessellated
**File:** `better_scientist.glb`
**Impact:** This single issue likely accounts for 70%+ of your frame time.

The character model has **492,054 vertices** and **732,930 indices**. For
reference, a League of Legends champion has ~5,000–15,000 triangles. Your
single character has **244,310 triangles** — roughly **20x more than a typical
MOBA character**, and **7.5x more vertices than your entire terrain** (65,536).

This model gets processed THREE times per frame:
1. **CPU Skinning** — transforms all 492K vertices on the CPU every frame
2. **Shadow pass** — renders all 732K indices to the shadow map
3. **Scene pass** — renders all 732K indices again for the main view

**Fix:** Decimate the model. A MOBA-style character at isometric camera distance
needs at most **5,000–15,000 triangles**. Use Blender's Decimate modifier:
```
Blender → Select mesh → Modifier → Decimate → Ratio: 0.03 (target ~15K tris)
```
Or use `meshoptimizer` / Simplygon at build time.

### 2. CPU Skinning of 492K Vertices Every Frame
**File:** `src/animation/CPUSkinning.cpp`
**Impact:** ~40ms+ per frame on CPU alone

You're doing **per-vertex matrix blending on the CPU** for half a million
vertices, then uploading the result to GPU via `DynamicMesh::updateVertices()`.
Even with the multithreading you've added, this is:
- 492,054 vertices × 4 bone influences × matrix multiply = ~8 million float ops
- Then a 492,054 × 44 bytes = **21.6 MB memcpy** to the GPU every frame

**Fix (immediate):** Decimate the model (see #1). A 10K vertex mesh skins in <0.1ms.

**Fix (proper — what AAA/MOBA games do):** Move skinning to GPU compute shader:
```glsl
// skinning.comp — GPU compute skinning
layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer BindPose { Vertex bindVerts[]; };
layout(std430, binding = 1) readonly buffer SkinData { SkinVertex skinData[]; };
layout(std430, binding = 2) readonly buffer Matrices { mat4 skinMats[]; };
layout(std430, binding = 3) writeonly buffer Output  { Vertex outVerts[]; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    // ... same logic but massively parallel on GPU
}
```
This eliminates the CPU→GPU upload entirely. The output buffer is already on GPU.

### 3. Shadow Pass Renders ALL Entities (No Culling)
**File:** `src/renderer/Renderer.cpp`, lines ~1093–1120
**Impact:** Rendering 200 rocks + character + all demo objects to shadow map

The shadow pass iterates **every entity with MeshComponent** and renders it with
zero frustum culling against the light frustum:
```cpp
auto renderView = m_scene.getRegistry().view<TransformComponent, MeshComponent>();
for (auto entity : renderView) {
    // No culling — draws everything
    m_scene.getMesh(...).draw(cmd);
}
```

**Fix:** Add light-frustum culling to the shadow pass. Most entities are outside
the shadow camera's view. Also skip tiny/distant objects.

---

## 🟡 IMPORTANT — Next Tier Optimizations

### 4. Redundant Demo Scene Objects Still Created in MOBA Mode
**File:** `src/renderer/Renderer.cpp`, `buildScene()`

Even in MOBA mode, you create 200 rocks, cubes, spheres, torus, torus knot,
spring, gear, cone, cylinder, capsule, icosphere, floor terrain, and light
gizmos — **218 entities total** (matches the debug overlay count). These all:
- Consume memory for mesh/texture GPU resources
- Get iterated in shadow pass (rendered to shadow map)
- Get iterated in scene update (rotation/orbit animations)

In MOBA mode, only the character, projectiles, terrain, and map entities matter.

**Fix:** Don't create demo objects when `m_mobaMode` is true, or tag them and
skip them in MOBA render paths.

### 5. Post-Process Shader is a Kitchen Sink
**File:** `shaders/postprocess.frag` (324 lines)

The post-process fragment shader runs on **every pixel** and includes:
- Chromatic aberration (3 texture fetches)
- Heat distortion (3 more fetches when enabled)
- SSAO application
- Bloom compositing
- God rays (up to **64 texture samples** in a loop!)
- Depth of field (9 texture samples per pixel)
- Auto-exposure (16 texture samples)
- FXAA (5+ texture samples + tone mapping per sample)
- Sharpening (5 texture samples + tone mapping per sample)
- Sobel edge detection (9 texture samples)
- Film grain
- Vignette
- Color grading (saturation + temperature)
- Dithering

Even with many effects disabled, the shader compiler may not optimize away
the branching well on all drivers.

**Fix:**
- Use **shader permutations** (compile separate SPIR-V variants for active effects)
- Or use **specialization constants** to compile out disabled features at pipeline creation
- Disable effects not needed for MOBA: god rays, heat distortion, DoF, film grain, chromatic aberration
- The FXAA and Sharpen implementations re-tone-map neighbor samples — very wasteful. Use a proper single-pass FXAA on the LDR output instead.

### 6. SSAO Runs at Half Resolution but Still Every Frame
**Files:** `src/renderer/SSAO.cpp`, `shaders/ssao.frag`

SSAO does 16 hemisphere samples per pixel at half resolution, plus a blur pass.
For a MOBA with mostly top-down terrain view, SSAO adds minimal visual value.

**Fix:** Disable SSAO in MOBA mode or reduce to quarter resolution with 8 samples.

### 7. Bloom is Two Fullscreen Passes (Extract + Blur)
**File:** `src/renderer/Bloom.cpp`

Bloom extracts bright pixels then does horizontal + vertical Gaussian blur.
Three additional fullscreen passes.

**Fix:** Acceptable cost, but consider skipping bloom in MOBA mode or using
a single-pass dual-filter approach.

---

## 🟢 ARCHITECTURE — How AAA/MOBA Games Handle This at Scale

### What League of Legends / Dota 2 Actually Do:

#### A. GPU Skinning (Not CPU)
Every modern game skins on the GPU via compute shaders or vertex shader bone
transforms. The skeleton typically has 30-60 bones, and the vertex shader does:
```glsl
vec4 skinPos = vec4(0);
for (int i = 0; i < 4; i++) {
    skinPos += boneMatrices[joints[i]] * vec4(position, 1.0) * weights[i];
}
```
The bind-pose mesh stays in a static GPU buffer. Only the ~60 bone matrices
(60 × 64 bytes = 3.8 KB) are uploaded per character per frame.

**Contrast with your engine:** You upload **21.6 MB** of vertex data per character per frame.

#### B. LOD System for Characters
MOBA characters typically have 3-4 LOD levels:
- **LOD0:** ~10K tris (close-up, recall base, champion spotlight)
- **LOD1:** ~5K tris (normal gameplay distance)
- **LOD2:** ~2K tris (far away / minimap edge)
- **LOD3:** ~500 tris (very far, barely visible)

You already have LOD for rocks — extend it to characters.

#### C. Draw Call Batching / Instancing
You already have indirect drawing and instancing infrastructure — good.
The key optimization is to avoid per-entity descriptor set binds and use
bindless textures (which you already do). Keep this pattern.

#### D. Frustum Culling on EVERYTHING
Your scene pass does frustum culling (good), but your shadow pass doesn't.
Also, the MOBA mode should have tighter frustum culling since the isometric
camera has a fixed, known view volume.

#### E. Render Pass Structure for a MOBA
Efficient MOBA rendering pipeline:
```
1. Shadow Map        — Light-frustum culled, only nearby characters + terrain
2. Main Scene        — Frustum culled, instanced, GPU-skinned characters
3. Particles/FX      — Additive blended, simple shaders
4. Post-Process      — Minimal: tone mapping + FXAA only (no DoF, no god rays)
```

Your current pipeline:
```
1. Shadow Map        — ALL entities, no culling, CPU-skinned character
2. GPU Particles     — Compute dispatch
3. Main Scene        — Sky + Terrain + Water + frustum-culled entities + CPU-skinned character
4. SSAO              — Half-res, 16 samples + blur
5. Bloom             — Extract + 2-pass blur
6. Post-Process      — Tone map + FXAA + sharpen + vignette + chromatic aberration + ...
7. ImGui Overlay
```

That's **7 passes** where a MOBA needs **3-4**.

#### F. Terrain Optimization
Your terrain is 256×256 = 65K vertices drawn in 64 chunks with frustum culling.
This is reasonable but could be improved:
- **Clipmap/CDLOD:** Only render terrain at full resolution near the camera,
  coarser further away. Terrain LOD is essential when the map scales up.
- **GPU tessellation:** Use hull/domain shaders with a simple heightmap texture.

#### G. Texture Streaming
Your `buildScene()` creates 13 procedural textures + loads GLB textures all
at startup. For a full MOBA with 100+ champion skins, abilities, and map
textures, you need on-demand streaming:
- Load textures asynchronously on a transfer queue
- Use mipmap bias to show low-res first, swap in high-res
- Evict unused textures from GPU memory

---

## 📋 Implementation Priority Order

### Phase 1: Get to 60 FPS (1-2 hours)
1. **Decimate `better_scientist.glb` to ~10K triangles** (Blender Decimate modifier)
2. **Skip demo entities in MOBA mode** (guard `buildScene` with `if (!m_mobaMode)`)
3. **Add light-frustum culling to shadow pass**
4. **Disable SSAO and Bloom in MOBA mode**

### Phase 2: Scalable Architecture (1-2 days)
5. **Implement GPU skinning** via compute shader
6. **Add character LOD** (3 mesh variants per character)
7. **Shader specialization constants** for post-process permutations
8. **Consolidate render passes** (merge where possible)

### Phase 3: Production Scale (1-2 weeks)
9. **Terrain CDLOD** for large map support
10. **Async texture streaming** with transfer queue
11. **Occlusion culling** (Hi-Z or GPU-driven)
12. **GPU-driven rendering** (indirect dispatch based on GPU culling results)
13. **Multi-draw indirect** to batch all geometry into 1-2 draw calls

---

## Quick Reference: Triangle Budgets (MOBA at 1080p, 60 FPS)

| Asset              | Triangles | Count | Total       |
|--------------------|-----------|-------|-------------|
| Player Champion    | 10,000    | 1     | 10,000      |
| Other Champions    | 5,000     | 9     | 45,000      |
| Minions            | 500       | 60    | 30,000      |
| Towers             | 3,000     | 11    | 33,000      |
| Terrain            | 65,000    | 1     | 65,000      |
| Trees/Rocks/Deco   | 200       | 500   | 100,000     |
| Particles/FX       | 50        | 200   | 10,000      |
| **TOTAL**          |           |       | **~300K**   |

Your **single character** is currently **732K indices = ~244K triangles**.
That's nearly the ENTIRE triangle budget for a full MOBA scene.


On mesh decimation (#1)
The plan says "Decimate in Blender at ratio 0.03." This works for a quick test, but automated decimation at 97% reduction will destroy UV seams, joint weighting, and silhouette quality. For a real fix:

Retopologize manually (or use InstantMeshes/Quad Remesher) to get a clean 8–12K tri mesh with proper edge loops around joints and the face. Then re-bake normals from the high-poly onto the low-poly. This is what every MOBA studio actually does — the "high poly" model is the sculpt, and the game mesh is a separate, hand-made low-poly.
If you want an automated pipeline for many characters, meshoptimizer's meshopt_simplify is far better than Blender's Decimate modifier. It preserves UV boundaries and can lock vertices on bone boundaries. Simplygon is the industry standard but costs money.
Don't forget to also simplify the skeleton. If your GLB has 100+ bones (common from Mixamo or full-body mocap rigs), merge leaf bones and reduce to 30–50. Fewer bones = faster skinning and smaller uniform uploads.

On GPU skinning (#2) — the plan undersells this
The compute shader approach shown is correct, but there's a simpler intermediate step the plan doesn't mention: vertex shader skinning. You don't need a compute pass at all for a small number of characters. Just store the bind-pose mesh in a static VkBuffer, upload bone matrices as a UBO/SSBO, and do the skin in your vertex shader:
glsl// In your existing vertex shader
layout(set = 1, binding = 0) uniform BoneData {
    mat4 bones[MAX_BONES];
};

void main() {
    mat4 skin = bones[joints.x] * weights.x
              + bones[joints.y] * weights.y
              + bones[joints.z] * weights.z
              + bones[joints.w] * weights.w;
    vec4 worldPos = model * skin * vec4(position, 1.0);
    // ...
}
This is trivial to implement in your existing pipeline (no new compute pass, no new synchronization), and it's what most games with < 50 skinned characters on screen actually do. Reserve compute skinning for when you have 100+ skinned entities (large minion waves, spectator mode with all champions visible).
On shadow pass culling (#3) — expand this significantly
Light-frustum culling is the minimum. For a MOBA you should also:

Use a smaller shadow map region. Your directional light shadow map probably covers the whole scene. For isometric MOBA, fit the shadow frustum tightly around the camera view frustum (cascade shadow maps, or a single tight frustum since your view is fixed). This dramatically reduces what's "in" the light frustum.
Shadow caching / dirty flagging. Static geometry (terrain, rocks, towers) doesn't move. Render them to the shadow map once, and only re-render when the light direction changes or a dynamic object enters their shadow region. Many MOBAs use baked lightmaps for static shadows entirely and only render character shadows dynamically.
Simple character shadows. LoL doesn't even use shadow-mapped shadows for champions at default settings — it uses a projected circle/blob shadow. Consider this as a low-quality option. It's nearly free.

On demo entities (#4) — correct but incomplete
Beyond not creating them, audit your ECS iteration patterns. If your view<TransformComponent, MeshComponent>() iterates all entities, even entities that aren't rendered still cost iteration time. Use archetype tags or separate ECS groups for "renderable in MOBA" vs. "demo only." EnTT (if that's what you're using) supports groups and sorting that can make this zero-cost.
On post-process (#5) — the plan is right but I'd be more aggressive
For a MOBA at 1080p/60fps, your post-process should be:

Tone mapping (ACES or similar, one sample)
FXAA or TAA (TAA is better for thin geometry like minion health bars and particle edges)
That's it for the default quality setting

No god rays, no DoF, no chromatic aberration, no film grain, no sharpening, no SSAO, no bloom. These are "Ultra" settings that 90% of MOBA players disable anyway. The plan suggests "shader permutations" — yes, but more importantly, just don't run the passes at all. Don't dispatch the bloom compute/draw if bloom is off. Branching in the uber-shader is the wrong level to solve this; skip the entire render pass.
Major missing items
1. Memory and descriptor management for scale. The plan doesn't discuss VkDescriptorPool sizing, VkDeviceMemory allocation strategy, or buffer sub-allocation. When you go from 1 character to 10 champions + 60 minions + projectiles, naive per-entity allocations will kill you. You need:

A ring buffer allocator for per-frame dynamic data (bone matrices, per-object uniforms)
VMA (Vulkan Memory Allocator) if you aren't already using it
Descriptor indexing (you mention bindless textures — good, extend this to all resources)

2. Frame pipelining / double/triple buffering. Are you using frames-in-flight properly? With Vulkan you should have 2–3 frames in flight so the CPU can record frame N+1 while the GPU executes frame N. If you're doing vkQueueSubmit → vkWaitForFences synchronously each frame, you're serializing CPU and GPU and leaving performance on the table.
3. Transfer queue for asset loading. The plan mentions async texture streaming in Phase 3, but you should set up a dedicated transfer queue early. Vulkan exposes separate transfer queues on most GPUs. Use one for all buffer/image uploads so your graphics queue never stalls on asset loading. This is essential before you start loading champion select screens, ability VFX, etc.
4. Particle system budget. The plan ignores particles entirely. MOBA ability VFX can easily generate thousands of particles. You need a particle budget (e.g., 10K particles max on screen), GPU-driven particle simulation (you mention you have compute particles — good), and a LOD system for particles (reduce emission rate at distance).
5. Network-aware rendering. MOBAs are networked games. Your render loop needs to handle interpolation, prediction, and variable tick rates without coupling to frame rate. If your game logic runs in lockstep with rendering, you'll get gameplay stutters whenever you drop frames. Decouple simulation tick (e.g., 30Hz fixed) from render (variable, vsync or uncapped).
6. Profiling infrastructure. Before optimizing further, instrument your engine:

VK_EXT_debug_utils markers around each render pass so GPU profilers (RenderDoc, Nsight) can label them
Timestamp queries (vkCmdWriteTimestamp) at the start/end of each pass to get per-pass GPU time
CPU frame timers around each system (skinning, culling, draw call recording, present)

Without this, you're guessing at where time goes after the initial obvious fixes.
Revised priority order
Phase 0: Measure (30 minutes)

Add GPU timestamp queries per render pass and CPU timers per system. You need numbers before and after each change.

Phase 1: Get to 60 FPS (a few hours)

Decimate the character to ~10K tris (meshoptimizer or Blender, rebake normals)
Move skinning to the vertex shader (not compute — simpler, sufficient for < 50 characters)
Guard buildScene demo objects behind if (!m_mobaMode)
Fit shadow frustum tightly to camera view; skip shadow pass for static-only frames or use blob shadows
Disable SSAO, Bloom, and all non-essential post-process passes (not just branch over them — skip the draw/dispatch)

Phase 2: Scale to full match (days)
6. Ring buffer allocator for per-frame dynamic data
7. Proper frames-in-flight (2–3) if not already done
8. Dedicated transfer queue for async loads
9. Character LOD (3 levels)
10. Particle budget and LOD system
11. Decouple simulation tick from render frame
Phase 3: Production (weeks)
12. Compute skinning (when character count demands it)
13. Terrain CDLOD
14. Cascade shadow maps with static shadow caching
15. GPU-driven culling and multi-draw indirect
16. Shader permutation system for post-process
17. Texture streaming with mip bias
The plan is fundamentally sound. The single biggest win — decimating that model and moving skinning off the CPU — will likely take you from 11 FPS to well over 100 FPS on its own. Everything else is about scaling to a full 10-player match with minions, VFX, and a large map.