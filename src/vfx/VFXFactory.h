#pragma once

// VFX Factory — high-level API for spawning VFX effects by definition name.
// Looks up definitions from VFXDefinitionLoader, converts to EmitterDef,
// and registers/spawns via VFXRenderer.

#include "vfx/VFXDefinitionLoader.h"
#include "vfx/VFXTypes.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace glory {

class VFXRenderer;

class VFXFactory {
public:
    VFXFactory() = default;

    void init(VFXDefinitionLoader* loader, VFXRenderer* renderer);

    // Spawn a VFX effect by definition name. Returns a handle for later control.
    // If the definition is not found, logs a warning and returns INVALID_VFX_HANDLE.
    uint32_t spawn(const std::string& defName,
                   const glm::vec3& position,
                   const glm::vec3& direction = glm::vec3(0.f, 1.f, 0.f),
                   float scale = 1.0f,
                   float lifetimeOverride = -1.0f);

    // Convenience: spawn at an entity's position
    uint32_t spawnAt(const std::string& defName,
                     const glm::vec3& position,
                     float scale = 1.0f);

    // Tick hot-reload on the loader. Call once per frame.
    // Returns number of reloaded definitions; if > 0, re-registers them with VFXRenderer.
    uint32_t tickHotReload();

private:
    VFXDefinitionLoader* m_loader   = nullptr;
    VFXRenderer*         m_renderer = nullptr;

    // Register a VFXDefinition with the renderer (converts to EmitterDef)
    void registerWithRenderer(const VFXDefinition& def);
};

} // namespace glory
