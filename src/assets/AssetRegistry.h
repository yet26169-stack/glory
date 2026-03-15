#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <filesystem>

namespace glory {

class AssetRegistry {
public:
    explicit AssetRegistry(const std::string& cookedDir,
                           const std::string& rawDir,
                           size_t cacheCapacity = 64);

    // Resolve asset name to file path (.glory preferred, .glb fallback)
    std::optional<std::string> resolve(const std::string& assetName) const;

    // Scan directories and build registry
    void scan();

    // Check if a cooked version exists for the given asset
    bool hasCookedVersion(const std::string& assetName) const;

    // Get all registered asset names
    std::vector<std::string> getAllAssetNames() const;

private:
    std::string m_cookedDir;
    std::string m_rawDir;
    size_t      m_cacheCapacity;

    struct AssetEntry {
        std::string cookedPath;  // .glory file path (empty if not cooked)
        std::string rawPath;     // .glb file path (empty if not found)
    };
    std::unordered_map<std::string, AssetEntry> m_entries;
};

} // namespace glory
