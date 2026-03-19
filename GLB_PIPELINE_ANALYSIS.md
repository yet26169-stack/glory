# GLORY VULKAN ENGINE: GLB MODEL LOADING & TEXTURE PIPELINE ANALYSIS

## EXECUTIVE SUMMARY
The engine uses tinygltf for GLB loading. Textures are embedded in GLB files, extracted on load, registered in a bindless descriptor array (4096 samplers), and referenced via uint indices in the GpuObjectData SSBO. The pipeline is functionally complete but has potential issues with texture lookup ordering and missing texture fallback handling.

---

## 1. GLB LOADER / MODEL LOADING

### File: `src/renderer/GLBLoader.cpp` (1000+ lines)

#### Function: `Model Model::loadFromGLB()`
**Location:** GLBLoader.cpp, lines 58-222  
**Signature:**
```cpp
static Model Model::loadFromGLB(const Device &device, VmaAllocator allocator,
                                const std::string &filepath)
```

**Return Value:**
- Returns a `Model` object containing loaded meshes
- **Does NOT embed textures** — only vertex data (POSITION, NORMAL, TEXCOORD_0), indices, and mesh material indices
- Textures must be loaded separately via `loadGLBTextures()`

**Implementation Details:**
1. Uses `tinygltf::TinyGLTF::LoadBinaryFromFile()` to load the binary GLB file
2. Iterates through `gltfModel.meshes[].primitives[]`
3. **For each primitive:**
   - Extracts POSITION (float3) - required
   - Extracts NORMAL (float3) - optional, defaults to (0, 1, 0) if missing
   - Extracts TEXCOORD_0 (float2) - optional, defaults to (0, 0)
   - Supports multiple UV component types: FLOAT, UNSIGNED_SHORT, UNSIGNED_BYTE (unpacks via decodeUV helper)
   - Handles indices (uint8, uint16, uint32)
   - **Computes smooth normals if GLB has none** (area-weighted face normal accumulation)

4. **Stores mesh material index:** `model.m_meshMaterialIndices.push_back(prim.material)`
   - Records which glTF material each primitive belonged to (for later texture lookup)

**Vertex Structure:**
```cpp
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;      // Always set to white (1, 1, 1)
    glm::vec3 normal;
    glm::vec2 texCoord;
};
```

**Exception Handling:**
- Throws `std::runtime_error` if file load fails
- Throws if model contains no renderable meshes
- Warns if primitives lack POSITION attribute

---

#### Function: `std::vector<GLBTexture> Model::loadGLBTextures()`
**Location:** GLBLoader.cpp, lines 225-292  
**Signature:**
```cpp
static std::vector<Model::GLBTexture> Model::loadGLBTextures(
    const Device &device, const std::string &filepath)
```

**Return Value:**
- Returns a `std::vector<GLBTexture>` containing extracted Texture objects
- **One entry per material with a base-color texture**
- **Order is NOT necessarily the same as primitives**; uses glTF material index

**GLBTexture Structure:**
```cpp
struct GLBTexture {
    int materialIndex;     // glTF material index (0-based)
    Texture texture;       // Loaded and GPU-allocated Texture object
};
```

**Implementation:**
1. Loads GLB and iterates through `gltfModel.materials[]`
2. For each material, extracts `mat.pbrMetallicRoughness.baseColorTexture.index`
3. If texture index is valid:
   - Retrieves `gltfModel.images[tex.source]`
   - Unpacks pixel data (already decoded by tinygltf via stb_image)
   - **Expands RGB → RGBA if needed** (adds alpha=255)
   - Creates `Texture` via `Texture::createFromPixels(device, pixels, w, h)`
   - Pushes `GLBTexture{materialIndex, texture}` to result vector
4. **Returns empty vector if no textures found**
5. **Logs all extracted textures with dimensions and component count**

**Current Issues:**
- ⚠️ **No normal maps or roughness/metallic textures extracted** — only base color
- ⚠️ **Caller must match texture to material index manually** — no automatic pairing

---

#### Function: `Model Model::getGLBBounds()`
**Location:** GLBLoader.cpp, lines 861-917

**Signature:**
```cpp
static AABB Model::getGLBBounds(const std::string &filepath)
```

**Returns:** Axis-aligned bounding box of all positions in the GLB (CPU-side, no GPU needed)

---

#### Function: `Model Model::getGLBRawMesh()`
**Location:** GLBLoader.cpp, lines 919-970

**Signature:**
```cpp
static RawMeshData Model::getGLBRawMesh(const std::string &filepath)
```

**Returns:**
```cpp
struct RawMeshData {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t>  indices;
};
```
Raw vertex positions and indices (CPU-side only, for physics/pathfinding)

---

#### Function: `Model Model::createTerrain()`
**Location:** Model.cpp, lines 630-720

**Signature:**
```cpp
static Model Model::createTerrain(const Device &device, VmaAllocator allocator,
                                   float size = 10.0f, uint32_t resolution = 64,
                                   float heightScale = 1.5f)
```

**Returns:** Procedurally generated flat terrain mesh with Perlin noise heights

**Details:**
- Generates `(resolution+1)² vertices` in an XZ grid
- **Heights computed via FBM (fractional Brownian motion) with 4 octaves**
- Computes smooth normals via finite differences
- Used for ground plane (200×200 flat, Y=0) and lane tiles (1×1 quads)
- **Currently used as fallback for lane tiles in buildScene()**

---

## 2. MATERIALCOMPONENT

### File: `src/scene/Components.h`, lines 61-68

```cpp
struct MaterialComponent {
    uint32_t materialIndex = 0;    // Diffuse texture index (in bindless array)
    uint32_t normalMapIndex = 0;   // Normal map texture index (0 = flat)
    float shininess = 0.0f;        // Legacy Blinn-Phong shine (0 = use global default)
    float metallic = 0.0f;         // 0 = dielectric, 1 = metal
    float roughness = 0.5f;        // 0.04–1.0 (clamped in shader)
    float emissive = 0.0f;         // 0 = no glow, >0 = self-illumination strength
};
```

**Observations:**
- ✅ **Single-texture design** (only diffuse + normal, no multi-layer)
- ✅ **Indices are uint32** (fits bindless array addressing)
- ⚠️ **Metallic is used as sentinel:** `metallic < -0.5` triggers terrain splat blending in shader
- ⚠️ **No texture for metallic/roughness/normal** — these are per-entity constants

---

## 3. TEXTURE SYSTEM

### File: `src/renderer/Texture.h`

```cpp
class Texture {
public:
    Texture() = default;
    Texture(const Device &device, const std::string &filepath);
    Texture(Image &&image);
    
    static Texture createDefault(const Device &device);
    static Texture createFromPixels(const Device &device, const uint32_t *pixels,
                                    uint32_t width, uint32_t height,
                                    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);
    
    VkImageView getImageView() const { return m_image.getImageView(); }
    VkImage     getImage()     const { return m_image.getImage(); }
    VkSampler   getSampler()   const { return m_sampler; }
};
```

#### `Texture::createDefault()`
**Location:** Texture.cpp

**Returns:** 1×1 white texture (RGBA = {255, 255, 255, 255})

**Used as:**
- Fallback when no texture found in GLB
- Base texture for procedural terrain (e.g., ground plane)

#### `Texture::createFromPixels()`
Used by `loadGLBTextures()` to create GPU textures from embedded image data.

**Format:** VK_FORMAT_R8G8B8A8_SRGB (linear RGB space for color, gamma-corrected on read)

---

## 4. SCENE / BINDLESS SYSTEM

### File: `src/scene/Scene.h` + `Scene.cpp`

```cpp
class Scene {
public:
    uint32_t addMesh(Model model);
    uint32_t addTexture(Texture texture);
    uint32_t addMaterial(Material material);
    
    Model&    getMesh(uint32_t index);
    Texture&  getTexture(uint32_t index);
    Material& getMaterial(uint32_t index);
    
private:
    std::vector<Model>     m_meshes;
    std::vector<Texture>   m_textures;
    std::vector<Material>  m_materials;
};
```

#### `Scene::addTexture()`
**Location:** Scene.cpp, lines 24-28

```cpp
uint32_t Scene::addTexture(Texture texture) {
    uint32_t idx = static_cast<uint32_t>(m_textures.size());
    m_textures.push_back(std::move(texture));
    return idx;  // Index to use in MaterialComponent.materialIndex
}
```

**Returns:** Sequential index (0, 1, 2, ...) into the texture vector

#### `Scene::addMesh()`
**Location:** Scene.cpp, lines 18-22

```cpp
uint32_t Scene::addMesh(Model model) {
    uint32_t idx = static_cast<uint32_t>(m_meshes.size());
    m_meshes.push_back(std::move(model));
    return idx;  // Index to use in MeshComponent.meshIndex
}
```

### Bindless Registration

**Location:** Renderer.cpp (various places in buildScene)

```cpp
for (uint32_t i = 0; i < static_cast<uint32_t>(m_scene.getTextures().size()); ++i) {
    auto& tex = m_scene.getTexture(i);
    m_bindless->registerTexture(tex.getImageView(), tex.getSampler());
}
```

**What this does:**
- Iterates through all textures in `m_scene.m_textures`
- Calls `BindlessDescriptors::registerTexture()` for each
- Textures are registered in **order of addition to scene**
- Index in Scene = Index in bindless descriptor array

---

## 5. RENDERER BUILDSCENE() — GLB LOADING PIPELINE

### File: `src/renderer/Renderer.cpp`, lines 1840–2072

#### Map Asset Loading Lambda
**Location:** Renderer.cpp, lines 1983–2002

```cpp
auto loadMapAsset = [&](const std::string& filename) {
    std::string path = std::string(MODEL_DIR) + "map models/" + filename;
    try {
        // 1. Load mesh (positions, normals, UVs, indices)
        Model model = Model::loadFromGLB(*m_device, m_device->getAllocator(), path);
        uint32_t meshIdx = m_scene.addMesh(std::move(model));
        uint32_t texIdx = defaultTex;  // fallback
        
        // 2. Extract embedded textures
        auto glbTextures = Model::loadGLBTextures(*m_device, path);
        if (!glbTextures.empty()) {
            // Take FIRST texture from GLB
            texIdx = m_scene.addTexture(std::move(glbTextures[0].texture));
            
            // 3. Register in bindless descriptor array
            m_bindless->registerTexture(
                m_scene.getTexture(texIdx).getImageView(),
                m_scene.getTexture(texIdx).getSampler());
        }
        mapAssets[filename] = { meshIdx, texIdx };
        spdlog::info("Loaded map asset: {} (mesh={}, tex={})", filename, meshIdx, texIdx);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to load map asset '{}': {}", filename, e.what());
    }
};
```

**Critical Issues Here:**
1. ⚠️ **Only first texture taken:** `glbTextures[0]` — ignores other materials in GLB
2. ⚠️ **No per-material assignment** — all meshes get same texture index
3. ⚠️ **Fallback to defaultTex if no textures** — but doesn't log which GLB lacks textures
4. ✅ Texture immediately registered in bindless

---

#### MaterialComponent Constructor
**Location:** Renderer.cpp, lines 2050–2051 (map structures)

```cpp
reg.emplace<MaterialComponent>(e,
    MaterialComponent{ mapAssets[modelFile].texture, flatNorm, 0.0f, 0.0f, 0.3f, 0.6f });
```

**Constructor Signature (positional arguments):**
```cpp
MaterialComponent{
    uint32_t materialIndex,      // texIdx from mapAssets
    uint32_t normalMapIndex,     // flatNorm (default flat normal)
    float    shininess,          // 0.0f
    float    metallic,           // 0.0f (dielectric)
    float    roughness,          // 0.3f (fairly rough)
    float    emissive            // 0.6f (slight glow for towers)
}
```

**Similar pattern for character** (line 2280):
```cpp
MaterialComponent{ charTex, flatNorm, 0.0f, 0.0f, 0.3f, 0.6f }
```

---

#### Texture Registration Flow
**Location:** Renderer.cpp, lines 1900–1903

```cpp
// Bind ALL loaded textures to bindless descriptor array
for (uint32_t i = 0; i < static_cast<uint32_t>(m_scene.getTextures().size()); ++i) {
    auto& tex = m_scene.getTexture(i);
    m_bindless->registerTexture(tex.getImageView(), tex.getSampler());
}
```

---

## 6. GPU OBJECT DATA & SHADER LOOKUP

### File: `src/renderer/Renderer.h`, lines 164–176

```cpp
struct GpuObjectData {
    glm::mat4 model;           // 64 bytes
    glm::mat4 normalMatrix;    // 64 bytes
    glm::vec4 aabbMin;         // 16 bytes (xyz = world-space min)
    glm::vec4 aabbMax;         // 16 bytes (xyz = world-space max)
    glm::vec4 tint;            // 16 bytes
    glm::vec4 params;          // 16 bytes (x=shininess, y=metallic, z=roughness, w=emissive)
    glm::vec4 texIndices;      // 16 bytes (x=diffuseIdx, y=normalIdx, z/w unused)
    uint32_t  meshVertexOffset;// 4 bytes
    uint32_t  meshIndexOffset; // 4 bytes
    uint32_t  meshIndexCount;  // 4 bytes
    uint32_t  _pad;            // 4 bytes
};  // Total: 224 bytes (matches shader layout)
```

### Assignment in Renderer.cpp
**Location:** Renderer.cpp, lines 1231–1232, 1253–1254

```cpp
instances[objectCount].texIndices = glm::vec4(
    static_cast<float>(mat.materialIndex),  // x = diffuse texture index
    static_cast<float>(mat.normalMapIndex), // y = normal map index
    0.0f, 0.0f);
```

---

## 7. FRAGMENT SHADER TEXTURE LOOKUP

### File: `shaders/triangle.frag`

#### Bindless Texture Array
**Location:** triangle.frag, line 4

```glsl
layout(set = 1, binding = 0) uniform sampler2D textures[4096]; // global bindless array
```

- **Set 1** = bindless descriptor set
- **Binding 0** = array of 4096 samplers
- Each sampler corresponds to a Scene texture (in order of addition)

#### Vertex Shader Output
**Location:** triangle.vert, lines 71–72

```glsl
fragDiffuseIdx = int(obj.texIndices.x);
fragNormalIdx  = int(obj.texIndices.y);
```

#### Fragment Shader Usage
**Location:** triangle.frag, lines 173–221

**Terrain Mode (Sentinel: metallic < -0.5):**
```glsl
if (fragMetallic < -0.5) {
    // Splat blending: fragDiffuseIdx = control map
    // fragDiffuseIdx+1..+4 = detail textures
    vec4 splatWeights = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord);
    vec4 textureA = texture(textures[nonuniformEXT(fragDiffuseIdx + 1)], fragTexCoord * 8.0);
    // ... blend 4 layers
}
```

**Normal Model (Standard single texture):**
```glsl
albedo = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord).rgb * fragColor;

// Normal mapping (if metallic >= -0.5)
if (uvDeriv > 1e-12) {
    vec3 mapN = texture(textures[nonuniformEXT(fragNormalIdx)], fragTexCoord).rgb * 2.0 - 1.0;
    N = normalize(TBN * mapN);
}
```

**Extensions Required:**
```glsl
#extension GL_EXT_nonuniform_qualifier : require
```

- Allows `nonuniformEXT()` for dynamic indexing of texture arrays
- Index must be the same across all lanes in a SIMD group (shader compiler enforces this at compile time via analysis)

---

## 8. MAP MODELS DIRECTORY

### Path: `/Users/donkey/Development/1/Glory/map models/`

**Contents (14 files, ~122 MB total):**

| File | Size | Purpose |
|------|------|---------|
| `arcane+tile+3d+model.glb` | 4.2 MB | Lane tile (base texture for lane paths) |
| `blue_team_tower_1.glb` | 4.6 MB | T1 tower (blue team) |
| `blue_team_tower_2.glb` | 4.5 MB | T2 tower (blue team) |
| `blue_team_tower_3.glb` | 5.2 MB | T3 tower (blue team) |
| `blue_team_inhib.glb` | 3.7 MB | Inhibitor (blue team) |
| `blue_team_nexus.glb` | 4.4 MB | Nexus (blue team) |
| `read_team_tower_2.glb` | 4.2 MB | T2 tower (red team) — **TYPO: "read" not "red"** |
| `red_team_tower_1.glb` | 4.2 MB | T1 tower (red team) |
| `red_team_tower3.glb` | 5.2 MB | T3 tower (red team) — **NO UNDERSCORE** |
| `red_team_inhib.glb` | 3.6 MB | Inhibitor (red team) |
| `red_team_nexus.glb` | 4.5 MB | Nexus (red team) |
| `jungle_tile.glb` | 3.2 MB | Jungle terrain tile |
| `river_tile.glb` | 4.2 MB | River terrain tile |
| `lower_quality_meshes.glb` | 4.0 MB | (Unused in buildScene) |

**All files loaded via `loadMapAsset()` lambda in buildScene() (lines 2004–2016)**

---

## PIPELINE FLOW DIAGRAM

```
┌─────────────────────────────────────────────────────────────────────┐
│ 1. LOAD GLB (Renderer::buildScene)                                  │
│    Model::loadFromGLB(device, allocator, "map models/tower.glb")   │
│    ↓                                                                 │
│    tinygltf parses binary GLB                                       │
│    ↓                                                                 │
│    Extracts meshes[].primitives[]:                                  │
│      - POSITION (float3)                                            │
│      - NORMAL (float3) or compute via smooth normals               │
│      - TEXCOORD_0 (float2)                                          │
│      - Indices (uint16/uint32)                                      │
│    ↓                                                                 │
│    Returns Model with m_meshes[] + m_meshMaterialIndices[]         │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 2. ADD TO SCENE                                                      │
│    uint32_t meshIdx = m_scene.addMesh(model)                        │
│    ↓                                                                 │
│    Stored in m_scene.m_meshes[meshIdx]                             │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 3. EXTRACT TEXTURES                                                  │
│    auto glbTextures = Model::loadGLBTextures(device, path)         │
│    ↓                                                                 │
│    tinygltf iterates materials[].pbrMetallicRoughness.baseColor    │
│    ↓                                                                 │
│    Extracts image data (already decoded by stb_image)              │
│    ↓                                                                 │
│    RGB → RGBA expansion if needed                                   │
│    ↓                                                                 │
│    Returns std::vector<GLBTexture> with (materialIndex, Texture)   │
│    ⚠️ ISSUE: Only first entry used ([0])                            │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 4. ADD TEXTURE TO SCENE & REGISTER IN BINDLESS                      │
│    uint32_t texIdx = m_scene.addTexture(glbTextures[0].texture)    │
│    ↓                                                                 │
│    Stored in m_scene.m_textures[texIdx]                            │
│    ↓                                                                 │
│    m_bindless->registerTexture(imageView, sampler)                 │
│    ↓                                                                 │
│    Added to bindless descriptor array at binding 0, set 1          │
│    (index in descriptor array = texIdx)                             │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 5. CREATE ENTITY & ATTACH COMPONENTS                                │
│    auto entity = m_scene.createEntity(name)                         │
│    ↓                                                                 │
│    MeshComponent{meshIdx}                                           │
│    ↓                                                                 │
│    MaterialComponent{texIdx, flatNormIdx, 0.0f, 0.0f, 0.3f, 0.6f}│
│    (diffuse, normal, shininess, metallic, roughness, emissive)     │
│    ↓                                                                 │
│    TransformComponent{position, rotation, scale}                    │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 6. RENDER (Renderer::recordFrame)                                   │
│    ↓                                                                 │
│    For each entity with [MeshComponent, MaterialComponent]:         │
│      ↓                                                               │
│      GpuObjectData obj;                                             │
│      obj.model = entity.transform.getModelMatrix();                 │
│      obj.texIndices.x = entity.material.materialIndex   (← texIdx) │
│      obj.texIndices.y = entity.material.normalMapIndex  (← normIdx)│
│      obj.params = {shininess, metallic, roughness, emissive}      │
│      ↓                                                               │
│      Write to SSBO (binding 7, set 0)                              │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 7. SHADER LOOKUP (triangle.vert + triangle.frag)                    │
│    ↓                                                                 │
│    Vertex shader reads obj = objects[gl_InstanceIndex] from SSBO   │
│    ↓                                                                 │
│    Passes fragDiffuseIdx = int(obj.texIndices.x)                   │
│    Passes fragNormalIdx = int(obj.texIndices.y)                    │
│    ↓                                                                 │
│    Fragment shader:                                                  │
│      albedo = texture(textures[fragDiffuseIdx], uv).rgb            │
│      normal = texture(textures[fragNormalIdx], uv).rgb             │
│    ↓                                                                 │
│    textures[] is bindless array at layout(set=1, binding=0)        │
│    Index = Scene texture vector index (order of addition)           │
└─────────────────────────────────────────────────────────────────────┘
```

---

## CURRENT ISSUES & OBSERVATIONS

### ✅ **Working:**
1. GLB vertex data loading (POSITION, NORMAL, TEXCOORD_0)
2. Smooth normal computation for GLBs without normals
3. Texture extraction from embedded base-color
4. Bindless texture registration (4096-sampler array)
5. SSBO-based GPU object data (GpuObjectData)
6. Fragment shader texture lookup via dynamic indexing

### ⚠️ **Issues & Limitations:**

1. **Multi-Material GLBs Unsupported:**
   - Only `glbTextures[0]` used; other materials ignored
   - All primitives in a model share same texture index
   - ❌ Causes: Towers with 3 different material zones render with single color

2. **Missing PBR Texture Extraction:**
   - Only base-color extracted, not:
     - Normal maps
     - Roughness/metallic maps (PNG packed in glTF)
     - Emissive maps
   - ❌ Causes: All materials use per-entity float constants instead

3. **No Texture Fallback Logging:**
   - If GLB has no textures, silently uses `defaultTex`
   - ❌ Causes: Hard to debug missing textures

4. **Material Index Mismatch Risk:**
   - `Model.m_meshMaterialIndices[i]` stores glTF material index
   - But `loadGLBTextures()` returns textures indexed by glTF material index
   - No code currently uses `m_meshMaterialIndices` to match textures to meshes
   - ❌ Causes: If GLB has materials [0, 2, 5], textures may be [0, 1, 2] (renumbered by missing indices)

5. **Terrain Splat Mode Hardcoded:**
   - `metallic < -0.5` is sentinel to enable splat blending
   - No explicit terrain component
   - ❌ Causes: Easy to trigger accidentally; shader bloat

---

## VERTEX DATA STRUCTURE (GPU)

As stored in MegaBuffer (mega-buffer.h):

```cpp
// Per-vertex in GPU buffer
struct Vertex {
    glm::vec3 position;    // 12 bytes
    glm::vec3 color;       // 12 bytes (always white for GLB meshes)
    glm::vec3 normal;      // 12 bytes
    glm::vec2 texCoord;    // 8 bytes
};  // Total: 44 bytes per vertex, typically padded to 48
```

---

## SUMMARY TABLE

| Component | Location | Struct/Type | Purpose |
|-----------|----------|-------------|---------|
| **GLB Loader** | GLBLoader.cpp:58–222 | `Model::loadFromGLB()` | Extract mesh vertex data |
| **Texture Extractor** | GLBLoader.cpp:225–292 | `Model::loadGLBTextures()` | Extract embedded images |
| **Material** | Components.h:61–68 | `MaterialComponent` | Per-entity material: 2 texture indices + 4 floats |
| **Texture Manager** | Scene.h/cpp | `Scene::addTexture()` | Store textures, return index |
| **Bindless Registry** | Renderer.cpp:1900–1903 | `m_bindless->registerTexture()` | Register all textures in descriptor array |
| **GPU Object Data** | Renderer.h:164–176 | `GpuObjectData` | SSBO struct: model matrix, material, texture indices |
| **Shader Lookup** | triangle.vert/frag | `texIndices.xy` → `textures[]` | Dynamic bindless texture indexing |

