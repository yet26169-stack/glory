# Glory Engine — Model Rendering & Texture Fix Specification

## Summary

Models in the Glory engine MOBA mode suffer from three interconnected issues:
1. **Textures not applied to GLB map entities** — Map tiles use `defaultTex` (flat white) instead of embedded GLB textures
2. **Axis mismatch** — glTF/GLB models use a Y-up right-handed coordinate system, but the engine's MOBA world assumes Y-up with XZ ground plane and no correction is applied for models authored in Z-up tools (e.g. Blender default exports)
3. **Per-mesh texture assignment is lost** — The GLB loader extracts textures per-material, but entities are created with a single `MaterialComponent.materialIndex`, so multi-material GLB models only show one texture (or the default)

---

## Repository Context

- **Repo**: `donkey-ux/glory`
- **Key files**:
  - `src/renderer/GLBLoader.cpp` — GLB mesh + texture loading
  - `src/renderer/Renderer.cpp` — `buildScene()` + `recordCommandBuffer()`
  - `src/scene/Components.h` — ECS components (`TransformComponent`, `MaterialComponent`, `MapComponent`)
  - `shaders/triangle.vert` — Instanced vertex shader (main scene pipeline)
  - `shaders/triangle.frag` — PBR fragment shader with bindless textures

---

## Bug 1: Map Tiles Use Default Texture Instead of GLB Textures

### Root Cause

In `src/renderer/Renderer.cpp` → `buildScene()`, the "Flat LoL-style map" section (around line 1378) creates map entities using `makeTile()` and `makeStrip()` lambdas that always assign `defaultTex` and `flatNorm`:

```cpp
// src/renderer/Renderer.cpp ~line 1378
auto makeTile = [&](const std::string &name, glm::vec3 pos, glm::vec3 sz,
                    glm::vec4 col) {
  auto e = m_scene.createEntity(name);
  m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{boxMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      e, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 1.0f}); // ← always defaultTex!
  m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{col});
  m_scene.getRegistry().emplace<MapComponent>(e);
  ...
};
```

When the GLB map model (`fantasy+arena+3d+model.glb`) is loaded, its embedded textures are extracted via `Model::loadGLBTextures()`, but **there is no code that creates a `MapComponent` entity using the GLB mesh + its extracted textures**. The `PLAN_GLB_MAP.md` describes the intended approach but it was never fully wired: the code still falls through to the flat colored-cube tile map (`m_customMap = true` path) instead of creating a single GLB map entity with proper textures.

### Fix

In `buildScene()`, when loading the GLB map, you must:
1. Load the GLB model via `Model::loadFromGLB()`
2. Load textures via `Model::loadGLBTextures()` and register them with `m_scene.addTexture()`
3. Create a map entity with `MeshComponent` pointing to the loaded GLB mesh
4. Set `MaterialComponent.materialIndex` to the first GLB texture index (not `defaultTex`)
5. Update the bindless descriptor array to include the new textures
6. Set `m_glbMapLoaded = true` so the terrain rendering is skipped

```cpp
// In buildScene(), MOBA mode section, BEFORE the flat tile fallback:

std::string mapGlbPath = std::string(MODEL_DIR) + "fantasy+arena+3d+model.glb";
try {
    auto bounds = Model::getGLBBounds(mapGlbPath);
    float width  = bounds.max.x - bounds.min.x;
    float depth  = bounds.max.z - bounds.min.z;
    float uniScale = 200.0f / std::max(width, depth);

    uint32_t mapMeshIdx = m_scene.addMesh(
        Model::loadFromGLB(*m_device, m_device->getAllocator(), mapGlbPath));

    auto mapTextures = Model::loadGLBTextures(*m_device, mapGlbPath);
    uint32_t mapTexIdx = defaultTex; // fallback
    if (!mapTextures.empty()) {
        mapTexIdx = m_scene.addTexture(std::move(mapTextures[0]));
    }
    // Register ALL additional textures for multi-material models
    for (size_t i = 1; i < mapTextures.size(); ++i) {
        m_scene.addTexture(std::move(mapTextures[i]));
    }

    // Re-write bindless descriptor array to include new textures
    uint32_t texCount = static_cast<uint32_t>(m_scene.getTextures().size());
    for (uint32_t t = 0; t < texCount; ++t) {
        auto &tex = m_scene.getTexture(t);
        m_descriptors->writeBindlessTexture(t, tex.getImageView(), tex.getSampler());
    }

    auto mapEntity = m_scene.createEntity("GLBMap");
    m_scene.getRegistry().emplace<MeshComponent>(mapEntity, MeshComponent{mapMeshIdx});
    m_scene.getRegistry().emplace<MaterialComponent>(
        mapEntity, MaterialComponent{mapTexIdx, flatNorm, 0.0f, 0.0f, 1.0f});
    m_scene.getRegistry().emplace<ColorComponent>(
        mapEntity, ColorComponent{glm::vec4(1.0f)}); // white tint = show texture as-is
    m_scene.getRegistry().emplace<MapComponent>(mapEntity);

    auto &mapT = m_scene.getRegistry().get<TransformComponent>(mapEntity);
    mapT.position = glm::vec3(
        100.0f - ((bounds.min.x + width / 2.0f) * uniScale),
        -(bounds.min.y * uniScale),
        100.0f - ((bounds.min.z + depth / 2.0f) * uniScale));
    mapT.scale = glm::vec3(uniScale);

    m_glbMapLoaded = true;
    spdlog::info("GLB map loaded: scale={:.2f}, pos=({:.1f},{:.1f},{:.1f})",
                 uniScale, mapT.position.x, mapT.position.y, mapT.position.z);
} catch (const std::exception &e) {
    spdlog::warn("Failed to load GLB map, falling back to flat tiles: {}", e.what());
    m_glbMapLoaded = false;
}

// Only create flat tile map if GLB load failed:
if (!m_glbMapLoaded) {
    m_customMap = true;
    // ... existing makeTile/makeStrip code ...
}
```

### Files to Modify
- `src/renderer/Renderer.cpp` — `buildScene()` method

---

## Bug 2: Axis Mismatch — GLB Models Oriented Incorrectly Relative to Map

### Root Cause

The glTF 2.0 specification mandates **Y-up, right-handed** coordinates. However, many 3D tools (especially Blender's default) export with different conventions, and the engine applies **no axis correction** when loading GLB files.

In `src/renderer/GLBLoader.cpp` → `loadFromGLB()`, vertex positions are copied raw:

```cpp
// GLBLoader.cpp line 120
std::memcpy(&v.position, posBytes + i * posStride, sizeof(float) * 3);
```

No rotation or axis swap is applied. Meanwhile, `TransformComponent::getModelMatrix()` (Components.h line 29-40) uses Y-X-Z Euler rotation order, but the map entity's `rotation` is left at `{0, 0, 0}` — meaning if the GLB model was authored with Z-up, it will appear lying on its side.

Additionally, the `getGLBBounds()` function computes the AABB from raw glTF positions. The scale/position calculation in `PLAN_GLB_MAP.md` uses `width = max.x - min.x` and `depth = max.z - min.z`, which is correct for Y-up models but **wrong for Z-up models** where depth would be along the Y axis in glTF space.

### Fix

Add an axis-correction rotation to the map entity transform when the GLB model's AABB suggests it is Z-up (i.e. the Y extent is much larger than expected for a flat map, or the Z extent is near zero):

```cpp
// In buildScene(), after computing bounds and before setting mapT:
float height = bounds.max.y - bounds.min.y;
float zExtent = bounds.max.z - bounds.min.z;

bool isZUp = (height > zExtent * 2.0f); // heuristic: Y range >> Z range → model is Z-up

if (isZUp) {
    // Rotate -90° around X to convert Z-up → Y-up
    mapT.rotation.x = glm::radians(-90.0f);

    // Recompute width/depth using corrected axes:
    // After -90° X rotation: new_Y = old_Z, new_Z = -old_Y
    width  = bounds.max.x - bounds.min.x;
    depth  = bounds.max.y - bounds.min.y; // was the "height" in Z-up
    uniScale = 200.0f / std::max(width, depth);

    mapT.position = glm::vec3(
        100.0f - ((bounds.min.x + width / 2.0f) * uniScale),
        -(bounds.min.z * uniScale),  // Z min becomes ground level after rotation
        100.0f - ((bounds.min.y + depth / 2.0f) * uniScale));
    mapT.scale = glm::vec3(uniScale);

    spdlog::info("GLB map detected as Z-up, applied -90° X rotation");
}
```

For **character/skinned models** (scientist, minions), the same issue applies. In `buildScene()` around line 1602, the character transform has no rotation correction either. If the character model was exported Z-up, add:

```cpp
// After setting charT.position:
charT.rotation.x = glm::radians(-90.0f); // Z-up → Y-up correction
```

### Important: Also handle the `loadSkinnedFromGLB` path

In `GLBLoader.cpp` → `loadSkinnedFromGLB()`, the skeleton joints' local transforms are read from glTF nodes directly (line ~424-436). The glTF spec says the root node's transform should encode any axis conversion. If it doesn't (common with Blender exports using "+Y Up" = false), the skeleton will also be misoriented. The fix is the same: apply the entity-level rotation, which already propagates through `inst.model` in the instance buffer.

### Files to Modify
- `src/renderer/Renderer.cpp` — `buildScene()`, entity transform setup
- Optionally `src/renderer/GLBLoader.cpp` — add an optional axis-correction parameter to `loadFromGLB()`

---

## Bug 3: Multi-Material GLB Models Show Only One Texture

### Root Cause

`MaterialComponent` holds a single `materialIndex` (diffuse texture index). But a GLB model can contain multiple primitives, each referencing a different material/texture. The current `loadFromGLB()` creates one `Mesh` per primitive (stored in `Model::m_meshes`), but the entity system assigns one `MaterialComponent` to the entire entity.

In `recordCommandBuffer()`, the `addEntityToGroups` lambda reads the entity's single `MaterialComponent` and writes the same `texIdx` into every instance:

```cpp
// Renderer.cpp ~line 2506
auto *matComp = m_scene.getRegistry().try_get<MaterialComponent>(entity);
if (matComp) {
    texIdx = matComp->materialIndex;  // ← single texture for ALL sub-meshes
    ...
}
inst.texIndices = glm::vec4(static_cast<float>(texIdx), ...);
```

This means sub-mesh 0 might correctly use texture 0, but sub-meshes 1, 2, etc. all get the same texture.

### Fix

This requires a design change. Two approaches:

#### Approach A: One Entity Per Primitive (Recommended — minimal shader changes)

When loading a multi-material GLB for the map, create **one entity per primitive** instead of one entity for the whole model. Each entity gets its own `MeshComponent` (pointing to the specific sub-mesh) and its own `MaterialComponent` (pointing to the matching texture).

```cpp
// In buildScene(), replace single-entity creation with:
auto mapTextures = Model::loadGLBTextures(*m_device, mapGlbPath);
std::vector<uint32_t> texIndices;
for (auto &t : mapTextures) {
    texIndices.push_back(m_scene.addTexture(std::move(t)));
}

Model mapModel = Model::loadFromGLB(*m_device, m_device->getAllocator(), mapGlbPath);
uint32_t meshCount = mapModel.getMeshCount();

// We need to add each sub-mesh as a separate Model (single-mesh) to the scene
// OR add the whole model and create per-submesh entities
uint32_t baseMeshIdx = m_scene.addMesh(std::move(mapModel));

for (uint32_t mi = 0; mi < meshCount; ++mi) {
    auto subEntity = m_scene.createEntity("GLBMap_sub" + std::to_string(mi));
    m_scene.getRegistry().emplace<MeshComponent>(subEntity, MeshComponent{baseMeshIdx});

    uint32_t texForThisMesh = (mi < texIndices.size()) ? texIndices[mi] : defaultTex;
    m_scene.getRegistry().emplace<MaterialComponent>(
        subEntity, MaterialComponent{texForThisMesh, flatNorm, 0.0f, 0.0f, 1.0f});
    m_scene.getRegistry().emplace<ColorComponent>(
        subEntity, ColorComponent{glm::vec4(1.0f)});
    m_scene.getRegistry().emplace<MapComponent>(subEntity);

    auto &t = m_scene.getRegistry().get<TransformComponent>(subEntity);
    t.position = mapT.position; // same transform as computed above
    t.scale    = mapT.scale;
    t.rotation = mapT.rotation;
}
```

**However**, this approach requires either splitting the `Model` into individual single-mesh `Model` objects, or making `MeshComponent` aware of which sub-mesh within a multi-mesh `Model` to draw. Currently the instanced draw loop calls `model.drawIndirect()` which draws **all** sub-meshes, defeating the purpose.

#### Approach B: Per-Primitive Texture Index Array (Better long-term)

Add a material/texture index array to the GLB loading pipeline so each primitive within a model knows its own texture. This requires:

1. **In `GLBLoader.cpp`**: Track which material/texture each primitive uses:

```cpp
// In loadFromGLB(), inside the primitive loop, after building vertices/indices:
int primMatIdx = prim.material; // glTF material index for this primitive
model.m_meshMaterialIndices.push_back(primMatIdx); // new field on Model
```

2. **In `Model.h`**: Add a per-mesh material index:

```cpp
class Model {
    // ... existing ...
    std::vector<int> m_meshMaterialIndices; // glTF material index per sub-mesh
public:
    int getMeshMaterialIndex(uint32_t meshIdx) const {
        return (meshIdx < m_meshMaterialIndices.size())
            ? m_meshMaterialIndices[meshIdx] : -1;
    }
};
```

3. **In `recordCommandBuffer()`**: When building indirect draw commands for a multi-mesh model, emit one draw group per sub-mesh with the correct texture index:

```cpp
for (uint32_t mi = 0; mi < g.indirectCount; ++mi) {
    auto &icmd = indirectPtr[mobaIndirectCmds++];
    icmd.indexCount = model.getMeshIndexCount(mi);
    icmd.instanceCount = g.instanceCount;
    icmd.firstIndex = 0;
    icmd.vertexOffset = 0;
    icmd.firstInstance = g.instanceOffset;

    // Override texture index for this sub-mesh if GLB material mapping exists
    int glbMatIdx = model.getMeshMaterialIndex(mi);
    if (glbMatIdx >= 0 && glbMatIdx < mapTexCount) {
        // Write corrected texIndices into the instance data for this draw
        // (requires per-draw-call instance data or push constant)
    }
}
```

### Files to Modify
- `src/renderer/GLBLoader.cpp` — track per-primitive material indices
- `src/renderer/Model.h` / `Model.cpp` — store `m_meshMaterialIndices`
- `src/renderer/Renderer.cpp` — `buildScene()` and `recordCommandBuffer()`

---

## Bug 4: `loadGLBTextures()` Texture-to-Mesh Mapping Is Lost

### Root Cause

`loadGLBTextures()` (GLBLoader.cpp line 209-273) iterates **materials** and extracts the `baseColorTexture` from each. It returns a flat `std::vector<Texture>` with no information about which mesh/primitive each texture belongs to. Materials that share a texture produce duplicates, and materials with no texture are skipped — causing the index mapping between returned textures and mesh primitives to be misaligned.

### Fix

Return a mapping alongside the textures:

```cpp
// New signature (or add a new overload):
struct GLBTextureInfo {
    Texture texture;
    int materialIndex; // which glTF material this came from
};
static std::vector<GLBTextureInfo> loadGLBTexturesWithMapping(
    const Device &device, const std::string &filepath);
```

Or simpler: return a `std::vector<int>` mapping primitive index → returned texture index:

```cpp
// In loadGLBTextures, build a side-channel:
// materialToTexIdx[glTF_material_index] = index in returned vector
std::unordered_map<int, uint32_t> materialToTexIdx;
```

Then when creating entities in `buildScene()`, use `model.getMeshMaterialIndex(mi)` → `materialToTexIdx` → scene texture index.

### Files to Modify
- `src/renderer/GLBLoader.cpp` — `loadGLBTextures()` or new overload
- `src/renderer/Model.h` — new struct/return type

---

## Bug 5: Vertex Color Tint Washes Out Textures

### Root Cause

In `shaders/triangle.frag` line 130:

```glsl
vec3 albedo = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord).rgb * fragColor;
```

And in `shaders/triangle.vert` line 38:

```glsl
fragColor = inColor * inTint.rgb;
```

The vertex color (`inColor`) comes from the mesh data, where the GLB loader sets it to white `(1,1,1)`. The tint comes from `ColorComponent`. **If `ColorComponent` has a non-white tint** (as the flat tile map does with colors like `cJungle(0.13, 0.18, 0.08)`), the texture color is multiplied by it, darkening or color-shifting the texture.

### Fix

For GLB map entities that have real textures, set `ColorComponent` to pure white:

```cpp
m_scene.getRegistry().emplace<ColorComponent>(
    mapEntity, ColorComponent{glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
```

This is already addressed in Bug 1's fix above. The existing `makeTile()` lambda sets tint colors intentionally for the flat procedural map — those should NOT be applied to textured GLB entities.

### Files to Modify
- `src/renderer/Renderer.cpp` — `buildScene()` GLB entity creation

---

## Summary of All Changes

| File | Change | Bug(s) |
|---|---|---|
| `src/renderer/Renderer.cpp` | Create GLB map entity with proper textures in `buildScene()`; skip flat tiles when GLB loads | 1, 5 |
| `src/renderer/Renderer.cpp` | Add axis-correction rotation to map/character entity transforms | 2 |
| `src/renderer/GLBLoader.cpp` | Track per-primitive material index during loading | 3, 4 |
| `src/renderer/Model.h` | Add `m_meshMaterialIndices` vector and accessor | 3, 4 |
| `src/renderer/GLBLoader.cpp` | Return material mapping from `loadGLBTextures()` | 4 |
| `src/renderer/Renderer.cpp` | In `recordCommandBuffer()`, use per-submesh texture when drawing multi-material models | 3 |
| `src/renderer/Renderer.cpp` | Ensure `ColorComponent` is white `(1,1,1,1)` for textured GLB entities | 5 |

## Priority Order

1. **Bug 1** (Critical) — GLB map not using its textures at all → everything appears untextured
2. **Bug 2** (Critical) — Axis mismatch → models appear sideways/rotated
3. **Bug 5** (High) — Color tint washing out textures → easy fix, immediate visual improvement
4. **Bug 3** (Medium) — Multi-material support → affects complex GLB models
5. **Bug 4** (Medium) — Texture mapping accuracy → needed for Bug 3 to work correctly

## Testing

1. Build: `cmake --build build` — verify no compilation errors
2. Launch in MOBA mode — GLB map should appear centered in the 200×200 world with visible textures
3. Verify the map is oriented correctly (ground on XZ plane, Y is up)
4. Verify the character model (scientist) stands upright on the map, not sideways
5. Verify the map's embedded textures are visible (not flat white or colored)
6. Check shadow casting/receiving still works on the map entity
7. Verify click-to-move still works (character navigates on the map surface)