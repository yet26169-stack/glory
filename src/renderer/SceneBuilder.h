#pragma once

namespace glory {

class Renderer;

/// Builds the game scene: loads map data, spawns structures, places lane
/// tiles, creates the player character, loads minion / ability assets,
/// and suballocates meshes into the mega-buffer.
///
/// Extracted from Renderer::buildScene() to keep Renderer.cpp focused on
/// Vulkan command recording and frame presentation.
class SceneBuilder {
public:
    /// Populate the scene with all game entities and assets.
    /// Called once during init (Renderer::buildScene delegates here).
    static void build(Renderer& renderer);
};

} // namespace glory
