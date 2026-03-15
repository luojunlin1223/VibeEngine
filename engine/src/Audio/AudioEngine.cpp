/*
 * AudioEngine implementation — wraps miniaudio's high-level engine API.
 *
 * Each Play() call creates a ma_sound from file. Sounds are tracked by handle
 * in a map so they can be controlled individually.  Finished (non-looping)
 * sounds are cleaned up lazily on the next Play/Stop/StopAll call.
 */

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "VibeEngine/Audio/AudioEngine.h"
#include "VibeEngine/Core/Log.h"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <filesystem>

namespace VE {

static ma_engine* s_Engine = nullptr;
static uint32_t   s_NextHandle = 1;
static float      s_MasterVolume = 1.0f;

struct SoundEntry {
    ma_sound  Sound;
    bool      Initialized = false;
};

static std::unordered_map<uint32_t, SoundEntry*> s_Sounds;
static std::mutex s_Mutex;

// Remove finished (non-looping) sounds
static void PruneSounds() {
    std::vector<uint32_t> toRemove;
    for (auto& [h, entry] : s_Sounds) {
        if (entry->Initialized && !ma_sound_is_looping(&entry->Sound) &&
            ma_sound_at_end(&entry->Sound)) {
            toRemove.push_back(h);
        }
    }
    for (uint32_t h : toRemove) {
        auto it = s_Sounds.find(h);
        if (it != s_Sounds.end()) {
            ma_sound_uninit(&it->second->Sound);
            delete it->second;
            s_Sounds.erase(it);
        }
    }
}

void AudioEngine::Init() {
    if (s_Engine) return;

    s_Engine = new ma_engine();
    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;

    ma_result result = ma_engine_init(&cfg, s_Engine);
    if (result != MA_SUCCESS) {
        VE_ERROR("Failed to initialize audio engine (error {})", (int)result);
        delete s_Engine;
        s_Engine = nullptr;
        return;
    }

    VE_INFO("Audio engine initialized");
}

void AudioEngine::Shutdown() {
    if (!s_Engine) return;

    // Clean up all active sounds
    for (auto& [h, entry] : s_Sounds) {
        if (entry->Initialized)
            ma_sound_uninit(&entry->Sound);
        delete entry;
    }
    s_Sounds.clear();

    ma_engine_uninit(s_Engine);
    delete s_Engine;
    s_Engine = nullptr;

    VE_INFO("Audio engine shut down");
}

bool AudioEngine::IsInitialized() {
    return s_Engine != nullptr;
}

void AudioEngine::SetMasterVolume(float volume) {
    s_MasterVolume = volume;
    if (s_Engine)
        ma_engine_set_volume(s_Engine, volume);
}

float AudioEngine::GetMasterVolume() {
    return s_MasterVolume;
}

void AudioEngine::StopAll() {
    if (!s_Engine) return;

    for (auto& [h, entry] : s_Sounds) {
        if (entry->Initialized) {
            ma_sound_stop(&entry->Sound);
            ma_sound_uninit(&entry->Sound);
        }
        delete entry;
    }
    s_Sounds.clear();
}

void AudioEngine::PlayOneShot(const std::string& filepath) {
    if (!s_Engine || filepath.empty()) return;
    ma_engine_play_sound(s_Engine, filepath.c_str(), nullptr);
}

uint32_t AudioEngine::Play(const std::string& filepath, float volume, float pitch, bool loop) {
    if (filepath.empty()) {
        VE_ENGINE_WARN("AudioEngine::Play: filepath is empty");
        return 0;
    }
    if (!s_Engine) {
        VE_ENGINE_ERROR("AudioEngine::Play: audio engine not initialized");
        return 0;
    }
    if (!std::filesystem::exists(filepath)) {
        VE_ENGINE_ERROR("AudioEngine::Play: file not found: {0}", filepath);
        return 0;
    }

    PruneSounds();

    auto* entry = new SoundEntry();
    ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    ma_result result = ma_sound_init_from_file(s_Engine, filepath.c_str(),
        flags, nullptr, nullptr, &entry->Sound);
    if (result != MA_SUCCESS) {
        VE_WARN("Failed to load audio: {} (error {})", filepath, (int)result);
        delete entry;
        return 0;
    }

    entry->Initialized = true;
    ma_sound_set_volume(&entry->Sound, volume);
    ma_sound_set_pitch(&entry->Sound, pitch);
    ma_sound_set_looping(&entry->Sound, loop);
    ma_sound_start(&entry->Sound);

    uint32_t handle = s_NextHandle++;
    s_Sounds[handle] = entry;
    return handle;
}

void AudioEngine::Stop(uint32_t handle) {
    if (!s_Engine) return;
    auto it = s_Sounds.find(handle);
    if (it == s_Sounds.end()) return;

    if (it->second->Initialized) {
        ma_sound_stop(&it->second->Sound);
        ma_sound_uninit(&it->second->Sound);
    }
    delete it->second;
    s_Sounds.erase(it);
}

bool AudioEngine::IsPlaying(uint32_t handle) {
    auto it = s_Sounds.find(handle);
    if (it == s_Sounds.end()) return false;
    return it->second->Initialized && ma_sound_is_playing(&it->second->Sound);
}

void AudioEngine::SetVolume(uint32_t handle, float volume) {
    auto it = s_Sounds.find(handle);
    if (it != s_Sounds.end() && it->second->Initialized)
        ma_sound_set_volume(&it->second->Sound, volume);
}

void AudioEngine::SetPitch(uint32_t handle, float pitch) {
    auto it = s_Sounds.find(handle);
    if (it != s_Sounds.end() && it->second->Initialized)
        ma_sound_set_pitch(&it->second->Sound, pitch);
}

void AudioEngine::SetLooping(uint32_t handle, bool loop) {
    auto it = s_Sounds.find(handle);
    if (it != s_Sounds.end() && it->second->Initialized)
        ma_sound_set_looping(&it->second->Sound, loop);
}

// 3D spatial audio

void AudioEngine::SetListenerPosition(const float pos[3], const float forward[3], const float up[3]) {
    if (!s_Engine) return;
    ma_engine_listener_set_position(s_Engine, 0, pos[0], pos[1], pos[2]);
    ma_engine_listener_set_direction(s_Engine, 0, forward[0], forward[1], forward[2]);
    ma_engine_listener_set_world_up(s_Engine, 0, up[0], up[1], up[2]);
}

void AudioEngine::SetSoundPosition(uint32_t handle, const float pos[3]) {
    auto it = s_Sounds.find(handle);
    if (it != s_Sounds.end() && it->second->Initialized)
        ma_sound_set_position(&it->second->Sound, pos[0], pos[1], pos[2]);
}

void AudioEngine::SetSoundSpatial(uint32_t handle, bool spatial) {
    auto it = s_Sounds.find(handle);
    if (it != s_Sounds.end() && it->second->Initialized)
        ma_sound_set_spatialization_enabled(&it->second->Sound, spatial);
}

void AudioEngine::SetSoundMinMaxDistance(uint32_t handle, float minDist, float maxDist) {
    auto it = s_Sounds.find(handle);
    if (it != s_Sounds.end() && it->second->Initialized) {
        ma_sound_set_min_distance(&it->second->Sound, minDist);
        ma_sound_set_max_distance(&it->second->Sound, maxDist);
    }
}

} // namespace VE
