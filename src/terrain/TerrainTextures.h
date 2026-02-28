#pragma once

#include "renderer/Texture.h"

#include <memory>

namespace glory {

class Device;

/// Holds all terrain-specific textures (6 total for splat-based rendering).
struct TerrainTextures {
  Texture grass; // Set 1, Binding 0 — tiling grass (SRGB)
  Texture dirt;  // Set 1, Binding 1 — tiling dirt/path (SRGB)
  Texture stone; // Set 1, Binding 2 — tiling rock (SRGB)
  Texture
      splatMap; // Set 1, Binding 3 — R=grass, G=dirt, B=stone, A=water (UNORM)
  Texture teamMap;   // Set 1, Binding 4 — gradient Blue(0)→Red(1) (R8 via RGBA)
  Texture normalMap; // Set 1, Binding 5 — terrain detail normals (UNORM)

  /// Generate all 6 textures procedurally.
  void generate(const Device &device);
};

} // namespace glory
