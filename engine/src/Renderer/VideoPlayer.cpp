/*
 * VideoPlayer -- MPEG1 video texture playback implementation.
 *
 * Uses pl_mpeg to decode video frames and uploads them to an OpenGL texture
 * via glTexSubImage2D every time a new frame is available.
 * The high-level plm_decode() drives the internal clock; a video callback
 * captures each decoded frame and converts YCrCb -> RGB -> GPU upload.
 */
#include "VibeEngine/Renderer/VideoPlayer.h"
#include "VibeEngine/Core/Log.h"

#include <pl_mpeg.h>
#include <glad/gl.h>
#include <cstring>

namespace VE {

// Static callback forwarded from pl_mpeg for each decoded video frame.
static void OnVideoDecoded(plm_t* /*plm*/, plm_frame_t* frame, void* user) {
    auto* player = static_cast<VideoPlayer*>(user);
    player->UploadFrame(frame);
}

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() {
    Close();
}

bool VideoPlayer::Open(const std::string& path) {
    // Close any previous file
    Close();

    plm_t* plm = plm_create_with_filename(path.c_str());
    if (!plm) {
        VE_ENGINE_ERROR("VideoPlayer: failed to open '{0}'", path);
        return false;
    }

    m_Decoder = plm;
    m_FilePath = path;
    m_Width  = static_cast<uint32_t>(plm_get_width(plm));
    m_Height = static_cast<uint32_t>(plm_get_height(plm));

    if (m_Width == 0 || m_Height == 0) {
        VE_ENGINE_ERROR("VideoPlayer: invalid dimensions in '{0}'", path);
        Close();
        return false;
    }

    // We only need video frames -- disable audio decoding
    plm_set_audio_enabled(plm, 0);

    // Apply loop setting
    plm_set_loop(plm, m_Loop ? 1 : 0);

    // Install the video decode callback so plm_decode() uploads each frame
    plm_set_video_decode_callback(plm, OnVideoDecoded, this);

    // Allocate CPU-side RGB buffer for YCrCb -> RGB conversion
    m_RGBBuffer = std::make_unique<uint8_t[]>(m_Width * m_Height * 3);
    std::memset(m_RGBBuffer.get(), 0, m_Width * m_Height * 3);

    // Create the GPU texture
    CreateTexture();

    VE_ENGINE_INFO("VideoPlayer: opened '{0}' ({1}x{2}, {3:.1f}s, {4:.1f} fps)",
                   path, m_Width, m_Height, GetDuration(), GetFramerate());
    return true;
}

void VideoPlayer::Close() {
    m_Playing = false;

    if (m_Decoder) {
        plm_destroy(static_cast<plm_t*>(m_Decoder));
        m_Decoder = nullptr;
    }

    DestroyTexture();

    m_RGBBuffer.reset();

    m_Width  = 0;
    m_Height = 0;
    m_FilePath.clear();
}

void VideoPlayer::Update(float deltaTime) {
    if (!m_Decoder || !m_Playing) return;

    plm_t* plm = static_cast<plm_t*>(m_Decoder);

    // plm_decode advances the internal clock by deltaTime seconds and decodes
    // as many video frames as needed to keep up.  Each decoded frame triggers
    // the OnVideoDecoded callback which calls UploadFrame().
    plm_decode(plm, static_cast<double>(deltaTime));

    // Check if playback has ended (non-looping)
    if (plm_has_ended(plm)) {
        m_Playing = false;
    }
}

void VideoPlayer::Play() {
    if (!m_Decoder) return;
    m_Playing = true;
}

void VideoPlayer::Pause() {
    m_Playing = false;
}

void VideoPlayer::Stop() {
    if (!m_Decoder) return;
    m_Playing = false;
    plm_rewind(static_cast<plm_t*>(m_Decoder));
}

void VideoPlayer::Seek(double timeSeconds) {
    if (!m_Decoder) return;
    plm_seek(static_cast<plm_t*>(m_Decoder), timeSeconds, 1);
}

void VideoPlayer::SetLooping(bool loop) {
    m_Loop = loop;
    if (m_Decoder) {
        plm_set_loop(static_cast<plm_t*>(m_Decoder), loop ? 1 : 0);
    }
}

double VideoPlayer::GetDuration() const {
    return m_Decoder ? plm_get_duration(static_cast<plm_t*>(m_Decoder)) : 0.0;
}

double VideoPlayer::GetCurrentTime() const {
    return m_Decoder ? plm_get_time(static_cast<plm_t*>(m_Decoder)) : 0.0;
}

float VideoPlayer::GetFramerate() const {
    return m_Decoder ? static_cast<float>(plm_get_framerate(static_cast<plm_t*>(m_Decoder))) : 0.0f;
}

void VideoPlayer::CreateTexture() {
    glGenTextures(1, &m_TextureID);
    glBindTexture(GL_TEXTURE_2D, m_TextureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Allocate storage (RGB8) -- initial content is undefined; first frame fills it
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8,
                 static_cast<GLsizei>(m_Width),
                 static_cast<GLsizei>(m_Height),
                 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoPlayer::DestroyTexture() {
    if (m_TextureID) {
        glDeleteTextures(1, &m_TextureID);
        m_TextureID = 0;
    }
}

void VideoPlayer::UploadFrame(void* framePtr) {
    plm_frame_t* frame = static_cast<plm_frame_t*>(framePtr);
    if (!frame || !m_RGBBuffer || !m_TextureID) return;

    // Convert YCrCb to RGB into the CPU buffer
    plm_frame_to_rgb(frame, m_RGBBuffer.get(), static_cast<int>(m_Width * 3));

    // Upload to GPU texture
    glBindTexture(GL_TEXTURE_2D, m_TextureID);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    static_cast<GLsizei>(m_Width),
                    static_cast<GLsizei>(m_Height),
                    GL_RGB, GL_UNSIGNED_BYTE, m_RGBBuffer.get());
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace VE
