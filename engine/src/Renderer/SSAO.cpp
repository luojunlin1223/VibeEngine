/*
 * SSAO implementation — screen-space ambient occlusion with blur.
 *
 * Algorithm:
 *   1. Reconstruct view-space position from depth buffer
 *   2. For each fragment, sample N random points in a hemisphere
 *   3. Compare sample depth to actual depth → count occlusions
 *   4. Blur the raw AO to reduce noise
 */
#include "VibeEngine/Renderer/SSAO.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>
#include <random>
#include <algorithm>

namespace VE {

// ── Shader sources ───────────────────────────────────────────────────

static const char* s_QuadVertSrc = R"(
#version 450 core
layout(location = 0) out vec2 v_UV;
void main() {
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* s_SSAOFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out float FragColor;

uniform sampler2D u_DepthMap;
uniform sampler2D u_NoiseTex;
uniform vec3      u_Samples[64];
uniform int       u_KernelSize;
uniform float     u_Radius;
uniform float     u_Bias;
uniform float     u_Intensity;
uniform mat4      u_Projection;
uniform mat4      u_View;
uniform vec2      u_ScreenSize;

// Reconstruct view-space position from depth
vec3 ViewPosFromDepth(vec2 uv, float depth) {
    // NDC
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = inverse(u_Projection) * ndc;
    return viewPos.xyz / viewPos.w;
}

void main() {
    float depth = texture(u_DepthMap, v_UV).r;
    if (depth >= 1.0) {
        FragColor = 1.0; // sky — no occlusion
        return;
    }

    vec3 fragPos = ViewPosFromDepth(v_UV, depth);

    // Approximate view-space normal from depth derivatives
    vec2 texelSize = 1.0 / u_ScreenSize;
    float dR = texture(u_DepthMap, v_UV + vec2(texelSize.x, 0.0)).r;
    float dL = texture(u_DepthMap, v_UV - vec2(texelSize.x, 0.0)).r;
    float dU = texture(u_DepthMap, v_UV + vec2(0.0, texelSize.y)).r;
    float dD = texture(u_DepthMap, v_UV - vec2(0.0, texelSize.y)).r;

    vec3 posR = ViewPosFromDepth(v_UV + vec2(texelSize.x, 0.0), dR);
    vec3 posL = ViewPosFromDepth(v_UV - vec2(texelSize.x, 0.0), dL);
    vec3 posU = ViewPosFromDepth(v_UV + vec2(0.0, texelSize.y), dU);
    vec3 posD = ViewPosFromDepth(v_UV - vec2(0.0, texelSize.y), dD);

    vec3 normal = normalize(cross(posR - posL, posU - posD));

    // Random rotation from noise texture (4x4 tiled)
    vec2 noiseScale = u_ScreenSize / 4.0;
    vec3 randomVec = normalize(texture(u_NoiseTex, v_UV * noiseScale).xyz * 2.0 - 1.0);

    // TBN basis (Gram-Schmidt)
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < u_KernelSize; i++) {
        // Sample position in view space
        vec3 samplePos = fragPos + TBN * u_Samples[i] * u_Radius;

        // Project to screen
        vec4 offset = u_Projection * vec4(samplePos, 1.0);
        offset.xy /= offset.w;
        offset.xy = offset.xy * 0.5 + 0.5;

        // Compare depths
        float sampleDepth = texture(u_DepthMap, offset.xy).r;
        vec3 sampleViewPos = ViewPosFromDepth(offset.xy, sampleDepth);

        // Range check + occlusion
        float rangeCheck = smoothstep(0.0, 1.0, u_Radius / abs(fragPos.z - sampleViewPos.z));
        occlusion += (sampleViewPos.z >= samplePos.z + u_Bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(u_KernelSize)) * u_Intensity;
    FragColor = occlusion;
}
)";

static const char* s_BlurFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out float FragColor;

uniform sampler2D u_Input;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(u_Input, 0));
    float result = 0.0;
    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            result += texture(u_Input, v_UV + vec2(float(x), float(y)) * texelSize).r;
        }
    }
    FragColor = result / 25.0;
}
)";

// ── Helpers ──────────────────────────────────────────────────────────

static uint32_t CompileShader(GLenum type, const char* src) {
    uint32_t shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        VE_ENGINE_ERROR("SSAO shader compile error: {}", log);
    }
    return shader;
}

static uint32_t LinkProgram(const char* vertSrc, const char* fragSrc) {
    uint32_t vert = CompileShader(GL_VERTEX_SHADER, vertSrc);
    uint32_t frag = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    uint32_t prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, 512, nullptr, log);
        VE_ENGINE_ERROR("SSAO program link error: {}", log);
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

// ── SSAO Implementation ─────────────────────────────────────────────

SSAO::~SSAO() {
    Shutdown();
}

void SSAO::Init(uint32_t width, uint32_t height) {
    if (m_Initialized) return;
    m_Width = width;
    m_Height = height;

    CompileShaders();
    glGenVertexArrays(1, &m_QuadVAO);
    GenerateKernel();
    GenerateNoiseTexture();
    CreateResources();

    m_Initialized = true;
    VE_ENGINE_INFO("SSAO initialized ({}x{})", width, height);
}

void SSAO::Shutdown() {
    if (!m_Initialized) return;
    DestroyResources();
    if (m_QuadVAO) { glDeleteVertexArrays(1, &m_QuadVAO); m_QuadVAO = 0; }
    if (m_SSAOShader) { glDeleteProgram(m_SSAOShader); m_SSAOShader = 0; }
    if (m_BlurShader) { glDeleteProgram(m_BlurShader); m_BlurShader = 0; }
    if (m_NoiseTexture) { glDeleteTextures(1, &m_NoiseTexture); m_NoiseTexture = 0; }
    m_Initialized = false;
}

void SSAO::Resize(uint32_t width, uint32_t height) {
    if (width == m_Width && height == m_Height) return;
    if (width == 0 || height == 0) return;
    m_Width = width;
    m_Height = height;
    if (m_Initialized) {
        DestroyResources();
        CreateResources();
    }
}

void SSAO::CompileShaders() {
    m_SSAOShader = LinkProgram(s_QuadVertSrc, s_SSAOFragSrc);
    m_BlurShader = LinkProgram(s_QuadVertSrc, s_BlurFragSrc);
}

void SSAO::GenerateKernel() {
    std::default_random_engine rng(42);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    m_Kernel.resize(64);
    for (int i = 0; i < 64; i++) {
        glm::vec3 sample(
            dist(rng) * 2.0f - 1.0f,
            dist(rng) * 2.0f - 1.0f,
            dist(rng) // hemisphere: z always positive
        );
        sample = glm::normalize(sample) * dist(rng);

        // Accelerating interpolation: more samples near origin
        float scale = static_cast<float>(i) / 64.0f;
        scale = 0.1f + scale * scale * 0.9f; // lerp(0.1, 1.0, scale^2)
        sample *= scale;

        m_Kernel[i] = sample;
    }
}

void SSAO::GenerateNoiseTexture() {
    std::default_random_engine rng(123);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    // 4x4 noise texture (random rotation vectors on XY plane)
    std::vector<float> noise(4 * 4 * 3);
    for (int i = 0; i < 16; i++) {
        noise[i * 3 + 0] = dist(rng) * 2.0f - 1.0f;
        noise[i * 3 + 1] = dist(rng) * 2.0f - 1.0f;
        noise[i * 3 + 2] = 0.0f; // rotate around Z
    }

    glGenTextures(1, &m_NoiseTexture);
    glBindTexture(GL_TEXTURE_2D, m_NoiseTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void SSAO::CreateResources() {
    // SSAO output (single channel R8)
    auto createR8FBO = [&](uint32_t& fbo, uint32_t& tex) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_Width, m_Height, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };

    createR8FBO(m_SSAOFBO, m_SSAOTexture);
    createR8FBO(m_BlurFBO, m_BlurTexture);
}

void SSAO::DestroyResources() {
    auto del = [](uint32_t& fbo, uint32_t& tex) {
        if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
    };
    del(m_SSAOFBO, m_SSAOTexture);
    del(m_BlurFBO, m_BlurTexture);
}

void SSAO::RenderFullscreenQuad() {
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

uint32_t SSAO::Compute(uint32_t depthTexture, uint32_t width, uint32_t height,
                        const glm::mat4& projection, const glm::mat4& view,
                        const SSAOSettings& settings) {
    if (!settings.Enabled) return 0;

    if (!m_Initialized)
        Init(width, height);
    Resize(width, height);

    int kernelSize = std::clamp(settings.KernelSize, 4, 64);

    GLboolean depthTest;
    glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    glDisable(GL_DEPTH_TEST);

    // ── Pass 1: SSAO ─────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, m_SSAOFBO);
    glViewport(0, 0, m_Width, m_Height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_SSAOShader);

    // Depth texture
    glUniform1i(glGetUniformLocation(m_SSAOShader, "u_DepthMap"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthTexture);

    // Noise texture
    glUniform1i(glGetUniformLocation(m_SSAOShader, "u_NoiseTex"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_NoiseTexture);

    // Kernel samples
    for (int i = 0; i < kernelSize; i++) {
        std::string name = "u_Samples[" + std::to_string(i) + "]";
        glUniform3fv(glGetUniformLocation(m_SSAOShader, name.c_str()), 1, &m_Kernel[i].x);
    }

    glUniform1i(glGetUniformLocation(m_SSAOShader, "u_KernelSize"), kernelSize);
    glUniform1f(glGetUniformLocation(m_SSAOShader, "u_Radius"), settings.Radius);
    glUniform1f(glGetUniformLocation(m_SSAOShader, "u_Bias"), settings.Bias);
    glUniform1f(glGetUniformLocation(m_SSAOShader, "u_Intensity"), settings.Intensity);
    glUniformMatrix4fv(glGetUniformLocation(m_SSAOShader, "u_Projection"), 1, GL_FALSE, &projection[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_SSAOShader, "u_View"), 1, GL_FALSE, &view[0][0]);
    glUniform2f(glGetUniformLocation(m_SSAOShader, "u_ScreenSize"),
                static_cast<float>(m_Width), static_cast<float>(m_Height));

    RenderFullscreenQuad();

    // ── Pass 2: Blur ─────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, m_BlurFBO);
    glViewport(0, 0, m_Width, m_Height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_BlurShader);
    glUniform1i(glGetUniformLocation(m_BlurShader, "u_Input"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_SSAOTexture);

    RenderFullscreenQuad();

    // Restore
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    if (depthTest) glEnable(GL_DEPTH_TEST);

    return m_BlurTexture;
}

} // namespace VE
