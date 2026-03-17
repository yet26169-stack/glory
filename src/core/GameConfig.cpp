#include "core/GameConfig.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>

namespace glory {

using json = nlohmann::json;

// ── Load from JSON ──────────────────────────────────────────────────────────

bool GameConfig::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::info("[Config] '{}' not found — using defaults", path);
        return false;
    }

    try {
        json j = json::parse(f, nullptr, /*allow_exceptions=*/true,
                             /*ignore_comments=*/true);

        // Display
        if (j.contains("windowWidth"))   windowWidth   = j["windowWidth"].get<int>();
        if (j.contains("windowHeight"))  windowHeight  = j["windowHeight"].get<int>();
        if (j.contains("fullscreen"))    fullscreen    = j["fullscreen"].get<bool>();
        if (j.contains("targetFps"))     targetFps     = j["targetFps"].get<int>();
        if (j.contains("vsync"))         vsync         = j["vsync"].get<bool>();

        // Audio
        if (j.contains("masterVolume"))  masterVolume  = j["masterVolume"].get<float>();
        if (j.contains("sfxVolume"))     sfxVolume     = j["sfxVolume"].get<float>();
        if (j.contains("musicVolume"))   musicVolume   = j["musicVolume"].get<float>();

        // Paths
        if (j.contains("mapModelsDir"))      mapModelsDir      = j["mapModelsDir"].get<std::string>();
        if (j.contains("characterModelDir")) characterModelDir = j["characterModelDir"].get<std::string>();
        if (j.contains("assetDir"))          assetDir          = j["assetDir"].get<std::string>();

        // Keybindings
        if (j.contains("keybindings")) {
            auto& kb = j["keybindings"];
            if (kb.contains("abilityQ")) keyAbilityQ = kb["abilityQ"].get<int>();
            if (kb.contains("abilityW")) keyAbilityW = kb["abilityW"].get<int>();
            if (kb.contains("abilityE")) keyAbilityE = kb["abilityE"].get<int>();
            if (kb.contains("abilityR")) keyAbilityR = kb["abilityR"].get<int>();
            if (kb.contains("abilityD")) keyAbilityD = kb["abilityD"].get<int>();
            if (kb.contains("ward"))     keyWard     = kb["ward"].get<int>();
            if (kb.contains("spawn"))    keySpawn    = kb["spawn"].get<int>();
            if (kb.contains("recall"))   keyRecall   = kb["recall"].get<int>();
        }

        // Rendering
        if (j.contains("renderQuality")) renderQuality = j["renderQuality"].get<int>();
        if (j.contains("bloomEnabled"))  bloomEnabled  = j["bloomEnabled"].get<bool>();
        if (j.contains("fowEnabled"))    fowEnabled    = j["fowEnabled"].get<bool>();

        // Gameplay
        if (j.contains("cameraZoom"))    cameraZoom    = j["cameraZoom"].get<float>();

        spdlog::info("[Config] Loaded from '{}'", path);
        return true;

    } catch (const json::exception& e) {
        spdlog::warn("[Config] Parse error in '{}': {} — using defaults", path, e.what());
        return false;
    }
}

// ── Save to JSON ────────────────────────────────────────────────────────────

bool GameConfig::saveToFile(const std::string& path) const {
    json j;

    j["windowWidth"]   = windowWidth;
    j["windowHeight"]  = windowHeight;
    j["fullscreen"]    = fullscreen;
    j["targetFps"]     = targetFps;
    j["vsync"]         = vsync;

    j["masterVolume"]  = masterVolume;
    j["sfxVolume"]     = sfxVolume;
    j["musicVolume"]   = musicVolume;

    if (!mapModelsDir.empty())      j["mapModelsDir"]      = mapModelsDir;
    if (!characterModelDir.empty()) j["characterModelDir"] = characterModelDir;
    if (!assetDir.empty())          j["assetDir"]          = assetDir;

    json kb;
    kb["abilityQ"] = keyAbilityQ;
    kb["abilityW"] = keyAbilityW;
    kb["abilityE"] = keyAbilityE;
    kb["abilityR"] = keyAbilityR;
    kb["abilityD"] = keyAbilityD;
    kb["ward"]     = keyWard;
    kb["spawn"]    = keySpawn;
    kb["recall"]   = keyRecall;
    j["keybindings"] = kb;

    j["renderQuality"] = renderQuality;
    j["bloomEnabled"]  = bloomEnabled;
    j["fowEnabled"]    = fowEnabled;
    j["cameraZoom"]    = cameraZoom;

    std::ofstream out(path);
    if (!out.is_open()) {
        spdlog::warn("[Config] Cannot write to '{}'", path);
        return false;
    }
    out << j.dump(4) << "\n";
    spdlog::info("[Config] Saved to '{}'", path);
    return true;
}

// ── CLI overrides ───────────────────────────────────────────────────────────

void GameConfig::applyCliOverrides(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            windowWidth = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            windowHeight = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else if (std::strcmp(argv[i], "--windowed") == 0) {
            fullscreen = false;
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            targetFps = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--map-models-dir") == 0 && i + 1 < argc) {
            mapModelsDir = argv[++i];
        } else if (std::strcmp(argv[i], "--asset-dir") == 0 && i + 1 < argc) {
            assetDir = argv[++i];
        } else if (std::strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            renderQuality = std::stoi(argv[++i]);
        }
    }
}

// ── Effective paths ─────────────────────────────────────────────────────────

std::string GameConfig::getAssetDir() const {
    if (!assetDir.empty()) return assetDir;
#ifdef ASSET_DIR
    return ASSET_DIR;
#else
    return "assets/";
#endif
}

std::string GameConfig::getModelDir() const {
    if (!mapModelsDir.empty()) return mapModelsDir;
#ifdef MODEL_DIR
    return MODEL_DIR;
#else
    return "./";
#endif
}

} // namespace glory
