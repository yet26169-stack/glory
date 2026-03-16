#pragma once

// Data-driven VFX Definition Loader with hot-reload support.
// Loads assets/vfx/*.json into a registry of VFXDefinition structs.
// Monitors file modification times for live reloading during development.

#include "vfx/VFXDefinition.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace glory {

class VFXDefinitionLoader {
public:
    // Load all *.json files from a directory into the registry.
    void loadDirectory(const std::string& dirPath);

    // Look up a definition by name. Returns nullptr if not found.
    const VFXDefinition* getDefinition(const std::string& name) const;

    // Check for modified files and re-parse them. Call once per frame.
    // Returns the number of definitions that were reloaded.
    uint32_t hotReload();

    // Get all loaded definition names.
    std::vector<std::string> getLoadedNames() const;

    uint32_t size() const { return static_cast<uint32_t>(m_defs.size()); }

private:
    struct FileEntry {
        std::filesystem::path                    path;
        std::filesystem::file_time_type          lastWrite;
        std::string                              defName;  // key into m_defs
    };

    std::unordered_map<std::string, VFXDefinition> m_defs;
    std::vector<FileEntry>                         m_watchedFiles;
    std::string                                    m_dirPath;

    // Parse a single JSON file into a VFXDefinition. Returns true on success.
    bool parseFile(const std::filesystem::path& filePath, VFXDefinition& out);
};

} // namespace glory
