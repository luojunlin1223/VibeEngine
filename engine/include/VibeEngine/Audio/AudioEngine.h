/*
 * AudioEngine — Global audio system powered by miniaudio.
 *
 * Provides clip loading, playback control, and 3D spatial audio.
 * Uses the high-level ma_engine API for mixing, spatialization, and decoding.
 */
#pragma once

#include <string>
#include <cstdint>

namespace VE {

class AudioEngine {
public:
    static void Init();
    static void Shutdown();
    static bool IsInitialized();

    // Master volume (0.0 – 1.0)
    static void  SetMasterVolume(float volume);
    static float GetMasterVolume();

    // Playback control (global)
    static void StopAll();

    // Fire-and-forget playback (simplest, no handle returned)
    static void PlayOneShot(const std::string& filepath);

    // Sound playback — returns a sound handle (0 = failure)
    // Loads the clip on-the-fly if not cached, then plays it.
    static uint32_t Play(const std::string& filepath, float volume = 1.0f,
                         float pitch = 1.0f, bool loop = false);
    static void Stop(uint32_t handle);
    static bool IsPlaying(uint32_t handle);

    // Per-sound control
    static void SetVolume(uint32_t handle, float volume);
    static void SetPitch(uint32_t handle, float pitch);
    static void SetLooping(uint32_t handle, bool loop);

    // 3D spatial audio
    static void SetListenerPosition(const float pos[3], const float forward[3], const float up[3]);
    static void SetSoundPosition(uint32_t handle, const float pos[3]);
    static void SetSoundSpatial(uint32_t handle, bool spatial);
    static void SetSoundMinMaxDistance(uint32_t handle, float minDist, float maxDist);
};

} // namespace VE
