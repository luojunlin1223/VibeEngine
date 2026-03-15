/*
 * VideoPlayer -- MPEG1 video texture playback using pl_mpeg.
 *
 * Decodes .mpg files and uploads each video frame to an OpenGL texture
 * that can be used anywhere a Texture2D is expected (mesh rendering,
 * UI images, etc.).  Decode runs at the video's native framerate,
 * driven by delta-time accumulation.
 */
#pragma once

#include <string>
#include <memory>
#include <cstdint>

namespace VE {

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // Non-copyable
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    /// Load an MPEG-1 (.mpg) file.  Returns true on success.
    bool Open(const std::string& path);

    /// Release all resources (decoder + texture).
    void Close();

    /// Advance playback by `deltaTime` seconds.
    /// Decodes frame(s) as needed and uploads RGB data to the GPU texture.
    void Update(float deltaTime);

    // -- Transport controls --------------------------------------------------
    void Play();
    void Pause();
    void Stop();
    void Seek(double timeSeconds);

    void SetLooping(bool loop);
    bool IsLooping() const { return m_Loop; }

    bool IsPlaying() const { return m_Playing; }
    bool IsOpen()    const { return m_Decoder != nullptr; }

    double GetDuration()    const;
    double GetCurrentTime() const;
    float  GetFramerate()   const;

    uint32_t GetWidth()  const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

    /// Returns the OpenGL texture ID that receives each decoded frame.
    /// Valid only while the player is open and at least one frame has been decoded.
    uint32_t GetTextureID() const { return m_TextureID; }

    /// Returns the texture ID as uint64 (suitable for ImGui::Image).
    uint64_t GetNativeTextureID() const { return static_cast<uint64_t>(m_TextureID); }

    /// Upload a decoded video frame to the GPU texture.
    /// The `frame` pointer is a plm_frame_t* (passed as void* to avoid header leak).
    void UploadFrame(void* frame);

private:
    void CreateTexture();
    void DestroyTexture();

    void*    m_Decoder    = nullptr; // plm_t* (opaque to avoid header leak)
    uint32_t m_TextureID  = 0;
    uint32_t m_Width      = 0;
    uint32_t m_Height     = 0;

    uint8_t* m_RGBBuffer  = nullptr; // CPU-side RGB buffer (width * height * 3)

    bool   m_Playing = false;
    bool   m_Loop    = false;

    std::string m_FilePath;
};

} // namespace VE
