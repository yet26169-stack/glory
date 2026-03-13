#include "audio/AudioResourceManager.h"

#include <spdlog/spdlog.h>
#include <filesystem>

namespace glory {

AudioResourceManager::AudioResourceManager(AudioEngine& engine)
    : m_engine(engine) {}

AudioResourceManager::~AudioResourceManager() {
    stopAll();
    m_sounds.clear();
    m_nameToId.clear();
    spdlog::debug("AudioResourceManager destroyed (stub)");
}

SoundId AudioResourceManager::loadSound(const std::string& path,
                                         SoundGroup group) {
    std::filesystem::path fsPath(path);
    std::string name = fsPath.stem().string();

    if (auto it = m_nameToId.find(name); it != m_nameToId.end()) {
        spdlog::debug("AudioResourceManager: '{}' already loaded as id={}", name, it->second);
        return it->second;
    }

    SoundId id = m_nextId++;
    m_sounds[id] = SoundEntry{id, path, name, group};
    m_nameToId[name] = id;

    spdlog::debug("AudioResourceManager::loadSound '{}' -> id={} (stub, no decode)",
                  path, id);
    // TODO: ma_sound_init_from_file
    return id;
}

void AudioResourceManager::unloadSound(SoundId id) {
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return;

    m_nameToId.erase(it->second.name);
    m_sounds.erase(it);
    spdlog::debug("AudioResourceManager::unloadSound id={} (stub)", id);
    // TODO: ma_sound_uninit
}

SoundHandle AudioResourceManager::play3D(SoundId id,
                                          const glm::vec3& position,
                                          float volume) {
    SoundHandle handle;
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) {
        spdlog::debug("AudioResourceManager::play3D unknown id={}", id);
        return handle;
    }

    handle.m_id       = id;
    handle.m_volume   = volume;
    handle.m_position = position;
    handle.m_playing  = true;

    spdlog::debug("AudioResourceManager::play3D '{}' at [{},{},{}] (stub)",
                  it->second.name, position.x, position.y, position.z);
    // TODO: create ma_sound, set position, start
    return handle;
}

SoundHandle AudioResourceManager::play2D(SoundId id, float volume) {
    SoundHandle handle;
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) {
        spdlog::debug("AudioResourceManager::play2D unknown id={}", id);
        return handle;
    }

    handle.m_id      = id;
    handle.m_volume  = volume;
    handle.m_playing = true;

    spdlog::debug("AudioResourceManager::play2D '{}' (stub)", it->second.name);
    // TODO: create non-spatialized ma_sound, start
    return handle;
}

SoundHandle AudioResourceManager::playMusic(SoundId id, float volume) {
    SoundHandle handle;
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) {
        spdlog::debug("AudioResourceManager::playMusic unknown id={}", id);
        return handle;
    }

    handle.m_id      = id;
    handle.m_volume  = volume;
    handle.m_looping = true;
    handle.m_playing = true;

    spdlog::debug("AudioResourceManager::playMusic '{}' (stub)", it->second.name);
    // TODO: create streaming ma_sound with looping, start
    return handle;
}

void AudioResourceManager::stopAll() {
    spdlog::debug("AudioResourceManager::stopAll (stub)");
    // TODO: iterate active sounds and ma_sound_stop
}

void AudioResourceManager::loadDirectory(const std::string& dirPath,
                                          SoundGroup group) {
    namespace fs = std::filesystem;

    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        spdlog::debug("AudioResourceManager::loadDirectory '{}' not found (stub)", dirPath);
        return;
    }

    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().string();
        if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
            loadSound(entry.path().string(), group);
        }
    }
}

SoundId AudioResourceManager::findSound(const std::string& name) const {
    if (auto it = m_nameToId.find(name); it != m_nameToId.end())
        return it->second;
    return INVALID_SOUND;
}

} // namespace glory
