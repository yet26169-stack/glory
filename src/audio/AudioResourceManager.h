#pragma once

#include "audio/AudioEngine.h"
#include "audio/SoundHandle.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cstdint>

namespace glory {

class AudioResourceManager {
public:
    static constexpr uint32_t MAX_VOICES = 32;

    explicit AudioResourceManager(AudioEngine& engine);
    ~AudioResourceManager();

    /// Set base directory for resolving relative sound paths (e.g. ASSET_DIR + "audio/").
    void setBasePath(const std::string& basePath) { m_basePath = basePath; }

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

    /// Called each frame to recycle finished voice slots.
    void tick();

    /// Release a voice slot (called by SoundHandle destructor — does NOT stop the sound).
    void releaseVoice(uint32_t slot);

private:
    AudioEngine& m_engine;

    struct SoundEntry {
        SoundId     id;
        std::string path;
        std::string name;
        SoundGroup  group;
    };

    std::unordered_map<SoundId, SoundEntry>   m_sounds;
    std::unordered_map<std::string, SoundId>  m_nameToId;
    std::unordered_set<SoundId>               m_failedSounds;
    std::string                               m_basePath;
    SoundId m_nextId = 1;

    struct VoiceSlot {
        void*  sound       = nullptr;  // ma_sound* (heap-allocated)
        bool   active      = false;    // currently in use
        bool   owned       = false;    // a SoundHandle still references this slot
        float  startTime   = 0.0f;
        uint8_t priority   = 0;        // higher = harder to evict
    };

    std::array<VoiceSlot, MAX_VOICES> m_voices{};
    float m_clock = 0.0f;

    uint32_t acquireVoice(uint8_t priority);
    void     freeVoiceSlot(uint32_t slot);
    SoundHandle initVoice(uint32_t slot, SoundId id, SoundGroup group,
                          bool spatial, const glm::vec3& pos, float volume, bool loop);
};

} // namespace glory
