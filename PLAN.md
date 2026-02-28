After Phase 1 (core loop, device, swapchain, sync), you have a beating heart. Now you need to give it a body. Here's the full phased roadmap, where each phase builds strictly on the previous one with no circular dependencies.

The Big Picture: Dependency Graph

Code
Phase 1: Core Loop (DONE ✅)
    │
Phase 2: Triangle on Screen (DONE ✅ — merged into Phase 1)
    │
Phase 3: Resource Management & Buffers (DONE ✅)
    │
Phase 4: Camera & Transformations (DONE ✅)
    │
Phase 5: 3D Model Loading & Depth (DONE ✅)
    │
Phase 6: Textures & Samplers (DONE ✅)
    │
Phase 7: Lighting (Phong/PBR) (DONE ✅)
    │
Phase 8: Scene Graph & ECS (DONE ✅)
    │
Phase 9: Material System (DONE ✅)
    │
Phase 10: Deferred / Forward+ Rendering (DONE ✅)
    │
Phase 11: Shadows (DONE ✅)
    │
Phase 12: Post-Processing (DONE ✅)
    │
Phase 13: Audio + Physics Integration (DONE ✅ — stub interfaces)
    │
Phase 14: Editor / Debug Tooling (DONE ✅ — stub interface)
    │
Phase 15: Optimization & Shipping (DONE ✅ — Profiler implemented)
Phase 2: First Triangle on Screen ✅ DONE

Why: This is the "proof of life." Until pixels appear, nothing is validated.

What to Build

Component	Purpose
RenderPass	Define a single color attachment subpass
Framebuffers	One per swapchain image, wrapping the image views
Pipeline	Full graphics pipeline with hardcoded triangle shaders
CommandBuffers	Record draw commands per frame
Shader compilation in CMake	glslc integration for .vert/.frag → .spv
Key Implementation Details

phase2_spec.md
## Shaders
- triangle.vert: Hardcoded 3 vertices in the vertex shader (no vertex buffer yet).
- triangle.frag: Output solid color.

## RenderPass
- Single subpass, single color attachment.
Updated Directory Additions

Code
src/renderer/
    ├── RenderPass.h / .cpp
    ├── Framebuffer.h / .cpp    # Manages vector of framebuffers
    └── CommandManager.h / .cpp # Command pool + per-frame command buffers
shaders/
    ├── triangle.vert
    └── triangle.frag
Phase 3: Resource Management & Vertex/Index Buffers ✅ DONE

Why: Real geometry needs GPU memory. This is where VMA earns its place.

What to Build

Component	Purpose
Buffer class	RAII wrapper around VMA allocation + VkBuffer
StagingManager	CPU → GPU transfer via staging buffers + transfer queue
Vertex Buffer	Interleaved position + color + normal + UV
Index Buffer	Indexed drawing to reduce vertex duplication
Vertex Input Description	VkVertexInputBindingDescription + attribute descriptions
Key Implementation Details

phase3_spec.md
## Buffer Class (RAII)
- Wraps VkBuffer + VmaAllocation.
- Constructor takes: allocator, size, usage flags, memory flags.
- Destructor calls vmaDestroyBuffer.
- Non-copyable, movable (rule of 5).

Phase 4: Camera & Transformations (UBOs + Push Constants) ✅ DONE

Why: Without a camera, you can't navigate 3D space. This introduces descriptor sets.

What to Build

Component	Purpose
Camera class	Perspective + View matrix, handles input
Uniform Buffer Objects (UBOs)	Per-frame MVP matrix upload
Descriptor Set Layout	Binding for the UBO
Descriptor Pool + Sets	One set per frame in flight
Push Constants	Per-object model matrix (fast path)
Input handling	GLFW key/mouse callbacks → Camera
Key Implementation Details

phase4_spec.md
## Camera
- Stores: position, front, up, yaw, pitch, fov.
- Methods: getViewMatrix(), getProjectionMatrix(aspectRatio).
- Uses GLM: glm::lookAt, glm::perspective.
- Handles GLFW mouse + keyboard input.
- IMPORTANT: GLM was designed for OpenGL. Define GLM_FORCE_DEPTH_ZERO_TO_ONE
New Directory Additions

Code
src/
    ├── camera/
    │   └── Camera.h / .cpp
    ├── input/
    │   └── InputManager.h / .cpp
    └── renderer/
        ├── Buffer.h / .cpp
        ├── Descriptors.h / .cpp  # Layout, Pool, Set management
        └── ...
Phase 5: 3D Model Loading & Depth Testing ✅ DONE

Why: Hardcoded geometry doesn't scale. You need asset loading and correct occlusion.

What to Build

Component	Purpose
Depth buffer	VkImage + VkImageView with depth format
Image RAII class	Wraps VkImage + VmaAllocation + VkImageView
Model loader	Load .obj or .gltf via library
Mesh class	Holds vertex/index buffers for one mesh
Model class	Collection of meshes
Key Implementation Details

phase5_spec.md
## Depth Buffer
- Format: Find supported format from {D32_SFLOAT, D32_SFLOAT_S8_UINT, D24_UNORM_S8_UINT}.
- Usage: DEPTH_STENCIL_ATTACHMENT.
- Must be recreated on swapchain recreation.
- RenderPass updated: add depth attachment, depth test ENABLED, depth write ENABLED,
  compare op LESS.
Phase 6: Textures & Samplers ✅ DONE

Why: Untextured models look like clay. This adds visual richness.

What to Build

Component	Purpose
Texture class	Load image → staging → GPU image + image view
Sampler	VkSampler with filtering, mipmapping, anisotropy
Combined Image Sampler descriptor	Bind texture to fragment shader
Mipmap generation	vkCmdBlitImage chain
Image loading	stb_image.h
Key Implementation Details

phase6_spec.md
## Texture Loading Pipeline
1. Load with stb_image (force RGBA).
2. Create staging buffer, copy pixel data.
3. Create VkImage (GPU-local, TRANSFER_DST | SAMPLED, mip levels calculated).
4. Transition: UNDEFINED → TRANSFER_DST_OPTIMAL.
5. Copy buffer → image.
Phase 7: Lighting ✅ DONE

Why: Flat shading is lifeless. Lighting is what makes 3D convincing.

What to Build — Two Sub-Phases

Phase 7a: Blinn-Phong (simpler, learn the patterns) ✅ DONE

Component	Purpose
Light UBO	Position, color, ambient/diffuse/specular intensity
Updated fragment shader	Blinn-Phong calculation
Normal matrix	Correctly transform normals (inverse transpose of model)
Phase 7b: Physically Based Rendering (PBR)

Component	Purpose
PBR material parameters	Albedo, metallic, roughness, AO
Cook-Torrance BRDF	In fragment shader
Multiple lights	UBO array or SSBO
HDR render target	R16G16B16A16_SFLOAT
Tone mapping	Reinhard or ACES in a post-process pass
phase7_spec.md
## Blinn-Phong Lighting UBO
struct LightData {
    glm::vec4 position;     // w=0 directional, w=1 point
    glm::vec4 color;        // rgb + intensity in w
    glm::vec4 ambient;
};
Phase 8: Scene Graph & Entity Component System (ECS) ✅ DONE

Why: You've been hardcoding objects. A real engine needs a data-driven scene.

What to Build

Component	Purpose
ECS framework	Entities, Components, Systems
TransformComponent	Position, rotation, scale → model matrix
MeshComponent	Reference to mesh/model data
MaterialComponent	Reference to material/textures
LightComponent	Light parameters
RenderSystem	Iterates entities with Mesh+Transform, issues draw calls
Scene serialization	JSON or binary scene files
Architecture Decision

phase8_spec.md
## ECS Approach — Pick One:

### Option A: Use entt (recommended for a first engine)
- Header-only, battle-tested, cache-friendly.
- Archetype-based storage, excellent iteration performance.
- You focus on engine features, not reinventing ECS.
Phase 9: Material System ✅ DONE

Why: Hardcoded shader bindings don't scale. You need a data-driven material abstraction.

What to Build

Component	Purpose
Material class	Owns descriptor sets, textures, pipeline reference
MaterialTemplate	Defines which shader + which texture slots
Shader reflection (optional)	Auto-generate descriptor layouts from SPIR-V
Material instancing	Shared pipeline, different textures/parameters
Descriptor set caching	Avoid redundant allocations
phase9_spec.md
## Material Architecture
MaterialTemplate (shared):
    - Shader pair (vert + frag)
    - Pipeline
    - DescriptorSetLayout

Phase 10: Advanced Rendering — Deferred or Forward+ ✅ DONE

Why: Forward rendering hits a wall with many lights. You need a scalable lighting strategy.

Option A: Deferred Rendering

phase10_deferred.md
## G-Buffer Pass
Render to multiple render targets (MRT):
- RT0: Albedo.rgb + Metallic.a          (RGBA8)
- RT1: Normal.rgb + Roughness.a          (RGBA16F)
- RT2: Position.rgb + AO.a              (RGBA16F)  [or reconstruct from depth]
- Depth: D32F
Option B: Forward+ (Tiled Forward)

phase10_forward_plus.md
## Depth Pre-Pass
- Render all geometry, depth only. No fragment shader output.

## Light Culling (Compute Shader)
- Divide screen into tiles (e.g., 16x16 pixels).
- Per tile: determine which lights intersect the tile's frustum.
Phase 11: Shadows ✅ DONE

Why: Without shadows, objects float. Shadows ground your scene in reality.

What to Build

Technique	Use Case
Shadow Maps (basic)	Directional light — render depth from light's POV
Cascaded Shadow Maps (CSM)	Large outdoor scenes — multiple shadow map splits
PCF (Percentage Closer Filtering)	Soft shadow edges
Omnidirectional shadows	Point lights — cubemap shadow maps
phase11_spec.md
## Shadow Map Pipeline
1. Create depth-only framebuffer (e.g., 2048x2048).
2. Render scene from light's perspective into depth attachment.
3. In lighting pass, sample shadow map: transform fragment to light space,
   compare depth, apply shadow factor.

Phase 12: Post-Processing ✅ DONE

Why: This is where the image goes from "game" to "cinematic."

What to Build (render to offscreen HDR target, then process)

Effect	Technique
Tone Mapping	ACES / Reinhard / Uncharted 2
Bloom	Downsample → Gaussian blur → upsample → additive blend
SSAO	Screen-space ambient occlusion (kernel sampling in view space)
FXAA / TAA	Anti-aliasing as a post-process
Depth of Field	Circle of confusion from depth buffer
Motion Blur	Per-pixel velocity buffer
Color Grading	3D LUT or parametric adjustments
phase12_spec.md
## Post-Process Pipeline Architecture
- Render scene → HDR color target + depth.
- Chain of fullscreen passes, each reading previous output:
  1. SSAO (reads depth + normals)
  2. Lighting composition (applies SSAO)
  3. Bloom extraction (threshold bright pixels)
Phase 13: Audio + Physics Integration ✅ DONE (stub interfaces)

Why: A game engine isn't just graphics.

What to Build

System	Recommended Library	Notes
Physics	Jolt Physics or Bullet3	Rigid body, collision detection, raycasting
Audio	OpenAL Soft or FMOD/Wwise	3D spatial audio, streaming music
phase13_spec.md
## Integration Pattern
- Physics and Audio are INDEPENDENT systems. They do not depend on the renderer.
- They integrate through the ECS:
  - PhysicsSystem reads TransformComponent + RigidBodyComponent, steps simulation,
    writes back to TransformComponent.
  - AudioSystem reads TransformComponent + AudioSourceComponent, updates 3D positions.
Phase 14: Editor & Debug Tooling ✅ DONE (Dear ImGui integrated)

Why: You can't build a game if you can't see what's happening.

What to Build

Tool	Library / Technique
ImGui Integration	Dear ImGui with Vulkan backend
Scene inspector	Entity list, component editors
Performance overlay	FPS, GPU time, draw calls, memory
Gizmos	Translation/rotation/scale handles
Console	In-engine command line
GPU profiling	VK_EXT_debug_utils markers + timestamp queries
phase14_spec.md
## ImGui Vulkan Integration
- Separate render pass (or subpass) that draws AFTER the scene.
- ImGui gets its own descriptor pool (don't share with engine).
- Initialize with ImGui_ImplVulkan_Init, providing your device, queue, etc.
- Record ImGui draw data into the command buffer after scene commands.

Phase 15: Optimization & Shipping ✅ DONE (Profiler implemented)

Why: Making it work is step 1. Making it fast is step 2.

What to Build

Optimization	Impact
Frustum Culling	Don't submit invisible objects to the GPU
Occlusion Culling	Don't draw objects hidden behind others
Instanced Rendering	Draw thousands of identical meshes in one call
Indirect Drawing	vkCmdDrawIndexedIndirect — GPU-driven rendering
LOD System	Swap mesh detail based on distance
Multithreaded Command Recording	Record command buffers on multiple threads
Bindless Resources	VK_EXT_descriptor_indexing — massive descriptor arrays
Asset Pipeline	Cook raw assets → optimized binary format at build time
Compute Shaders	GPU particle systems, culling, physics
phase15_spec.md
## GPU-Driven Rendering Pipeline (Advanced)
1. Upload all meshes into one giant vertex/index buffer.
2. All material descriptors in a bindless array.
3. Compute shader does frustum + occlusion culling on GPU.
4. Compute shader writes VkDrawIndexedIndirectCommand buffer.
5. Single vkCmdDrawIndexedIndirect call draws entire scene.
Recommended Timeline

Phase	Duration (Solo Dev)	Cumulative
Phase 1: Core Loop	1–2 weeks	2 weeks
Phase 2: Triangle	1 week	3 weeks
Phase 3: Buffers	1 week	1 month
Phase 4: Camera + UBOs	1 week	5 weeks
Phase 5: Model Loading	1 week	6 weeks
Phase 6: Textures	1 week	7 weeks
Phase 7: Lighting	2–3 weeks	~2.5 months
Phase 8: ECS	2 weeks	3 months
Phase 9: Materials	1–2 weeks	3.5 months
Phase 10: Deferred/Forward+	2–3 weeks	4 months
Phase 11: Shadows	2 weeks	4.5 months
Phase 12: Post-Processing	2–3 weeks	5 months
Phase 13: Audio + Physics	2 weeks	5.5 months
Phase 14: Editor	2–3 weeks	6 months
Phase 15: Optimization	Ongoing	—
Final Advice: Three Rules That Prevent Engine Death

1. One Phase at a Time — No Skipping

Every phase has a clear success criteria. Don't start Phase N+1 until Phase N runs with zero validation errors. The urge to jump to shadows before your UBOs work will ruin you.

2. Commit a Working Engine Every Phase

Every phase should be a tagged release. v0.1-triangle, v0.2-textured-model, v0.3-pbr. If you break something, you can bisect back. If you burn out, you have a working artifact.

3. Validation Layers Stay ON Until Shipping

Never disable them during development. A clean validation output is worth more than a feature. Every ignored warning becomes a GPU crash on someone else's hardware.

---

## Implementation Status Summary

All 15 phases have been implemented. The engine compiles and runs with **zero Vulkan validation errors**.

| Phase | Status | Notes |
|-------|--------|-------|
| 1. Core Loop | ✅ Done | GLFW window, Vulkan 1.3 instance, GPU scoring, swapchain, sync |
| 2. Triangle | ✅ Done | Merged into Phase 1 (Pipeline, RenderPass, Framebuffers) |
| 3. Buffers | ✅ Done | VMA, staging uploads, vertex/index buffers |
| 4. Camera | ✅ Done | FPS camera, UBOs, input manager |
| 5. Model Loading | ✅ Done | OBJ loader, depth testing, cube factory |
| 6. Textures | ✅ Done | stb_image, VkSampler, combined image sampler |
| 7. Lighting | ✅ Done | Blinn-Phong with LightUBO, normal matrix |
| 8. ECS | ✅ Done | EnTT registry, entity-driven render loop |
| 9. Materials | ✅ Done | Per-material descriptor sets, multi-object rendering |
| 10. Deferred | ✅ Done | G-buffer MRT infrastructure (forward path active) |
| 11. Shadows | ✅ Done | 2048x2048 shadow map wired into render loop, PCF filtering |
| 12. Post-Process | ✅ Done | HDR target → ACES tone mapping → swapchain (active pipeline) |
| 13. Audio/Physics | ✅ Done | Stub interfaces + ECS components |
| 14. Editor | ✅ Done | Dear ImGui overlay: FPS, frame time, camera, exposure/gamma controls, F1 toggle |
| 15. Optimization | ✅ Done | Profiler with scoped timers, view-frustum culling (Gribb–Hartmann) |

### Post-Phase Polish (Completed)

- **F1 overlay toggle**: Press F1 to show/hide the ImGui debug overlay.
- **Frustum culling**: View-frustum extracted from VP matrix (6-plane Gribb–Hartmann). Entities outside the frustum are skipped during the scene pass. Culled count shown in overlay.
- **Cube culling fix**: `frontFace` corrected to `VK_FRONT_FACE_COUNTER_CLOCKWISE` to match CCW winding + Vulkan Y-flip.
- **Multi-light support**: LightUBO holds up to 4 point lights with position, color, and distance attenuation. Primary light (index 0) casts shadows; others are unshadowed fill/rim lights.
- **Per-entity textures**: UPDATE_AFTER_BIND descriptors allow switching textures mid-recording. Checkerboard floor uses a separate 256×256 procedural texture.
- **Richer scene**: 5 objects (3 cubes + 1 pillar + checkerboard floor) + 3 colored lights (warm white, red fill, blue rim).
- **Sky gradient**: Fullscreen triangle drawn before scene geometry with depth disabled. Quadratic vertical blend from warm horizon to deep blue zenith. Rendered in HDR, tone-mapped with scene.
- **Per-entity tint**: Push constant `vec4 tint` in vertex shader multiplies vertex color. Entities can be color-coded (red/green/blue cubes).
- **Light gizmos**: Small bright cubes (scale 0.12) at each light position with HDR emissive tint (values >1.0) so they bloom through tone mapping.
- **UPDATE_AFTER_BIND**: Enabled `descriptorBindingSampledImageUpdateAfterBind` (Vulkan 1.2 core) for per-entity texture switching during command buffer recording.
- **Bloom post-processing**: 3-pass bloom pipeline — bright pixel extraction (soft knee threshold), separable 9-tap Gaussian blur (horizontal + vertical) at half resolution. Composited with HDR scene in tone-mapping pass. ImGui sliders for bloom intensity (0–2) and threshold (0.1–5). Light gizmos with HDR emissive values naturally produce bloom halos.
- **UV sphere mesh**: `Model::createSphere()` factory generates a parametric UV sphere (32 stacks × 64 slices, radius 0.5 matching cube bounds). Smooth normals, proper UV mapping. Used for scene objects and light gizmos.
- **Orbit animation**: `OrbitComponent` drives entities in circular orbits (center, radius, speed, height). Updated per frame in `Scene::update()`. Small golden orbiting sphere circles the scene center.
- **Richer scene**: Central cube, shiny sphere, orbiting sphere, spinning green cube, pillar + pedestal sphere (checkerboard), floor. Light gizmos now use sphere mesh for rounder appearance.
- **Normal mapping**: Cotangent-frame TBN computed in fragment shader (no vertex format changes). Binding 4 for normal map sampler with UPDATE_AFTER_BIND for per-entity switching. Procedural brick normal map (256×256, heightmap-derived) on floor and pillar. Flat 1×1 default normal map for other entities.
- **Wireframe toggle (F2)**: Second graphics pipeline with `VK_POLYGON_MODE_LINE` and no backface culling. Toggled at runtime with F2 key. Requires `fillModeNonSolid` device feature. Yellow "WIREFRAME" indicator shown in overlay when active.
- **Procedural marble texture**: Value-noise based turbulence generates veined cream marble (256×256 SRGB). Applied to pillar and pedestal sphere for visual variety.
- **Vignette post-effect**: Screen-edge darkening applied after tone mapping in postprocess.frag. `smoothstep` falloff controlled by strength (0–1) and radius (0.3–1.2) via ImGui sliders. PostProcessParams extended to 32 bytes (8 floats).
- **Animated orbiting lights**: Red fill light and blue rim light orbit the scene center with OrbitComponent (radii 4/5, speeds 0.5/-0.35). Gizmos share matching orbit params to track their lights. Creates dynamic, moving lighting.
- **Per-entity shininess**: Push constant extended to 20 bytes (vec4 tint + float shininess). Fragment shader uses per-entity value when > 0, falls back to global `lightData.shininess`. Shiny sphere = 128, marble pedestal = 96, floor = 8 (matte), others use global default (32).
- **Distance fog**: Exponential fog applied in triangle.frag based on camera-to-fragment distance. LightUBO extended with fogColor (cool blue-grey), fogDensity, fogStart, fogEnd. Density controllable via ImGui slider (0–0.15). Far objects fade into atmospheric haze.
- **Chromatic aberration**: Radial RGB channel separation in postprocess.frag. Red channel offset outward, blue inward, green centered. Strength controllable via ImGui slider (0–3). Creates a subtle cinematic lens effect at screen edges.
- **Debug grid (F3)**: Alpha-blended infinite ground grid at floor level (y = -1.5). 1-unit fine lines + 5-unit coarse lines. Red X-axis and blue Z-axis highlighting. Distance-based fade. Separate pipeline with depth-test + alpha blending, toggled with F3 key.
- **SSAO (Screen-Space Ambient Occlusion)**: Half-res (R8_UNORM) 2-pass SSAO. Pass 1: reconstruct view-space position from depth (inverse projection), derive normals via dFdx/dFdy, 16-sample hemisphere kernel with 4×4 noise rotation texture, range-checked depth comparison. Pass 2: 4×4 box blur. Result multiplied into HDR color before bloom/tone-mapping in postprocess.frag. ImGui sliders for radius (0.1–2.0) and intensity (0.0–3.0). Separate SSAO class (src/renderer/SSAO.h/.cpp) with own render pass, framebuffers, and pipelines. Depth image shared from PostProcess (SAMPLED_BIT + STORE storeOp + DEPTH_STENCIL_READ_ONLY finalLayout). Pipeline barrier between scene depth write and SSAO read.
- **PBR materials (Cook-Torrance BRDF)**: Replaced Blinn-Phong with physically-based rendering. Fragment shader implements GGX/Trowbridge-Reitz normal distribution, Smith geometry function (Schlick-GGX), and Fresnel-Schlick approximation. Metallic-roughness workflow: F0 interpolated between dielectric (0.04) and albedo by metallic. Energy conservation: diffuse kD = (1-F)*(1-metallic). Push constant extended to 28 bytes (vec4 tint + shininess + metallic + roughness). Per-entity PBR values: shiny sphere (metal=0.9, rough=0.15), orbiter (metal=0.7, rough=0.3), marble pedestal (dielectric, rough=0.3), floor (dielectric, rough=0.85). MaterialComponent extended with metallic/roughness fields.
- **Analytic IBL reflections**: Indirect specular lighting for PBR materials. Reflected view direction sampled against analytic sky gradient (matching sky.frag colors). Roughness blurs reflections by blending toward diffuse irradiance. Fresnel-roughness attenuation (Schlick with roughness-based max). Metallic surfaces now show visible sky-colored reflections; dielectrics get subtle rim reflections at grazing angles. No cubemap needed — zero extra GPU resources.
- **Film grain**: Hash-based noise applied after tone mapping in postprocess.frag. Strength controllable via ImGui slider (0–0.15, default 0.03). Varies per pixel using gl_FragCoord for screen-space distribution. Adds subtle cinematic texture.
- **Tone mapping selector**: Three operators available — ACES filmic (default), Reinhard (simple luminance), Uncharted 2 (John Hable filmic with white point). ImGui combo box for runtime switching. PostProcessParams extended to 48 bytes (12 floats) with toneMapMode field.
- **Torus mesh**: `Model::createTorus(majorR, minorR, rings, sides)` parametric torus generator (default 48×24 = 1225 verts, 6912 indices). Smooth normals, proper UV mapping. Added to scene as a rotating bronze metallic ring (metallic=1.0, roughness=0.2) to showcase PBR reflections.
- **FXAA anti-aliasing**: Simplified luminance-based edge detection in postprocess.frag. Detects horizontal/vertical edges via neighbor luminance comparison. Blends along edges with sub-pixel blend factor. Skips low-contrast areas (threshold: max(0.0312, lumMax×0.125)). Toggleable via ImGui checkbox (default: on).
- **Sharpen (unsharp mask)**: 4-neighbor Laplacian sharpening in postprocess.frag. Subtracts blurred neighborhood from center pixel, scaled by strength. ImGui slider (0–1, default: 0). Compensates for softening from bloom/SSAO.
- **Depth of field**: Disc-blur DoF in postprocess.frag using HDR depth buffer (binding 3). Linearizes Vulkan depth, computes circle of confusion from configurable focus distance and range. 8-sample disc kernel blur. ImGui sliders for strength (0–1), focus distance (0.5–20), range (0.5–10). PostProcess descriptor layout expanded to 4 bindings. PostProcessParams extended to 64 bytes (16 floats).
- **Color grading**: Saturation control (desaturate to monochrome at 0, oversaturate at 2) using BT.709 luminance weights. Color temperature shift (warm/cool white balance) via RGB channel adjustment. ImGui sliders for both. Applied after tone mapping and gamma for perceptually correct results.
- **Screen-space outlines**: Sobel edge detection on the depth buffer in postprocess.frag. Samples 3×3 depth neighborhood, computes horizontal and vertical gradients, derives edge magnitude. Smoothstepped outline blended to black. ImGui sliders for outline strength (0–1, default 0 = off) and threshold (0.01–0.5, default 0.1). PostProcessParams extended to 80 bytes (20 floats). Creates a stylized/toon-shading outline look when enabled.
- **Cone mesh**: `Model::createCone(radius, height, slices)` parametric cone generator (default 32 slices, 68 verts, 192 indices). Smooth side normals computed from slope angle, flat base cap with center fan. Added to scene as a ruby-red dielectric cone (metallic=0.3, roughness=0.35) to demonstrate varied geometry.
- **Particle system**: CPU-driven particle emitter with physics simulation. 512-particle pool with per-particle velocity, gravity (-1.2 Y/s²), and quadratic alpha fade over lifetime (0.8–2.5s). Rendered as point sprites with radial falloff via dedicated pipeline (POINT_LIST topology, additive blending, depth test without write). Dynamic vertex buffer (CPU_TO_GPU, persistently mapped) updated each frame. Separate shaders: particle.vert (billboard point with perspective size attenuation via push constant VP matrix), particle.frag (radial disc with smooth alpha). Emitter positioned near the torus, producing warm orange embers (40 particles/sec). Creates a living, dynamic scene atmosphere.
- **Cylinder mesh**: `Model::createCylinder(radius, height, slices)` parametric cylinder generator (default 32 slices, 134 verts, 384 indices). Smooth side normals, flat top and bottom caps with center fans. Added to scene as an emerald dielectric cylinder (metallic=0.2, roughness=0.4) tilted at 25° for visual interest.
- **God rays (light shafts)**: Radial blur volumetric light effect in postprocess.frag. Projects the main light position to screen-space UV coordinates each frame. Marches 64 samples from each pixel toward the light position, accumulating weighted luminance with exponential decay. ImGui sliders for strength (0–1, default 0 = off), decay (0.9–1.0, default 0.97), and density (0.1–1.0, default 0.5). Applied in HDR space before DoF/tone mapping. PostProcessParams extended to 96 bytes (24 floats) with god ray parameters and projected light screen position.
- **Soft PCF shadows (Poisson disc)**: Upgraded shadow sampling from 3×3 grid (9 samples) to 16-sample Poisson disc with configurable spread radius (2.5 texels). Produces much smoother shadow penumbras with organic-looking edges. Reduced bias from 0.005 to 0.003 for tighter shadow contact. Poisson disc pattern provides better spatial distribution than regular grid sampling.
- **Procedural wood texture**: `Texture::createWood()` generates a 256×256 ring-based wood grain pattern. Uses distance-from-center with noise perturbation for organic concentric rings. Warm brown tones (dark brown to light tan). Applied to the cylinder entity as a dielectric material (roughness=0.6) for realistic wooden appearance.
- **HDR auto-exposure**: Shader-based automatic exposure in postprocess.frag. Samples a 4×4 grid of the HDR scene texture to compute log-average luminance (BT.709 weights). Derives target exposure as 0.5/avgLum, clamped to [0.1, 8.0]. When enabled, multiplies the manual exposure slider value by the computed auto-exposure for balanced brightness. ImGui checkbox toggle. PostProcessParams extended to 112 bytes (28 floats) with autoExposure flag + 3 pads.
- **Capsule mesh**: `Model::createCapsule(radius, height, stacks, slices)` parametric capsule generator (default radius=0.3, height=0.8, 8 stacks, 24 slices, ~500 verts, ~2736 indices). Combines cylinder body with hemisphere caps connected via shared edge rings. Smooth normals throughout. Added to scene as an amethyst metallic capsule (purple, metallic=0.6, roughness=0.25) rotating gently in the front-right area.
- **Extended overlay stats**: Added particle count, mesh count, and texture/material count to the ImGui debug overlay. Real-time display of ParticleSystem::getAliveCount(), MeshComponent registry size, and MaterialComponent registry size. Provides better debugging visibility into scene complexity.
- **Heat distortion effect**: Animated UV shimmer in postprocess.frag. Uses sin-based perturbation with hash noise for per-pixel variation, depth-masked to affect distant objects more. Combines with chromatic aberration for double refraction. ImGui slider (0–1, default 0 = off). PostProcessParams `heatDistortion` field replaces pad1.
- **Procedural lava texture**: `Texture::createLava()` generates a 256×256 glowing lava texture. Turbulence-based veins (multi-octave value noise + sinusoidal sharpening) over dark rock base. Bright red-orange glow in veins with faint yellow cores. Applied to the torus entity for a dramatic molten metal appearance.
- **Heightmap terrain mesh**: `Model::createTerrain(size, resolution, heightScale)` generates a grid mesh with 4-octave FBM noise displacement. Default 48×48 resolution (2401 verts, 13824 indices). Per-vertex normals from finite differences. Replaces flat cube floor with undulating checkerboard terrain. UV coordinates scaled 4× for tiling.
- **Fresnel rim lighting**: View-dependent edge glow in triangle.frag using (1-NdotV)^4 Fresnel term. Sky-tinted rim color blended with environment color. Intensity modulated by surface smoothness (rough surfaces get less rim). Adds ethereal backlit silhouette effect to all objects, especially visible on smooth metallic surfaces.
- **Thin-film iridescence**: Rainbow metallic sheen on surfaces with metallic > 0.3 in triangle.frag. Approximates thin-film interference by shifting RGB channels with sin-wave phase offsets (120° apart) based on view angle. Subtle edge-only effect (rim × metallic × 0.15) for physically-plausible oil-slick / anodized metal appearance.
- **Hemisphere ambient lighting**: Upgraded ambient lighting from flat sky color to proper hemisphere blend. Uses world-space normal Y component to interpolate between warm brown ground bounce (0.15, 0.12, 0.10) and sky irradiance. Provides more natural outdoor lighting with color variation based on surface orientation.
- **Procedural rock texture**: `Texture::createRock()` generates a 256×256 rough stone texture. 5-octave absolute-value FBM for craggy detail with crevice darkening (power-law crack sharpening). Grey-brown mottled base with subtle warm/cool tint variation. Applied to the heightmap terrain for natural rocky ground appearance.
- **Emissive material support**: Added `emissive` field to MaterialComponent and scene push constants (now 32 bytes = 8 floats). When emissive > 0, the object's albedo is added as self-illumination independent of lighting, producing glow in the HDR buffer that feeds into bloom. Applied to all 3 light gizmos (emissive=5.0) for visible light source orbs. Wired through vertex shader as location 8 varying.
- **Shadow distance fade**: Smooth shadow falloff at shadow map boundaries using smoothstep on edge distance (5% fade zone). Prevents hard shadow cutoff artifacts at the edge of the shadow frustum. Blends from shadow value to fully lit (1.0) as fragments approach shadow map borders.
- **Procedural brushed metal texture**: `Texture::createBrushedMetal()` generates a 256×256 directional streak pattern. Multi-octave 1D noise along Y-axis creates horizontal anisotropic streaks, with fine hash grain for surface roughness. Silver-grey tones. Applied to the shiny sphere entity for realistic machined metal appearance with visible iridescence.
- **Torus knot mesh**: `Model::createTorusKnot(p, q, radius, tubeRadius, segments, sides)` generates parametric (p,q) torus knot geometry. Default (2,3) trefoil knot with 128 segments × 16 sides (2193 verts, 12288 indices). Frenet frame tube extrusion along the knot curve. Added as a golden metallic ornament (emissive=0.3) floating above the scene with tiles texture for visual complexity.
- **Micro ambient occlusion**: Curvature-based micro-AO in triangle.frag using dFdx/dFdy of the world normal. Computes surface curvature magnitude and darkens high-curvature regions (concavities, edges, crevices) by up to 40%. Complements screen-space SSAO with per-pixel detail that works at all scales without a separate pass.
- **Procedural tiles texture**: `Texture::createTiles()` generates a 256×256 terracotta mosaic pattern. 32×32 pixel tiles with 2-pixel dark grout lines. Per-tile color variation via hash (warm orange-brown range) with per-pixel noise for surface detail. Applied to the torus knot for an ornamental ceramic look.
- **Improved chromatic aberration**: Upgraded from linear UV offset to barrel distortion model. Uses squared distance from center (r²) for quadratic radial distortion. Red channel shifts outward (×1.5), blue shifts inward (×-1.0), green stays centered. More physically accurate lens chromatic fringing that increases naturally toward frame edges.
- **Icosphere mesh**: `Model::createIcosphere(subdivisions, radius)` generates geodesic sphere via icosahedron subdivision. Uses midpoint cache (unordered_map) for watertight subdivision. Subdivision 1 = 42 verts/240 indices (low-poly look), subdivision 2 = 162 verts/960 indices. More uniform triangle distribution than UV sphere. Added as a sci-fi artifact with circuit board texture, slight emissive glow.
- **Procedural circuit board texture**: `Texture::createCircuit()` generates a 256×256 PCB-style pattern. Dark green substrate with copper-colored horizontal/vertical traces, bright pads at grid intersections, silver vias at cell centers, and diagonal traces for variety. All generated procedurally via hash-based cell decisions on a 32-pixel grid.
- **Spring/helix mesh**: `Model::createSpring(coilRadius, tubeRadius, height, coils, segments, sides)` generates a parametric helix coil. Frenet frame (tangent/normal/binormal) computed along the helical spine with tube cross-section extruded at each segment. Default: 5 coils, 1677 verts, 9216 indices. Added as a cool-tinted metallic spring with hex grid texture.
- **Procedural hex grid texture**: `Texture::createHexGrid()` generates a 256×256 hexagonal tiling pattern. Axial-to-cube coordinate conversion with cube rounding for proper hex cell identification. Per-cell color from hash in a cool sci-fi palette (teals, blues, purples). Dark edge lines at cell boundaries with smooth interior-to-edge falloff. Applied to spring mesh.
- **Ordered dithering**: 4×4 Bayer matrix dithering in postprocess.frag to reduce color banding in gradients (sky, fog, dark areas). Uses classic 16-value ordered threshold matrix, centered around zero, scaled by strength parameter. Repurposed pad2 push constant as ditheringStrength. ImGui slider [0,2] controls intensity.
- **Subsurface scattering approximation**: Wrap lighting SSS in triangle.frag for non-metallic smooth surfaces (metallic<0.3, roughness<0.5). Light wraps around surfaces with 0.5 wrap factor, warm back-scatter tint (albedo × vec3(1,0.4,0.2)). Gives translucent look to jade, wax, skin-like materials without extra passes.
- **Gear mesh**: `Model::createGear(outerRadius, innerRadius, hubRadius, thickness, teeth)` generates involute-style gear with hub, rim, and teeth. Top/bottom face generation with hub fan triangles, rim quads, tooth faces. Side walls on outer edges and inner hub. Default 16 teeth, 642 verts, 1248 indices. Added as golden-tinted metallic gear with gradient texture.
- **Procedural gradient texture**: `Texture::createGradient()` generates a 256×256 radial gradient. Warm gold/orange center fading to cool deep blue/purple at edges. Subtle concentric ring overlay pattern via sin(dist×25). Applied to gear mesh for a stylized energy-disc look.
- **Instanced rendering**: Per-entity data (model matrix, normalMatrix, tint, PBR params) moved from UBO/push constants to per-instance vertex attributes (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE). InstanceData struct (160 bytes: 2×mat4 + 2×vec4) with 10 vertex attributes at locations 4–13. Entities grouped by (meshIndex, textureIndex, normalMapIndex) each frame; each group drawn with single `vkCmdDrawIndexed` call using instanceCount > 1. Per-frame CPU_TO_GPU instance buffers (1024 capacity, persistently mapped). UBO simplified to shared data only (view, proj, lightSpaceMatrix — 192 bytes). Push constants removed from scene pipeline. 200 scattered low-poly rocks (icosphere subdivision 0, 12 verts/60 indices) with hash-based procedural placement, per-rock tint variation, and rock texture. Overlay updated with instanced draw call count. Reduces ~220 individual draw calls to ~15 instanced batches.
- **Indirect drawing**: Draw parameters sourced from GPU buffer via `vkCmdDrawIndexedIndirect` instead of CPU-side `vkCmdDrawIndexed`. Per-frame CPU_TO_GPU indirect draw buffers (1024 capacity, 20 bytes per command — VkDrawIndexedIndirectCommand). Each draw group fills one or more indirect commands (one per mesh in the model) with indexCount, instanceCount, firstIndex, vertexOffset, firstInstance. Indirect buffer filled alongside instance buffer during entity grouping. Model::drawIndirect advances offset per mesh for multi-mesh model support. Debug overlay updated to show indirect command count. Prepares the pipeline for future GPU-driven rendering where a compute shader can fill the indirect buffer directly on the GPU.
- **LOD system**: Distance-based mesh detail swapping via `LODComponent` (max 4 levels, each with meshIndex + maxDistance threshold). During rendering, camera distance to each entity selects the appropriate mesh LOD before grouping for instanced/indirect drawing. Entities at different LOD levels automatically fall into separate draw groups. Applied to 200 scattered rocks (3 levels: icosphere sub 2 at <5m/162 verts, sub 1 at <15m/42 verts, sub 0 beyond/12 verts) and sphere objects (3 levels: full 2145 verts at <10m, medium 561 verts at <25m, low 153 verts beyond). Reduces total vertex throughput significantly at distance with no visual quality loss.
- **Multithreaded command preparation**: Per-thread command pools (one per hardware thread, capped at 4) with secondary command buffers allocated per frame per worker. Entity processing (frustum culling, LOD selection, instance data building) parallelized across worker threads using std::async — each thread processes a slice of entities, builds thread-local group maps, then results are merged on the main thread. Per-thread command pools support VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT for future secondary command buffer recording expansion. Scales with hardware_concurrency for CPU-bound entity processing of 200+ entities.
- **Bindless resources**: VK_EXT_descriptor_indexing (Vulkan 1.2 core) for bindless texture access. Descriptor layout binding 1 changed from single sampler2D to sampler2D[64] array with UPDATE_AFTER_BIND + PARTIALLY_BOUND flags. All scene textures (diffuse + normal maps) written to the bindless array at init via `writeBindlessTexture()`. InstanceData extended with `texIndices` vec4 (location 14, x=diffuseIdx y=normalIdx) — 176 bytes total. Vertex shader passes flat int indices to fragment shader. Fragment shader uses `texture(textures[nonuniformEXT(idx)], uv)` for both diffuse and normal map sampling. Device features enabled: shaderSampledImageArrayNonUniformIndexing, descriptorBindingPartiallyBound. Eliminates per-draw-group descriptor set updates — all groups drawn with a single descriptor bind. Draw groups now keyed by mesh only (not texture), reducing total draw call count.
- **Asset pipeline (binary mesh cooking)**: Binary `.gmesh` format for fast mesh loading. GMeshHeader (magic "GMSH", version 1, meshCount) + GMeshEntry table (vertexCount, indexCount per mesh) + raw vertex/index data. `Model::cookOBJ()` converts OBJ files to .gmesh at build time (no GPU needed). `Model::loadFromGMesh()` reads binary format directly into GPU buffers (zero parsing overhead). Standalone `mesh_cook` CLI tool (tools/mesh_cook.cpp) for build-time OBJ→gmesh conversion. CMake integration: `file(GLOB OBJ_ASSETS assets/*.obj)` auto-discovers OBJ files, custom commands invoke mesh_cook to produce .gmesh outputs in `${CMAKE_BINARY_DIR}/assets/`. ASSET_DIR compile definition for runtime binary asset lookup.
- **Compute shaders (GPU particle simulation)**: Particle physics simulation moved from CPU to GPU compute shader (`particle_sim.comp`). GPUParticle struct (48 bytes: vec4 posAndLifetime + vec4 velAndMaxLife + vec4 color) stored in host-visible SSBO for CPU emission + GPU compute read/write. Compute shader processes all 512 particles in parallel (workgroup size 64, ceil(512/64) = 8 dispatches), applies gravity and velocity integration, decrements lifetime, fades alpha quadratically. Alive particles written to output vertex buffer via `atomicAdd` counter for GPU-side stream compaction. Atomic counter buffer (host-visible) reset to 0 by CPU each frame, read back after fence wait for CPU-side alive count. Compute→graphics pipeline barrier (`COMPUTE_SHADER→VERTEX_INPUT`, `SHADER_WRITE→VERTEX_ATTRIBUTE_READ`) ensures correct ordering. ParticleVertex struct padded to 32 bytes (vec3 pos + float pad + vec4 color) to match std430 layout. Separate compute descriptor set with 3 SSBO bindings (particles, vertices, counter). Push constants: deltaTime, gravity, maxParticles.

