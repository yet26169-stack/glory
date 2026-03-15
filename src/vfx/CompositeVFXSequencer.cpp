#include "vfx/CompositeVFXSequencer.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/TrailRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "vfx/MeshEffectRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "renderer/DistortionRenderer.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace glory {

void CompositeVFXSequencer::loadDirectory(const std::string& dirPath) {
    namespace fs = std::filesystem;
    if (!fs::exists(dirPath)) return;

    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        try {
            nlohmann::json j;
            f >> j;
            
            CompositeVFXDef def;
            def.id = j.value("id", entry.path().stem().string());
            
            if (j.contains("layers")) {
                for (const auto& lj : j["layers"]) {
                    VFXLayer layer;
                    layer.delay = lj.value("delay", 0.0f);
                    
                    std::string typeStr = lj.value("type", "PARTICLE");
                    if (typeStr == "PARTICLE") layer.type = VFXLayerType::PARTICLE;
                    else if (typeStr == "TRAIL") layer.type = VFXLayerType::TRAIL;
                    else if (typeStr == "GROUND_DECAL") layer.type = VFXLayerType::GROUND_DECAL;
                    else if (typeStr == "MESH_EFFECT") layer.type = VFXLayerType::MESH_EFFECT;
                    else if (typeStr == "SHOCKWAVE") layer.type = VFXLayerType::SHOCKWAVE;
                    else if (typeStr == "CONE") layer.type = VFXLayerType::CONE;
                    else if (typeStr == "SHIELD") layer.type = VFXLayerType::SHIELD;
                    else if (typeStr == "SPRITE_EFFECT") layer.type = VFXLayerType::SPRITE_EFFECT;
                    else if (typeStr == "DISTORTION") layer.type = VFXLayerType::DISTORTION;

                    layer.effectRef = lj.value("effectRef", "");
                    layer.duration = lj.value("duration", -1.0f);
                    layer.scale = lj.value("scale", 1.0f);
                    
                    if (lj.contains("color")) {
                        auto& c = lj["color"];
                        layer.color = {c[0], c[1], c[2], c[3]};
                    }

                    std::string anchorStr = lj.value("anchor", "CASTER");
                    if (anchorStr == "CASTER")     layer.anchor = VFXLayer::Anchor::CASTER;
                    else if (anchorStr == "TARGET") layer.anchor = VFXLayer::Anchor::TARGET;
                    else if (anchorStr == "PROJECTILE") layer.anchor = VFXLayer::Anchor::PROJECTILE;
                    else layer.anchor = VFXLayer::Anchor::WORLD;

                    if (lj.contains("offset")) {
                        auto& o = lj["offset"];
                        layer.offset = {o[0], o[1], o[2]};
                    }

                    layer.followAnchor = lj.value("followAnchor", false);

                    def.layers.push_back(layer);
                }
            }
            m_defs[def.id] = def;
            spdlog::info("CompositeVFX: Loaded '{}' with {} layers", def.id, def.layers.size());
        } catch (const std::exception& e) {
            spdlog::error("CompositeVFX: Failed to parse {}: {}", entry.path().string(), e.what());
        }
    }
}

uint32_t CompositeVFXSequencer::trigger(const std::string& compositeId,
                                         glm::vec3 casterPos,
                                         glm::vec3 targetPos,
                                         glm::vec3 direction,
                                         glm::vec3 projectilePos) {
    auto it = m_defs.find(compositeId);
    if (it == m_defs.end()) {
        spdlog::warn("CompositeVFX: Unknown definition '{}'", compositeId);
        return 0;
    }

    ActiveComposite active;
    active.handle       = m_nextHandle++;
    active.def          = &it->second;
    active.casterPos    = casterPos;
    active.targetPos    = targetPos;
    active.direction    = direction;
    active.projectilePos = projectilePos;
    active.fired.resize(it->second.layers.size(), false);
    
    m_active.push_back(active);
    return active.handle;
}

void CompositeVFXSequencer::cancel(uint32_t handle) {
    m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
        [handle](const ActiveComposite& c) { return c.handle == handle; }), m_active.end());
}

void CompositeVFXSequencer::updatePositions(uint32_t handle,
                                             glm::vec3 casterPos,
                                             glm::vec3 targetPos,
                                             glm::vec3 projectilePos) {
    for (auto& ac : m_active) {
        if (ac.handle != handle) continue;
        ac.casterPos     = casterPos;
        ac.targetPos     = targetPos;
        ac.projectilePos = projectilePos;
        break;
    }
}

void CompositeVFXSequencer::update(float dt,
                                    VFXEventQueue& particleQueue,
                                    TrailRenderer& trails,
                                    GroundDecalRenderer& decals,
                                    MeshEffectRenderer& meshFX,
                                    ExplosionRenderer& explosions,
                                    ConeAbilityRenderer& cones,
                                    SpriteEffectRenderer& sprites,
                                    DistortionRenderer& distortions) {
    for (auto it = m_active.begin(); it != m_active.end(); ) {
        it->elapsed += dt;
        bool allFired = true;

        for (size_t i = 0; i < it->def->layers.size(); ++i) {
            if (it->fired[i]) continue;
            
            const auto& layer = it->def->layers[i];
            if (it->elapsed >= layer.delay) {
                auto resolvePos = [&](const ActiveComposite& ac, VFXLayer::Anchor a) -> glm::vec3 {
                    switch (a) {
                        case VFXLayer::Anchor::CASTER:     return ac.casterPos;
                        case VFXLayer::Anchor::TARGET:     return ac.targetPos;
                        case VFXLayer::Anchor::PROJECTILE: return ac.projectilePos;
                        default:                           return glm::vec3(0);
                    }
                };
                glm::vec3 pos = resolvePos(*it, layer.anchor) + layer.offset;

                switch (layer.type) {
                    case VFXLayerType::PARTICLE: {
                        VFXEvent ev{};
                        ev.type = VFXEventType::Spawn;
                        std::strncpy(ev.effectID, layer.effectRef.c_str(), sizeof(ev.effectID)-1);
                        ev.position = pos;
                        ev.direction = it->direction;
                        ev.scale = layer.scale;
                        ev.lifetime = layer.duration;
                        particleQueue.push(ev);
                        break;
                    }
                    case VFXLayerType::TRAIL:
                        // Trails usually need a moving source, but we can spawn one at pos
                        trails.spawn(layer.effectRef, pos);
                        break;
                    case VFXLayerType::GROUND_DECAL:
                        decals.spawn(layer.effectRef, pos, layer.scale);
                        break;
                    case VFXLayerType::MESH_EFFECT:
                        meshFX.spawn(layer.effectRef, pos, it->direction, layer.scale);
                        break;
                    case VFXLayerType::SHOCKWAVE:
                        explosions.addExplosion(pos); // ExplosionRenderer uses addExplosion
                        break;
                    case VFXLayerType::CONE:
                        // ConeAbilityRenderer is usually triggered directly by AbilitySystem
                        // for timing, but we can support it here if we pass more params.
                        break;
                    case VFXLayerType::SPRITE_EFFECT:
                        // Needs effect index from string ID
                        // sprites.spawn(...)
                        break;
                    case VFXLayerType::DISTORTION:
                        distortions.spawn(layer.effectRef, pos);
                        break;
                    default: break;
                }
                it->fired[i] = true;

                // followAnchor layers: keep the composite alive until their duration
                // expires so callers can push updated positions via updatePositions().
                if (layer.followAnchor && layer.duration > 0.0f) {
                    it->fired[i] = false; // re-arm for duration tracking
                    // re-check allFired = false handled below
                }
            } else {
                allFired = false;
            }
        }

        // followAnchor layers also keep the composite alive post-fire until expiry
        for (size_t i = 0; i < it->def->layers.size(); ++i) {
            const auto& layer = it->def->layers[i];
            if (!it->fired[i] && layer.followAnchor && layer.duration > 0.0f) {
                float layerExpiry = layer.delay + layer.duration;
                if (it->elapsed < layerExpiry) {
                    allFired = false;
                } else {
                    it->fired[i] = true; // duration elapsed, mark done
                }
            }
        }

        if (allFired) {
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace glory
