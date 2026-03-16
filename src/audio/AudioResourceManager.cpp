#include "audio/AudioResourceManager.h"

#include <miniaudio.h>
#include <spdlog/spdlog.h>
#include <filesystem>

namespace glory {

AudioResourceManager::AudioResourceManager(AudioEngine& engine)
    : m_engine(engine) {}

AudioResourceManager::~AudioResourceManager() {
    stopAll();
    m_sounds.clear();
    m_nameToId.clear();
}

// ── Resource registry ──────────────────────────────────────────────────────────

SoundId AudioResourceManager::loadSound(const std::string& path, SoundGroup group) {
    std::filesystem::path fsPath(path);
    std::string name = fsPath.stem().string();

    if (auto it = m_nameToId.find(name); it != m_nameToId.end()) {
        return it->second;
    }

    SoundId id = m_nextId++;
    m_sounds[id] = SoundEntry{id, path, name, group};
    m_nameToId[name] = id;

    spdlog::debug("AudioResourceManager: registered '{}' -> id={}", path, id);
    return id;
}

void AudioResourceManager::unloadSound(SoundId id) {
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return;
    m_nameToId.erase(it->second.name);
    m_sounds.erase(it);
}

void AudioResourceManager::loadDirectory(const std::string& dirPath, SoundGroup group) {
    namespace fs = std::filesystem;
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        spdlog::debug("AudioResourceManager::loadDirectory '{}' not found", dirPath);
        return;
    }

    for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
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

// ── Voice pool ─────────────────────────────────────────────────────────────────

uint32_t AudioResourceManager::acquireVoice(uint8_t priority) {
    // 1. Find a free slot
    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        if (!m_voices[i].active) return i;
    }

    // 2. Find a finished (not playing) slot
    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        auto& v = m_voices[i];
        if (v.sound && !ma_sound_is_playing(static_cast<ma_sound*>(v.sound))) {
            freeVoiceSlot(i);
            return i;
        }
    }

    // 3. Evict lowest-priority, oldest sound
    uint32_t evictIdx = UINT32_MAX;
    uint8_t  lowestPri = 255;
    float    oldestTime = 1e30f;

    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        auto& v = m_voices[i];
        if (v.priority < lowestPri || (v.priority == lowestPri && v.startTime < oldestTime)) {
            if (v.priority <= priority) {  // only evict equal-or-lower priority
                evictIdx   = i;
                lowestPri  = v.priority;
                oldestTime = v.startTime;
            }
        }
    }

    if (evictIdx != UINT32_MAX) {
        freeVoiceSlot(evictIdx);
        return evictIdx;
    }

    return UINT32_MAX;  // all voices are higher priority
}

void AudioResourceManager::freeVoiceSlot(uint32_t slot) {
    auto& v = m_voices[slot];
    if (v.sound) {
        ma_sound_uninit(static_cast<ma_sound*>(v.sound));
        delete static_cast<ma_sound*>(v.sound);
        v.sound = nullptr;
    }
    v.active = false;
    v.owned  = false;
}

void AudioResourceManager::releaseVoice(uint32_t slot) {
    if (slot >= MAX_VOICES) return;
    m_voices[slot].owned = false;
    // Don't free yet — let the sound finish playing. tick() will recycle it.
}

void AudioResourceManager::tick() {
    m_clock += 1.0f / 60.0f;  // approximate; good enough for eviction ordering

    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        auto& v = m_voices[i];
        if (!v.active || v.owned) continue;

        // Not owned by a SoundHandle and finished playing → recycle
        if (v.sound && !ma_sound_is_playing(static_cast<ma_sound*>(v.sound))) {
            freeVoiceSlot(i);
        }
    }
}

SoundHandle AudioResourceManager::initVoice(uint32_t slot, SoundId id, SoundGroup group,
                                              bool spatial, const glm::vec3& pos,
                                              float volume, bool loop) {
    SoundHandle handle;

    auto soundIt = m_sounds.find(id);
    if (soundIt == m_sounds.end()) return handle;

    auto* engine = static_cast<ma_engine*>(m_engine.getEnginePtr());
    if (!engine) return handle;

    auto* maGroup = static_cast<ma_sound_group*>(m_engine.getGroupPtr(group));

    auto* maSound = new ma_sound;

    ma_uint32 flags = MA_SOUND_FLAG_DECODE;  // decode upfront for low-latency
    if (!spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result res = ma_sound_init_from_file(engine, soundIt->second.path.c_str(),
                                             flags, maGroup, nullptr, maSound);
    if (res != MA_SUCCESS) {
        delete maSound;
        spdlog::debug("AudioResourceManager: failed to init sound '{}' ({})",
                      soundIt->second.path, static_cast<int>(res));
        return handle;
    }

    ma_sound_set_volume(maSound, volume);
    ma_sound_set_looping(maSound, loop ? MA_TRUE : MA_FALSE);

    if (spatial) {
        ma_sound_set_position(maSound, pos.x, pos.y, pos.z);
        ma_sound_set_min_distance(maSound, 1.0f);
        ma_sound_set_max_distance(maSound, 100.0f);
        ma_sound_set_attenuation_model(maSound, ma_attenuation_model_inverse);
    }

    ma_sound_start(maSound);

    auto& v      = m_voices[slot];
    v.sound      = maSound;
    v.active     = true;
    v.owned      = true;
    v.startTime  = m_clock;
    v.priority   = loop ? 2 : 1;  // looping sounds have higher priority

    handle.m_id        = id;
    handle.m_sound     = maSound;
    handle.m_manager   = this;
    handle.m_voiceSlot = slot;

    return handle;
}

// ── Playback API ───────────────────────────────────────────────────────────────

SoundHandle AudioResourceManager::play3D(SoundId id, const glm::vec3& position, float volume) {
    if (!m_engine.isInitialized()) return {};
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return {};

    uint32_t slot = acquireVoice(1);
    if (slot == UINT32_MAX) return {};

    return initVoice(slot, id, it->second.group, true, position, volume, false);
}

SoundHandle AudioResourceManager::play2D(SoundId id, float volume) {
    if (!m_engine.isInitialized()) return {};
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return {};

    uint32_t slot = acquireVoice(1);
    if (slot == UINT32_MAX) return {};

    return initVoice(slot, id, it->second.group, false, {}, volume, false);
}

SoundHandle AudioResourceManager::playMusic(SoundId id, float volume) {
    if (!m_engine.isInitialized()) return {};
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return {};

    uint32_t slot = acquireVoice(3);  // music = highest priority
    if (slot == UINT32_MAX) return {};

    return initVoice(slot, id, SoundGroup::Music, false, {}, volume, true);
}

void AudioResourceManager::stopAll() {
    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        if (m_voices[i].active) {
            if (m_voices[i].sound) {
                ma_sound_stop(static_cast<ma_sound*>(m_voices[i].sound));
            }
            freeVoiceSlot(i);
        }
    }
}

} // namespace glory
