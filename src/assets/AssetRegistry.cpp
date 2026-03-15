#include "assets/AssetRegistry.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace glory {

namespace fs = std::filesystem;

AssetRegistry::AssetRegistry(const std::string& cookedDir,
                             const std::string& rawDir,
                             size_t cacheCapacity)
    : m_cookedDir(cookedDir)
    , m_rawDir(rawDir)
    , m_cacheCapacity(cacheCapacity) {}

void AssetRegistry::scan() {
    m_entries.clear();

    // Scan cooked directory for .glory files
    if (fs::exists(m_cookedDir) && fs::is_directory(m_cookedDir)) {
        for (const auto& entry : fs::recursive_directory_iterator(m_cookedDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".glory") continue;
            std::string name = entry.path().stem().string();
            m_entries[name].cookedPath = entry.path().string();
        }
    }

    // Scan raw directory for .glb files
    if (fs::exists(m_rawDir) && fs::is_directory(m_rawDir)) {
        for (const auto& entry : fs::recursive_directory_iterator(m_rawDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".glb") continue;
            std::string name = entry.path().stem().string();
            m_entries[name].rawPath = entry.path().string();
        }
    }

    spdlog::info("AssetRegistry: scanned {} assets ({} cooked, {} raw)",
                 m_entries.size(),
                 std::count_if(m_entries.begin(), m_entries.end(),
                     [](const auto& p) { return !p.second.cookedPath.empty(); }),
                 std::count_if(m_entries.begin(), m_entries.end(),
                     [](const auto& p) { return !p.second.rawPath.empty(); }));
}

std::optional<std::string> AssetRegistry::resolve(const std::string& assetName) const {
    auto it = m_entries.find(assetName);
    if (it == m_entries.end()) return std::nullopt;

    // Prefer cooked version
    if (!it->second.cookedPath.empty()) return it->second.cookedPath;
    if (!it->second.rawPath.empty())    return it->second.rawPath;
    return std::nullopt;
}

bool AssetRegistry::hasCookedVersion(const std::string& assetName) const {
    auto it = m_entries.find(assetName);
    return it != m_entries.end() && !it->second.cookedPath.empty();
}

std::vector<std::string> AssetRegistry::getAllAssetNames() const {
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (const auto& [name, _] : m_entries) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace glory
