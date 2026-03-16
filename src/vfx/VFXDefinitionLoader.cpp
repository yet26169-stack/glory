#include "vfx/VFXDefinitionLoader.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>

namespace glory {

namespace fs = std::filesystem;

// ══════════════════════════════════════════════════════════════════════════
// JSON Parsing
// ══════════════════════════════════════════════════════════════════════════

static glm::vec3 parseVec3(const nlohmann::json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

static glm::vec4 parseVec4(const nlohmann::json& j, const glm::vec4& fallback = glm::vec4(1.0f)) {
    if (!j.is_array() || j.size() < 4) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}

bool VFXDefinitionLoader::parseFile(const fs::path& filePath, VFXDefinition& out) {
    std::ifstream f(filePath);
    if (!f.is_open()) return false;

    try {
        nlohmann::json j;
        f >> j;

        out.name             = j.value("name", j.value("id", filePath.stem().string()));
        out.maxParticles     = j.value("maxParticles", 256u);
        out.lifeMin          = j.value("lifetimeMin", j.value("lifeMin", 0.8f));
        out.lifeMax          = j.value("lifetimeMax", j.value("lifeMax", 1.6f));
        out.emitRate         = j.value("emitRate", 40.0f);
        out.burstCount       = j.value("burstCount", 0.0f);
        out.looping          = j.value("looping", false);
        out.duration         = j.value("duration", 1.5f);

        // Emission shape
        std::string shape    = j.value("emitShape", "cone");
        if      (shape == "point")  out.emitShape = VFXDefinition::EmitShape::Point;
        else if (shape == "sphere") out.emitShape = VFXDefinition::EmitShape::Sphere;
        else if (shape == "box")    out.emitShape = VFXDefinition::EmitShape::Box;
        else                        out.emitShape = VFXDefinition::EmitShape::Cone;

        out.spreadAngle      = j.value("spreadAngle", 45.0f);
        out.emitRadius       = j.value("emitRadius", 0.0f);
        if (j.contains("emitExtents"))
            out.emitExtents  = parseVec3(j["emitExtents"]);

        // Per-particle randomisation
        out.initialSpeedMin  = j.value("initialSpeedMin", 2.0f);
        out.initialSpeedMax  = j.value("initialSpeedMax", 6.0f);
        out.sizeMin          = j.value("sizeMin", 0.2f);
        out.sizeMax          = j.value("sizeMax", 0.6f);
        out.sizeEnd          = j.value("sizeEnd", -1.0f);
        out.rotationSpeedMin = j.value("rotationSpeedMin", 0.0f);
        out.rotationSpeedMax = j.value("rotationSpeedMax", 0.0f);

        // Physics
        out.gravity          = j.value("gravity", 4.0f);
        out.drag             = j.value("drag", 0.0f);
        out.alphaCurve       = j.value("alphaCurve", 1.0f);
        out.windStrength     = j.value("windStrength", 0.0f);
        if (j.contains("windDirection"))
            out.windDirection = parseVec3(j["windDirection"]);

        // Force modules
        out.forces.clear();
        if (j.contains("forces") && j["forces"].is_array()) {
            for (const auto& fj : j["forces"]) {
                VFXDefinition::ForceModule fm;
                std::string ftype = fj.value("type", "gravity");
                if      (ftype == "gravity")    fm.type = VFXDefinition::ForceModule::Type::Gravity;
                else if (ftype == "radial")     fm.type = VFXDefinition::ForceModule::Type::Radial;
                else if (ftype == "vortex")     fm.type = VFXDefinition::ForceModule::Type::Vortex;
                else if (ftype == "turbulence") fm.type = VFXDefinition::ForceModule::Type::Turbulence;

                fm.strength  = fj.value("strength", 1.0f);
                if (fj.contains("direction")) fm.direction = parseVec3(fj["direction"]);
                if (fj.contains("center"))    fm.center    = parseVec3(fj["center"]);
                out.forces.push_back(fm);
            }
        } else {
            // Legacy: single forceType field
            uint32_t ft = j.value("forceType", 0u);
            VFXDefinition::ForceModule fm;
            fm.type     = static_cast<VFXDefinition::ForceModule::Type>(ft);
            fm.strength = j.value("forceStrength", 1.0f);
            if (j.contains("attractorPos"))
                fm.center = parseVec3(j["attractorPos"]);
            out.forces.push_back(fm);
        }

        // Rendering
        std::string rm = j.value("renderMode", "billboard");
        if      (rm == "mesh")  out.renderMode = VFXDefinition::RenderMode::Mesh;
        else if (rm == "trail") out.renderMode = VFXDefinition::RenderMode::Trail;
        else                    out.renderMode = VFXDefinition::RenderMode::Billboard;

        std::string bm = j.value("blendMode", "alpha");
        out.blendMode = (bm == "additive") ? VFXDefinition::BlendMode::Additive
                                           : VFXDefinition::BlendMode::Alpha;

        out.texturePath      = j.value("texturePath", j.value("textureAtlas", ""));
        out.atlasFrameCount  = j.value("atlasFrameCount", 1u);

        // Color over life
        out.colorOverLife.clear();
        if (j.contains("colorOverLifetime")) {
            for (const auto& k : j["colorOverLifetime"]) {
                float t = k["time"].get<float>();
                glm::vec4 c = parseVec4(k["color"]);
                out.colorOverLife.emplace_back(t, c);
            }
        }

        // Size over life
        out.sizeOverLife.clear();
        if (j.contains("sizeOverLifetime")) {
            for (const auto& k : j["sizeOverLifetime"]) {
                float t = k["time"].get<float>();
                float s = k["value"].get<float>();
                out.sizeOverLife.emplace_back(t, s);
            }
        }

        return true;
    } catch (const std::exception& e) {
        spdlog::warn("[VFXLoader] Failed to parse '{}': {}", filePath.string(), e.what());
        return false;
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Directory Loading
// ══════════════════════════════════════════════════════════════════════════

void VFXDefinitionLoader::loadDirectory(const std::string& dirPath) {
    m_dirPath = dirPath;

    if (!fs::exists(dirPath)) {
        spdlog::warn("[VFXLoader] Directory '{}' not found", dirPath);
        return;
    }

    uint32_t count = 0;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.path().extension() != ".json") continue;
        // Skip composites subdirectory entries
        if (entry.is_directory()) continue;

        VFXDefinition def;
        if (parseFile(entry.path(), def)) {
            FileEntry fe;
            fe.path      = entry.path();
            fe.lastWrite = fs::last_write_time(entry.path());
            fe.defName   = def.name;
            m_watchedFiles.push_back(fe);

            m_defs[def.name] = std::move(def);
            count++;
        }
    }

    spdlog::info("[VFXLoader] Loaded {} VFX definitions from '{}'", count, dirPath);
}

// ══════════════════════════════════════════════════════════════════════════
// Queries
// ══════════════════════════════════════════════════════════════════════════

const VFXDefinition* VFXDefinitionLoader::getDefinition(const std::string& name) const {
    auto it = m_defs.find(name);
    return (it != m_defs.end()) ? &it->second : nullptr;
}

std::vector<std::string> VFXDefinitionLoader::getLoadedNames() const {
    std::vector<std::string> names;
    names.reserve(m_defs.size());
    for (const auto& [k, v] : m_defs)
        names.push_back(k);
    return names;
}

// ══════════════════════════════════════════════════════════════════════════
// Hot-Reload
// ══════════════════════════════════════════════════════════════════════════

uint32_t VFXDefinitionLoader::hotReload() {
    uint32_t reloaded = 0;

    for (auto& fe : m_watchedFiles) {
        try {
            if (!fs::exists(fe.path)) continue;

            auto currentWrite = fs::last_write_time(fe.path);
            if (currentWrite <= fe.lastWrite) continue;

            VFXDefinition def;
            if (parseFile(fe.path, def)) {
                // If the name changed, remove old entry
                if (def.name != fe.defName) {
                    m_defs.erase(fe.defName);
                    fe.defName = def.name;
                }

                m_defs[def.name] = std::move(def);
                fe.lastWrite = currentWrite;
                reloaded++;

                spdlog::info("[VFXLoader] Hot-reloaded '{}'", fe.defName);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[VFXLoader] Hot-reload error for '{}': {}",
                         fe.path.string(), e.what());
        }
    }

    // Also check for new files added to the directory
    if (!m_dirPath.empty() && fs::exists(m_dirPath)) {
        for (const auto& entry : fs::directory_iterator(m_dirPath)) {
            if (entry.path().extension() != ".json") continue;
            if (entry.is_directory()) continue;

            // Check if already watched
            bool found = false;
            for (const auto& fe : m_watchedFiles) {
                if (fe.path == entry.path()) { found = true; break; }
            }
            if (found) continue;

            VFXDefinition def;
            if (parseFile(entry.path(), def)) {
                FileEntry fe;
                fe.path      = entry.path();
                fe.lastWrite = fs::last_write_time(entry.path());
                fe.defName   = def.name;
                m_watchedFiles.push_back(fe);

                m_defs[def.name] = std::move(def);
                reloaded++;

                spdlog::info("[VFXLoader] Hot-loaded new file '{}'", fe.defName);
            }
        }
    }

    return reloaded;
}

} // namespace glory
