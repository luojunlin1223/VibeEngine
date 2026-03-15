/*
 * SSR implementation — screen-space reflections via ray marching.
 *
 * Algorithm:
 *   1. Reconstruct view-space position and normal from the depth buffer
 *   2. Compute reflection direction in view space
 *   3. March a ray in screen space (UV + depth), checking depth intersections
 *   4. On hit, sample the scene color and write it as the reflection
 *   5. Apply screen-edge and facing-angle fade to hide artifacts
 */
#include "VibeEngine/Renderer/SSR.h"
#include "VibeEngine/Renderer/ShaderSources.h"
#include "VibeEngine/Core/Log.h"
#include <glad/gl.h>

namespace VE {

// ── Shader sources ───────────────────────────────────────────────────

// Fullscreen quad vertex shader shared via ShaderSources.h
static const char* s_QuadVertSrc = QuadVertexShaderSrc;

static const char* s_SSRFragSrc = R"(
#version 450 core
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_SceneColor;
uniform sampler2D u_DepthMap;
uniform mat4      u_Projection;
uniform mat4      u_InvProjection;
uniform mat4      u_View;
uniform vec2      u_ScreenSize;
uniform int       u_MaxSteps;
uniform float     u_StepSize;
uniform float     u_Thickness;
uniform float     u_MaxDistance;

// ── Helpers ──────────────────────────────────────────────────────────

// Reconstruct view-space position from depth at a given UV
vec3 ViewPosFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = u_InvProjection * ndc;
    return viewPos.xyz / viewPos.w;
}

// Project a view-space position to screen UV + depth
vec3 ProjectToScreen(vec3 viewPos) {
    vec4 clip = u_Projection * vec4(viewPos, 1.0);
    clip.xyz /= clip.w;
    return vec3(clip.xy * 0.5 + 0.5, clip.z * 0.5 + 0.5);
}

// Reconstruct view-space normal from depth buffer via cross product of
// neighboring view-space positions.
vec3 ReconstructNormal(vec2 uv, vec3 fragPos) {
    vec2 texelSize = 1.0 / u_ScreenSize;
    float dR = texture(u_DepthMap, uv + vec2(texelSize.x, 0.0)).r;
    float dL = texture(u_DepthMap, uv - vec2(texelSize.x, 0.0)).r;
    float dU = texture(u_DepthMap, uv + vec2(0.0, texelSize.y)).r;
    float dD = texture(u_DepthMap, uv - vec2(0.0, texelSize.y)).r;

    vec3 posR = ViewPosFromDepth(uv + vec2(texelSize.x, 0.0), dR);
    vec3 posL = ViewPosFromDepth(uv - vec2(texelSize.x, 0.0), dL);
    vec3 posU = ViewPosFromDepth(uv + vec2(0.0, texelSize.y), dU);
    vec3 posD = ViewPosFromDepth(uv - vec2(0.0, texelSize.y), dD);

    return normalize(cross(posR - posL, posU - posD));
}

// Screen-edge vignette: fade to zero near edges of the screen
float ScreenEdgeFade(vec2 uv) {
    vec2 fade = smoothstep(0.0, 0.1, uv) * (1.0 - smoothstep(0.9, 1.0, uv));
    return fade.x * fade.y;
}

// ── Main ─────────────────────────────────────────────────────────────

void main() {
    float depth = texture(u_DepthMap, v_UV).r;

    // Skip sky pixels
    if (depth >= 1.0) {
        FragColor = vec4(0.0);
        return;
    }

    vec3 fragPos = ViewPosFromDepth(v_UV, depth);
    vec3 normal  = ReconstructNormal(v_UV, fragPos);

    // View direction (in view space, camera is at origin)
    vec3 viewDir = normalize(fragPos);
    vec3 reflectDir = reflect(viewDir, normal);

    // Skip reflections pointing toward camera (backfacing)
    // Also fade based on angle — reflections at grazing angles are stronger
    float facingFade = max(reflectDir.z, 0.0);
    // reflectDir.z > 0 means ray points away from camera (into the scene)
    // We want some tolerance for near-perpendicular reflections
    if (facingFade < 0.001) {
        FragColor = vec4(0.0);
        return;
    }
    // Attenuate reflections that point mostly toward the camera
    float dirFade = 1.0 - pow(1.0 - facingFade, 3.0);

    // ── Ray march in screen space ────────────────────────────────────

    // Start position and direction in screen space
    vec3 startScreen = ProjectToScreen(fragPos);
    vec3 endViewPos  = fragPos + reflectDir * u_MaxDistance;
    vec3 endScreen   = ProjectToScreen(endViewPos);

    vec3 rayDir = endScreen - startScreen;
    // Normalize so the largest component steps at most u_StepSize per step
    float rayLen = length(rayDir.xy);
    if (rayLen < 0.0001) {
        FragColor = vec4(0.0);
        return;
    }

    // Scale step to be in UV-space increments
    vec3 rayStep = rayDir / rayLen * u_StepSize;

    vec3 currentPos = startScreen + rayStep; // skip start pixel
    bool hit = false;
    vec3 hitColor = vec3(0.0);
    vec2 hitUV = vec2(0.0);

    for (int i = 0; i < u_MaxSteps; i++) {
        // Bounds check
        if (currentPos.x < 0.0 || currentPos.x > 1.0 ||
            currentPos.y < 0.0 || currentPos.y > 1.0 ||
            currentPos.z < 0.0 || currentPos.z > 1.0) {
            break;
        }

        float sampledDepth = texture(u_DepthMap, currentPos.xy).r;

        // Convert sampled depth and ray depth to view-space Z for thickness check
        vec3 sampledViewPos = ViewPosFromDepth(currentPos.xy, sampledDepth);
        vec3 rayViewPos     = ViewPosFromDepth(currentPos.xy, currentPos.z);

        float depthDiff = rayViewPos.z - sampledViewPos.z;

        // Check if ray is behind the surface but within thickness
        if (depthDiff > 0.0 && depthDiff < u_Thickness) {
            // Binary search refinement (5 iterations)
            vec3 refinedPos = currentPos;
            vec3 refinedStep = rayStep * 0.5;
            for (int j = 0; j < 5; j++) {
                refinedPos -= refinedStep;
                refinedStep *= 0.5;

                float refDepth = texture(u_DepthMap, refinedPos.xy).r;
                vec3 refSampledView = ViewPosFromDepth(refinedPos.xy, refDepth);
                vec3 refRayView     = ViewPosFromDepth(refinedPos.xy, refinedPos.z);
                float refDiff = refRayView.z - refSampledView.z;

                if (refDiff > 0.0) {
                    // Still behind surface, step back
                    // (refinedPos was already moved back, just continue)
                } else {
                    // In front of surface, step forward
                    refinedPos += refinedStep * 2.0;
                }
            }

            // Verify final hit is still valid
            float finalDepth = texture(u_DepthMap, refinedPos.xy).r;
            if (finalDepth >= 1.0) {
                // Hit sky — skip
                currentPos += rayStep;
                continue;
            }
            vec3 finalSampledView = ViewPosFromDepth(refinedPos.xy, finalDepth);
            vec3 finalRayView     = ViewPosFromDepth(refinedPos.xy, refinedPos.z);
            float finalDiff = finalRayView.z - finalSampledView.z;

            if (abs(finalDiff) < u_Thickness) {
                hitUV = refinedPos.xy;
                hitColor = texture(u_SceneColor, hitUV).rgb;
                hit = true;
            }
            break;
        }

        currentPos += rayStep;
    }

    if (!hit) {
        FragColor = vec4(0.0);
        return;
    }

    // Fade based on screen edge proximity
    float edgeFade = ScreenEdgeFade(hitUV);

    // Distance fade — fade out as ray travels farther
    float travelDist = length(hitUV - v_UV) / max(length(rayDir.xy), 0.001) * rayLen;
    float distFade = 1.0 - smoothstep(0.5, 1.0, travelDist);

    float alpha = edgeFade * dirFade * distFade;
    FragColor = vec4(hitColor, alpha);
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
        VE_ENGINE_ERROR("SSR shader compile error: {}", log);
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
        VE_ENGINE_ERROR("SSR program link error: {}", log);
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

// ── SSR Implementation ──────────────────────────────────────────────

SSR::~SSR() {
    Shutdown();
}

void SSR::Init(uint32_t width, uint32_t height) {
    if (m_Initialized) return;
    m_Width = width;
    m_Height = height;

    CompileShaders();
    glGenVertexArrays(1, &m_QuadVAO);
    CreateResources();

    m_Initialized = true;
    VE_ENGINE_INFO("SSR initialized ({}x{})", width, height);
}

void SSR::Shutdown() {
    if (!m_Initialized) return;
    DestroyResources();
    if (m_QuadVAO) { glDeleteVertexArrays(1, &m_QuadVAO); m_QuadVAO = 0; }
    if (m_SSRShader) { glDeleteProgram(m_SSRShader); m_SSRShader = 0; }
    m_Initialized = false;
}

void SSR::Resize(uint32_t width, uint32_t height) {
    if (width == m_Width && height == m_Height) return;
    if (width == 0 || height == 0) return;
    m_Width = width;
    m_Height = height;
    if (m_Initialized) {
        DestroyResources();
        CreateResources();
    }
}

void SSR::CompileShaders() {
    m_SSRShader = LinkProgram(s_QuadVertSrc, s_SSRFragSrc);
    CacheUniformLocations();
}

void SSR::CacheUniformLocations() {
    m_LocSceneColor    = glGetUniformLocation(m_SSRShader, "u_SceneColor");
    m_LocDepthMap      = glGetUniformLocation(m_SSRShader, "u_DepthMap");
    m_LocProjection    = glGetUniformLocation(m_SSRShader, "u_Projection");
    m_LocInvProjection = glGetUniformLocation(m_SSRShader, "u_InvProjection");
    m_LocView          = glGetUniformLocation(m_SSRShader, "u_View");
    m_LocScreenSize    = glGetUniformLocation(m_SSRShader, "u_ScreenSize");
    m_LocMaxSteps      = glGetUniformLocation(m_SSRShader, "u_MaxSteps");
    m_LocStepSize      = glGetUniformLocation(m_SSRShader, "u_StepSize");
    m_LocThickness     = glGetUniformLocation(m_SSRShader, "u_Thickness");
    m_LocMaxDistance    = glGetUniformLocation(m_SSRShader, "u_MaxDistance");
}

void SSR::CreateResources() {
    // SSR output (RGBA16F — RGB = reflected color, A = confidence)
    glGenFramebuffers(1, &m_SSRFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_SSRFBO);
    glGenTextures(1, &m_ReflectionTexture);
    glBindTexture(GL_TEXTURE_2D, m_ReflectionTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_Width, m_Height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ReflectionTexture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSR::DestroyResources() {
    if (m_SSRFBO) { glDeleteFramebuffers(1, &m_SSRFBO); m_SSRFBO = 0; }
    if (m_ReflectionTexture) { glDeleteTextures(1, &m_ReflectionTexture); m_ReflectionTexture = 0; }
}

void SSR::RenderFullscreenQuad() {
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

uint32_t SSR::Compute(uint32_t colorTexture, uint32_t depthTexture,
                       uint32_t width, uint32_t height,
                       const glm::mat4& projection, const glm::mat4& view,
                       const SSRSettings& settings) {
    if (!settings.Enabled) return 0;

    if (!m_Initialized)
        Init(width, height);
    Resize(width, height);

    GLboolean depthTest;
    glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    glDisable(GL_DEPTH_TEST);

    // ── SSR pass ─────────────────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, m_SSRFBO);
    glViewport(0, 0, m_Width, m_Height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_SSRShader);

    // Scene color texture
    glUniform1i(m_LocSceneColor, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, colorTexture);

    // Depth texture
    glUniform1i(m_LocDepthMap, 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthTexture);

    // Matrices
    glm::mat4 invProjection = glm::inverse(projection);
    glUniformMatrix4fv(m_LocProjection, 1, GL_FALSE, &projection[0][0]);
    glUniformMatrix4fv(m_LocInvProjection, 1, GL_FALSE, &invProjection[0][0]);
    glUniformMatrix4fv(m_LocView, 1, GL_FALSE, &view[0][0]);

    // Settings
    glUniform2f(m_LocScreenSize,
                static_cast<float>(m_Width), static_cast<float>(m_Height));
    glUniform1i(m_LocMaxSteps, settings.MaxSteps);
    glUniform1f(m_LocStepSize, settings.StepSize);
    glUniform1f(m_LocThickness, settings.Thickness);
    glUniform1f(m_LocMaxDistance, settings.MaxDistance);

    RenderFullscreenQuad();

    // Restore
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    if (depthTest) glEnable(GL_DEPTH_TEST);

    return m_ReflectionTexture;
}

} // namespace VE
