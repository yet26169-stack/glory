#include "vfx/VFXFactory.h"
#include "vfx/VFXRenderer.h"

#include <spdlog/spdlog.h>
#include <cstring>

namespace glory {

void VFXFactory::init(VFXDefinitionLoader* loader, VFXRenderer* renderer) {
    m_loader   = loader;
    m_renderer = renderer;

    // Register all loaded definitions with the renderer
    if (m_loader) {
        for (const auto& name : m_loader->getLoadedNames()) {
            const VFXDefinition* def = m_loader->getDefinition(name);
            if (def) registerWithRenderer(*def);
        }
        spdlog::info("[VFXFactory] Registered {} definitions with renderer",
                     m_loader->size());
    }
}

void VFXFactory::registerWithRenderer(const VFXDefinition& def) {
    if (!m_renderer) return;
    EmitterDef emitterDef = def.toEmitterDef();
    m_renderer->registerEmitter(std::move(emitterDef));
}

uint32_t VFXFactory::spawn(const std::string& defName,
                            const glm::vec3& position,
                            const glm::vec3& direction,
                            float scale,
                            float lifetimeOverride) {
    if (!m_loader || !m_renderer) {
        spdlog::warn("[VFXFactory] Not initialized, cannot spawn '{}'", defName);
        return INVALID_VFX_HANDLE;
    }

    const VFXDefinition* def = m_loader->getDefinition(defName);
    if (!def) {
        spdlog::warn("[VFXFactory] Definition '{}' not found", defName);
        return INVALID_VFX_HANDLE;
    }

    // Build a VFX event and push it through the renderer's event handling
    VFXEvent ev{};
    ev.type      = VFXEventType::Spawn;
    ev.handle    = 0; // will be assigned by handleSpawn
    ev.position  = position;
    ev.direction = direction;
    ev.scale     = scale;
    ev.lifetime  = lifetimeOverride;

    // Copy effect ID (truncate to 47 chars + null)
    std::strncpy(ev.effectID, defName.c_str(), sizeof(ev.effectID) - 1);
    ev.effectID[sizeof(ev.effectID) - 1] = '\0';

    // Use renderer's public spawn interface
    m_renderer->spawnDirect(ev);
    return ev.handle;
}

uint32_t VFXFactory::spawnAt(const std::string& defName,
                              const glm::vec3& position,
                              float scale) {
    return spawn(defName, position, glm::vec3(0.f, 1.f, 0.f), scale);
}

uint32_t VFXFactory::tickHotReload() {
    if (!m_loader) return 0;

    uint32_t reloaded = m_loader->hotReload();
    if (reloaded > 0 && m_renderer) {
        // Re-register all reloaded definitions
        for (const auto& name : m_loader->getLoadedNames()) {
            const VFXDefinition* def = m_loader->getDefinition(name);
            if (def) registerWithRenderer(*def);
        }
    }
    return reloaded;
}

} // namespace glory
