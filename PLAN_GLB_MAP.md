# Plan: Replace Procedural Terrain with GLB Map Model

## Context

The current MOBA map is a procedurally generated 200x200 terrain (`TerrainSystem`) with Perlin noise heightmap, splat-based texturing, and water -- all using a dedicated terrain pipeline/shaders separate from the scene pipeline. The user wants to replace this visual terrain with `cyberpunk+moba+map+model.glb`, a pre-made 3D map model.

**Approach:** Load the GLB as a regular scene entity rendered through the existing scene pipeline (same pipeline used for characters/projectiles via instanced drawing). Skip the procedural terrain rendering. Keep the terrain system alive only for camera bounds.

---

## Files to Modify

### 1. `src/renderer/Model.h` -- Add bounds extraction helper

Add static method:
```cpp
static AABB getGLBBounds(const std::string &filepath);
```

### 2. `src/renderer/GLBLoader.cpp` -- Implement `getGLBBounds()`

Load GLB with tinygltf, read POSITION accessor `minValues`/`maxValues` across all meshes/primitives, return overall AABB. Fallback: compute from vertex data if min/max missing.

### 3. `src/scene/Components.h` -- Add `MapComponent` tag

```cpp
struct MapComponent {}; // tag to identify the map entity for MOBA rendering
```

### 4. `src/renderer/Renderer.cpp` -- Three changes

**a) `buildScene()`: Load GLB map (before current character loading section)**
- Call `Model::getGLBBounds("cyberpunk+moba+map+model.glb")` to get model AABB
- Compute uniform scale: `200.0 / max(width, depth)`
- Compute translation to center at (100, 0, 100) with ground at Y=0
- Load model via `loadFromGLB()`, textures via `loadGLBTextures()`
- Create entity with `MeshComponent`, `MaterialComponent`, `ColorComponent`, `MapComponent`
- Set transform with computed scale/position

**b) `recordCommandBuffer()` MOBA section (~line 1141): Skip terrain rendering**
- Replace `m_terrain->render(...)` with a comment / conditional skip
- Keep isometric camera update and debug line rendering intact

**c) `recordCommandBuffer()` MOBA entity section (~line 1260): Include map in rendering**
- Add view query for `TransformComponent + MeshComponent + MapComponent`
- Feed into existing `addEntityToGroups()` lambda (same instanced path as characters/projectiles)

### 5. `src/scene/Scene.cpp` -- Disable terrain height snapping

In the character movement section, wrap the `GetHeightAt` call so it's skipped (character stays at Y=0, matching the GLB ground plane). Simplest: remove/comment the terrain height snapping or guard with a flag.

---

## Scale/Position Calculation

```
AABB from GLB: min(x,y,z) -> max(x,y,z)
width  = max.x - min.x
depth  = max.z - min.z

scale = 200.0 / max(width, depth)
position.x = 100.0 - ((min.x + width/2) * scale)
position.y = -(min.y * scale)
position.z = 100.0 - ((min.z + depth/2) * scale)
```

---

## Verification

1. **Build**: `cmake --build build` -- no errors
2. **Launch**: GLB map should appear centered in the 200x200 world
3. **Camera**: Isometric camera should frame the map correctly (bounds 0-200 unchanged)
4. **Character**: Click-to-move should work, character walks on flat ground (Y~=0)
5. **Shadows**: Map should cast/receive shadows
6. **Textures**: GLB embedded textures should render via bindless texture array
