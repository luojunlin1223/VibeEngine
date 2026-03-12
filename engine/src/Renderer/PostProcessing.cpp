/*
 * PostProcessing — OpenGL implementation of fullscreen post-processing.
 *
 * Composite pass order:
 *   1. Bloom additive blend
 *   2. Exposure
 *   3. Color Curves (1D LUT sampling)
 *   4. Shadows / Midtones / Highlights
 *   5. Color filter, Contrast, Saturation
 *   6. Tonemapping (HDR → LDR)
 *   7. Gamma correction
 *   8. Vignette
 */
#include "VibeEngine/Renderer/PostProcessing.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>
#include <cmath>
#include <algorithm>

namespace VE {

// ── Shader sources ───────────────────────────────────────────────────

static const char* s_QuadVertexSrc = R"(
#version 450 core
layout(location = 0) out vec2 v_UV;
void main() {
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* s_BrightExtractFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;
uniform sampler2D u_Scene;
uniform float u_Threshold;
void main() {
    vec3 color = texture(u_Scene, v_UV).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    FragColor = (brightness > u_Threshold) ? vec4(color, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
}
)";

static const char* s_BlurFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;
uniform sampler2D u_Image;
uniform bool u_Horizontal;
const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(u_Image, 0));
    vec3 result = texture(u_Image, v_UV).rgb * weights[0];
    if (u_Horizontal) {
        for (int i = 1; i < 5; ++i) {
            result += texture(u_Image, v_UV + vec2(texelSize.x * float(i), 0.0)).rgb * weights[i];
            result += texture(u_Image, v_UV - vec2(texelSize.x * float(i), 0.0)).rgb * weights[i];
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            result += texture(u_Image, v_UV + vec2(0.0, texelSize.y * float(i))).rgb * weights[i];
            result += texture(u_Image, v_UV - vec2(0.0, texelSize.y * float(i))).rgb * weights[i];
        }
    }
    FragColor = vec4(result, 1.0);
}
)";

static const char* s_CompositeFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_Scene;
uniform sampler2D u_Bloom;
uniform sampler2D u_CurvesLUT;  // 256x1 RGBA

// Bloom
uniform bool  u_BloomEnabled;
uniform float u_BloomIntensity;

// Color Curves
uniform bool u_CurvesEnabled;

// Shadows / Midtones / Highlights
uniform bool u_SMHEnabled;
uniform vec3 u_SMH_Shadows;
uniform vec3 u_SMH_Midtones;
uniform vec3 u_SMH_Highlights;
uniform float u_SMH_ShadowStart;
uniform float u_SMH_ShadowEnd;
uniform float u_SMH_HighlightStart;
uniform float u_SMH_HighlightEnd;

// Color Adjustments
uniform bool  u_ColorEnabled;
uniform float u_Exposure;
uniform float u_Contrast;
uniform float u_Saturation;
uniform vec3  u_ColorFilter;
uniform float u_Gamma;

// Tonemapping
uniform bool u_TonemapEnabled;
uniform int  u_TonemapMode; // 0=None, 1=Reinhard, 2=ACES, 3=Uncharted2

// Vignette
uniform bool  u_VignetteEnabled;
uniform float u_VignetteIntensity;
uniform float u_VignetteSmoothness;

// ── Tonemapping operators ────────────────────────────────────────────

vec3 TonemapReinhard(vec3 c) {
    return c / (c + vec3(1.0));
}

vec3 TonemapACES(vec3 c) {
    // Narkowicz ACES fit
    const float a = 2.51;
    const float b = 0.03;
    const float cc = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0.0, 1.0);
}

vec3 Uncharted2Helper(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 TonemapUncharted2(vec3 c) {
    const float W = 11.2;
    vec3 curr = Uncharted2Helper(c * 2.0);
    vec3 whiteScale = vec3(1.0) / Uncharted2Helper(vec3(W));
    return curr * whiteScale;
}

void main() {
    vec3 color = texture(u_Scene, v_UV).rgb;

    // 1. Bloom
    if (u_BloomEnabled) {
        vec3 bloom = texture(u_Bloom, v_UV).rgb;
        color += bloom * u_BloomIntensity;
    }

    // 2. Exposure (before color grading, in linear HDR)
    if (u_ColorEnabled) {
        color *= pow(2.0, u_Exposure);
    }

    // 3. Color Curves (1D LUT lookup)
    if (u_CurvesEnabled) {
        // Clamp to [0,1] for LUT lookup; HDR values above 1 are clamped
        float r = clamp(color.r, 0.0, 1.0);
        float g = clamp(color.g, 0.0, 1.0);
        float b = clamp(color.b, 0.0, 1.0);
        // LUT stores: R=master curve, G=red curve, B=green curve, A=blue curve
        // Each channel maps input→output for that curve
        float masterR = texture(u_CurvesLUT, vec2(r, 0.5)).r;
        float masterG = texture(u_CurvesLUT, vec2(g, 0.5)).r;
        float masterB = texture(u_CurvesLUT, vec2(b, 0.5)).r;
        color.r = texture(u_CurvesLUT, vec2(masterR, 0.5)).g;
        color.g = texture(u_CurvesLUT, vec2(masterG, 0.5)).b;
        color.b = texture(u_CurvesLUT, vec2(masterB, 0.5)).a;
    }

    // 4. Shadows / Midtones / Highlights
    if (u_SMHEnabled) {
        float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float shadowWeight    = 1.0 - smoothstep(u_SMH_ShadowStart, u_SMH_ShadowEnd, luma);
        float highlightWeight = smoothstep(u_SMH_HighlightStart, u_SMH_HighlightEnd, luma);
        float midtoneWeight   = 1.0 - shadowWeight - highlightWeight;
        midtoneWeight = max(midtoneWeight, 0.0);

        vec3 grade = u_SMH_Shadows * shadowWeight
                   + u_SMH_Midtones * midtoneWeight
                   + u_SMH_Highlights * highlightWeight;
        color *= grade;
    }

    // 5. Color filter, Contrast, Saturation
    if (u_ColorEnabled) {
        color *= u_ColorFilter;
        color = clamp((color - 0.5) * (1.0 + u_Contrast) + 0.5, 0.0, 100.0);
        float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = max(mix(vec3(luma), color, 1.0 + u_Saturation), 0.0);
    }

    // 6. Tonemapping
    if (u_TonemapEnabled) {
        if (u_TonemapMode == 1)      color = TonemapReinhard(color);
        else if (u_TonemapMode == 2) color = TonemapACES(color);
        else if (u_TonemapMode == 3) color = TonemapUncharted2(color);
    }

    // 7. Gamma correction
    if (u_ColorEnabled && u_Gamma != 1.0) {
        color = pow(max(color, 0.0), vec3(1.0 / u_Gamma));
    }

    // 8. Vignette
    if (u_VignetteEnabled) {
        vec2 center = v_UV - 0.5;
        float dist = length(center) * 1.414;
        float vignette = 1.0 - smoothstep(1.0 - u_VignetteSmoothness, 1.0, dist * u_VignetteIntensity);
        color *= vignette;
    }

    FragColor = vec4(color, 1.0);
}
)";

// ── Shader compilation helpers ───────────────────────────────────────

static uint32_t CompileShader(GLenum type, const char* src) {
    uint32_t shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        VE_ENGINE_ERROR("PostProcessing shader compile error: {}", log);
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
        VE_ENGINE_ERROR("PostProcessing program link error: {}", log);
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

// ── Curve evaluation (monotone cubic Hermite interpolation) ──────────

static float EvalCurve(const CurveChannel& ch, float x) {
    const auto& pts = ch.Points;
    if (pts.empty()) return x;
    if (pts.size() == 1) return pts[0].second;

    // Clamp to curve bounds
    if (x <= pts.front().first) return pts.front().second;
    if (x >= pts.back().first)  return pts.back().second;

    // Find segment
    size_t seg = 0;
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        if (x < pts[i + 1].first) { seg = i; break; }
    }

    float x0 = pts[seg].first,     y0 = pts[seg].second;
    float x1 = pts[seg + 1].first, y1 = pts[seg + 1].second;
    float dx = x1 - x0;
    if (dx < 1e-6f) return y0;
    float t = (x - x0) / dx;

    // Compute tangents (Catmull-Rom style, clamped for monotonicity)
    auto tangent = [&](size_t i) -> float {
        if (pts.size() < 2) return 0.0f;
        if (i == 0) return (pts[1].second - pts[0].second) / (pts[1].first - pts[0].first + 1e-6f);
        if (i == pts.size() - 1) {
            size_t n = pts.size() - 1;
            return (pts[n].second - pts[n - 1].second) / (pts[n].first - pts[n - 1].first + 1e-6f);
        }
        return 0.5f * ((pts[i + 1].second - pts[i - 1].second) /
                        (pts[i + 1].first  - pts[i - 1].first + 1e-6f));
    };

    float m0 = tangent(seg) * dx;
    float m1 = tangent(seg + 1) * dx;

    // Hermite basis
    float t2 = t * t, t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1;
    float h10 = t3 - 2 * t2 + t;
    float h01 = -2 * t3 + 3 * t2;
    float h11 = t3 - t2;

    float val = h00 * y0 + h10 * m0 + h01 * y1 + h11 * m1;
    return std::clamp(val, 0.0f, 1.0f);
}

// ── PostProcessing implementation ────────────────────────────────────

PostProcessing::~PostProcessing() {
    Shutdown();
}

void PostProcessing::Init(uint32_t width, uint32_t height) {
    if (m_Initialized) return;
    m_Width = width;
    m_Height = height;

    CompileShaders();
    glGenVertexArrays(1, &m_QuadVAO);

    // Create curves LUT texture
    glGenTextures(1, &m_CurvesLUT);
    glBindTexture(GL_TEXTURE_2D, m_CurvesLUT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    CreateResources();
    m_Initialized = true;
    VE_ENGINE_INFO("PostProcessing initialized ({}x{})", width, height);
}

void PostProcessing::Shutdown() {
    if (!m_Initialized) return;
    DestroyResources();

    if (m_QuadVAO) { glDeleteVertexArrays(1, &m_QuadVAO); m_QuadVAO = 0; }
    if (m_CurvesLUT) { glDeleteTextures(1, &m_CurvesLUT); m_CurvesLUT = 0; }
    if (m_BrightExtractShader) { glDeleteProgram(m_BrightExtractShader); m_BrightExtractShader = 0; }
    if (m_BlurShader) { glDeleteProgram(m_BlurShader); m_BlurShader = 0; }
    if (m_CompositeShader) { glDeleteProgram(m_CompositeShader); m_CompositeShader = 0; }

    m_Initialized = false;
}

void PostProcessing::Resize(uint32_t width, uint32_t height) {
    if (width == m_Width && height == m_Height) return;
    if (width == 0 || height == 0) return;
    m_Width = width;
    m_Height = height;
    if (m_Initialized) {
        DestroyResources();
        CreateResources();
    }
}

void PostProcessing::CompileShaders() {
    m_BrightExtractShader = LinkProgram(s_QuadVertexSrc, s_BrightExtractFragSrc);
    m_BlurShader = LinkProgram(s_QuadVertexSrc, s_BlurFragSrc);
    m_CompositeShader = LinkProgram(s_QuadVertexSrc, s_CompositeFragSrc);
}

static uint32_t CreateColorFBO(uint32_t& texture, uint32_t w, uint32_t h) {
    uint32_t fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        VE_ENGINE_ERROR("PostProcessing FBO incomplete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbo;
}

void PostProcessing::CreateResources() {
    uint32_t halfW = (m_Width > 1) ? m_Width / 2 : 1;
    uint32_t halfH = (m_Height > 1) ? m_Height / 2 : 1;

    m_BrightFBO = CreateColorFBO(m_BrightTexture, m_Width, m_Height);
    for (int i = 0; i < 2; i++)
        m_BlurFBO[i] = CreateColorFBO(m_BlurTexture[i], halfW, halfH);
    m_CompositeFBO = CreateColorFBO(m_CompositeTexture, m_Width, m_Height);
}

void PostProcessing::DestroyResources() {
    auto deleteFBO = [](uint32_t& fbo, uint32_t& tex) {
        if (fbo) { glDeleteFramebuffers(1, &fbo); fbo = 0; }
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
    };
    deleteFBO(m_BrightFBO, m_BrightTexture);
    deleteFBO(m_BlurFBO[0], m_BlurTexture[0]);
    deleteFBO(m_BlurFBO[1], m_BlurTexture[1]);
    deleteFBO(m_CompositeFBO, m_CompositeTexture);
}

void PostProcessing::RenderFullscreenQuad() {
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void PostProcessing::BakeCurvesLUT(const ColorCurvesSettings& curves) {
    // 256x1 RGBA texture:
    //   R = Master curve output
    //   G = Red curve output
    //   B = Green curve output
    //   A = Blue curve output
    uint8_t data[256 * 4];
    for (int i = 0; i < 256; i++) {
        float t = static_cast<float>(i) / 255.0f;
        data[i * 4 + 0] = static_cast<uint8_t>(std::clamp(EvalCurve(curves.Master, t) * 255.0f + 0.5f, 0.0f, 255.0f));
        data[i * 4 + 1] = static_cast<uint8_t>(std::clamp(EvalCurve(curves.Red,    t) * 255.0f + 0.5f, 0.0f, 255.0f));
        data[i * 4 + 2] = static_cast<uint8_t>(std::clamp(EvalCurve(curves.Green,  t) * 255.0f + 0.5f, 0.0f, 255.0f));
        data[i * 4 + 3] = static_cast<uint8_t>(std::clamp(EvalCurve(curves.Blue,   t) * 255.0f + 0.5f, 0.0f, 255.0f));
    }
    glBindTexture(GL_TEXTURE_2D, m_CurvesLUT);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

uint32_t PostProcessing::Apply(uint32_t sceneColorTexture, uint32_t width, uint32_t height,
                                const PostProcessSettings& settings) {
    if (!m_Initialized)
        Init(width, height);
    Resize(width, height);

    bool anyEffect = settings.Bloom.Enabled || settings.Vignette.Enabled
                  || settings.Color.Enabled || settings.SMH.Enabled
                  || settings.Curves.Enabled || settings.Tonemap.Enabled;
    if (!anyEffect)
        return sceneColorTexture;

    // Save GL state
    GLboolean depthTest;
    glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    glDisable(GL_DEPTH_TEST);

    // ── Bloom passes ─────────────────────────────────────────────────
    uint32_t bloomTex = 0;
    if (settings.Bloom.Enabled) {
        uint32_t halfW = (m_Width > 1) ? m_Width / 2 : 1;
        uint32_t halfH = (m_Height > 1) ? m_Height / 2 : 1;

        glBindFramebuffer(GL_FRAMEBUFFER, m_BrightFBO);
        glViewport(0, 0, m_Width, m_Height);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(m_BrightExtractShader);
        glUniform1i(glGetUniformLocation(m_BrightExtractShader, "u_Scene"), 0);
        glUniform1f(glGetUniformLocation(m_BrightExtractShader, "u_Threshold"), settings.Bloom.Threshold);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColorTexture);
        RenderFullscreenQuad();

        glUseProgram(m_BlurShader);
        glUniform1i(glGetUniformLocation(m_BlurShader, "u_Image"), 0);
        glViewport(0, 0, halfW, halfH);

        bool horizontal = true;
        uint32_t inputTex = m_BrightTexture;
        int passes = settings.Bloom.Iterations * 2;
        for (int i = 0; i < passes; i++) {
            int target = horizontal ? 0 : 1;
            glBindFramebuffer(GL_FRAMEBUFFER, m_BlurFBO[target]);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform1i(glGetUniformLocation(m_BlurShader, "u_Horizontal"), horizontal ? 1 : 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, inputTex);
            RenderFullscreenQuad();
            inputTex = m_BlurTexture[target];
            horizontal = !horizontal;
        }
        bloomTex = inputTex;
    }

    // ── Bake curves LUT ──────────────────────────────────────────────
    if (settings.Curves.Enabled) {
        BakeCurvesLUT(settings.Curves);
    }

    // ── Composite pass ───────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, m_CompositeFBO);
    glViewport(0, 0, m_Width, m_Height);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(m_CompositeShader);

    // Texture unit 0: scene
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_Scene"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture);

    // Texture unit 1: bloom
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_BloomEnabled"), settings.Bloom.Enabled ? 1 : 0);
    if (settings.Bloom.Enabled) {
        glUniform1i(glGetUniformLocation(m_CompositeShader, "u_Bloom"), 1);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_BloomIntensity"), settings.Bloom.Intensity);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomTex);
    }

    // Texture unit 2: curves LUT
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_CurvesEnabled"), settings.Curves.Enabled ? 1 : 0);
    if (settings.Curves.Enabled) {
        glUniform1i(glGetUniformLocation(m_CompositeShader, "u_CurvesLUT"), 2);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_CurvesLUT);
    }

    // SMH
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_SMHEnabled"), settings.SMH.Enabled ? 1 : 0);
    if (settings.SMH.Enabled) {
        glUniform3f(glGetUniformLocation(m_CompositeShader, "u_SMH_Shadows"),
                    settings.SMH.Shadows[0], settings.SMH.Shadows[1], settings.SMH.Shadows[2]);
        glUniform3f(glGetUniformLocation(m_CompositeShader, "u_SMH_Midtones"),
                    settings.SMH.Midtones[0], settings.SMH.Midtones[1], settings.SMH.Midtones[2]);
        glUniform3f(glGetUniformLocation(m_CompositeShader, "u_SMH_Highlights"),
                    settings.SMH.Highlights[0], settings.SMH.Highlights[1], settings.SMH.Highlights[2]);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_SMH_ShadowStart"), settings.SMH.ShadowStart);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_SMH_ShadowEnd"), settings.SMH.ShadowEnd);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_SMH_HighlightStart"), settings.SMH.HighlightStart);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_SMH_HighlightEnd"), settings.SMH.HighlightEnd);
    }

    // Color Adjustments
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_ColorEnabled"), settings.Color.Enabled ? 1 : 0);
    if (settings.Color.Enabled) {
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_Exposure"), settings.Color.Exposure);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_Contrast"), settings.Color.Contrast);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_Saturation"), settings.Color.Saturation);
        glUniform3f(glGetUniformLocation(m_CompositeShader, "u_ColorFilter"),
                    settings.Color.ColorFilter[0], settings.Color.ColorFilter[1], settings.Color.ColorFilter[2]);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_Gamma"), settings.Color.Gamma);
    }

    // Tonemapping
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_TonemapEnabled"), settings.Tonemap.Enabled ? 1 : 0);
    if (settings.Tonemap.Enabled) {
        glUniform1i(glGetUniformLocation(m_CompositeShader, "u_TonemapMode"), static_cast<int>(settings.Tonemap.Mode));
    }

    // Vignette
    glUniform1i(glGetUniformLocation(m_CompositeShader, "u_VignetteEnabled"), settings.Vignette.Enabled ? 1 : 0);
    if (settings.Vignette.Enabled) {
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_VignetteIntensity"), settings.Vignette.Intensity);
        glUniform1f(glGetUniformLocation(m_CompositeShader, "u_VignetteSmoothness"), settings.Vignette.Smoothness);
    }

    RenderFullscreenQuad();

    // Restore GL state
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    if (depthTest) glEnable(GL_DEPTH_TEST);

    return m_CompositeTexture;
}

} // namespace VE
