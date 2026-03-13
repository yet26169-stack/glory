#pragma once

#include "audio/AudioEngine.h"
#include "audio/SoundHandle.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace glory {

class AudioResourceManager {
public:
    explicit AudioResourceManager(AudioEngine& engine);
    ~AudioResourceManager();

    SoundId loadSound(const std::string& path,
                      SoundGroup group = SoundGroup::SFX);

    void unloadSound(SoundId id);

    SoundHandle play3D(SoundId id, const glm::vec3& position,
                       float volume = 1.0f);

    SoundHandle play2D(SoundId id, float volume = 1.0f);

    SoundHandle playMusic(SoundId id, float volume = 0.7f);

    void stopAll();

    void loadDirectory(const std::string& dirPath,
                       SoundGroup group = SoundGroup::SFX);

    SoundId findSound(const std::string& name) const;

private:
    AudioEngine& m_engine;

    struct SoundEntry {
        SoundId    id;
        std::string path;
        std::string name;
        SoundGroup  group;
    };

    std::unordered_map<SoundId, SoundEntry>   m_sounds;
    std::unordered_map<std::string, SoundId>  m_nameToId;
    SoundId m_nextId = 1;
};

} // namespace glory
