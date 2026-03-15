#pragma once

/*
 * ShaderSources.h — Shared inline shader source strings.
 *
 * Centralizes shader source code that is used by multiple subsystems
 * (post-processing, SSAO, SSR, error fallback) to eliminate duplication.
 */

namespace VE {

// ── Fullscreen quad vertex shader ───────────────────────────────────
// Generates a fullscreen triangle from gl_VertexID (no VBO needed).
// Used by post-processing, SSAO, SSR, and any fullscreen pass.
inline const char* QuadVertexShaderSrc = R"(
#version 450 core
layout(location = 0) out vec2 v_UV;
void main() {
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

// ── Error / fallback shader (bright magenta) ────────────────────────
// Standard game-engine practice: when a shader fails to compile or load,
// render objects in bright magenta so the problem is immediately visible.
inline const char* ErrorVertexSrc = R"(
#version 450 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;
uniform mat4 u_MVP;
void main() {
    gl_Position = u_MVP * vec4(a_Position, 1.0);
}
)";

inline const char* ErrorFragmentSrc = R"(
#version 450 core
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = vec4(1.0, 0.0, 1.0, 1.0); // Bright magenta — signals a broken shader
}
)";

} // namespace VE
