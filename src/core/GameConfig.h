#pragma once
/// GameConfig: Central configuration loaded from config.json with CLI overrides.
/// Covers resolution, fullscreen, audio, keybindings, model paths, and rendering.

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

namespace glory {

struct GameConfig {
    // ── Display ──────────────────────────────────────────────────────────
    int  windowWidth   = 1280;
    int  windowHeight  = 720;
    bool fullscreen    = false;
    int  targetFps     = 60;
    bool vsync         = true;

    // ── Audio ────────────────────────────────────────────────────────────
    float masterVolume = 1.0f;   // 0.0–1.0
    float sfxVolume    = 1.0f;
    float musicVolume  = 0.7f;

    // ── Paths (override CMake defaults at runtime) ───────────────────────
    std::string mapModelsDir;       // if non-empty, overrides MODEL_DIR for maps
    std::string characterModelDir;  // if non-empty, overrides MODEL_DIR for characters
    std::string assetDir;           // if non-empty, overrides ASSET_DIR

    // ── Keybindings (GLFW key codes) ─────────────────────────────────────
    int keyAbilityQ    = 81;  // GLFW_KEY_Q
    int keyAbilityW    = 87;  // GLFW_KEY_W
    int keyAbilityE    = 69;  // GLFW_KEY_E
    int keyAbilityR    = 82;  // GLFW_KEY_R
    int keyAbilityD    = 68;  // GLFW_KEY_D
    int keyWard        = 70;  // GLFW_KEY_F
    int keySpawn       = 88;  // GLFW_KEY_X
    int keyRecall      = 66;  // GLFW_KEY_B

    // ── Rendering ────────────────────────────────────────────────────────
    int  renderQuality = 2;   // 0=low, 1=medium, 2=high
    bool bloomEnabled  = true;
    bool fowEnabled    = true;

    // ── Gameplay ─────────────────────────────────────────────────────────
    float cameraZoom   = 18.0f;

    // ── Load / Save ──────────────────────────────────────────────────────

    /// Load from a JSON file. Missing fields keep their defaults.
    /// Returns true if the file was loaded (even partially).
    bool loadFromFile(const std::string& path);

    /// Save current config to a JSON file.
    bool saveToFile(const std::string& path) const;

    /// Apply command-line overrides (call after loadFromFile).
    /// Recognized flags: --width, --height, --fullscreen, --windowed,
    ///   --fps, --config, --map-models-dir, --asset-dir, --quality
    void applyCliOverrides(int argc, char* argv[]);

    /// Get the effective asset directory (config override or compile-time).
    std::string getAssetDir() const;

    /// Get the effective model directory (config override or compile-time).
    std::string getModelDir() const;
};

} // namespace glory
