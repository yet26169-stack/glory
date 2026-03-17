#include "core/Log.h"
#include "core/Application.h"
#include "core/GameConfig.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

static void printUsage(const char* prog) {
    spdlog::info("Usage: {} [options]", prog);
    spdlog::info("  --server              Start as server (host)");
    spdlog::info("  --connect <ip>        Connect to server at <ip>");
    spdlog::info("  --port <port>         Network port (default 7777)");
    spdlog::info("  --players <n>         Number of players (default 2)");
    spdlog::info("  --config <path>       Path to config.json");
    spdlog::info("  --width <w>           Window width");
    spdlog::info("  --height <h>          Window height");
    spdlog::info("  --fullscreen          Start in fullscreen mode");
    spdlog::info("  --windowed            Start in windowed mode");
    spdlog::info("  --fps <n>             Target frame rate");
    spdlog::info("  --map-models-dir <p>  Override map models directory");
    spdlog::info("  --asset-dir <p>       Override asset directory");
    spdlog::info("  --quality <0-2>       Render quality (0=low, 1=med, 2=high)");
}

int main(int argc, char* argv[]) {
    glory::Log::init();

    // ── Load config.json (before CLI parsing so CLI can override) ────────
    glory::GameConfig gameConfig;
    std::string configPath = "config.json";  // default

    // Pre-scan for --config flag
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
            break;
        }
    }
    gameConfig.loadFromFile(configPath);

    // Try the asset-dir copy as fallback
    if (configPath == "config.json") {
        std::string assetConfig = gameConfig.getAssetDir() + "config/config.json";
        gameConfig.loadFromFile(assetConfig);
    }

    gameConfig.applyCliOverrides(argc, argv);

    // ── Parse network config ─────────────────────────────────────────────
    glory::NetworkConfig netCfg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0) {
            netCfg.role = glory::NetworkRole::Server;
        } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            netCfg.role = glory::NetworkRole::Client;
            netCfg.host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            netCfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--players") == 0 && i + 1 < argc) {
            netCfg.playerCount = static_cast<uint8_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return EXIT_SUCCESS;
        }
        // --config, --width, --height, etc. already handled by applyCliOverrides
    }

    try {
        glory::Application app("Glory Engine", netCfg, gameConfig);
        app.run();
    } catch (const std::exception& e) {
        spdlog::critical("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
