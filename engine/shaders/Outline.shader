// VibeEngine ShaderLab — Outline Shader
// Renders silhouette outline by extruding back-face vertices along
// clip-space normals for constant screen-pixel width.

Shader "VibeEngine/Outline"
{
    Properties
    {
        _OutlineColor ("Outline Color", Color) = (1.0, 0.5, 0.0, 1.0)
    }

    SubShader
    {
        Tags { "RenderType"="Overlay" "Queue"="Overlay" }

        Pass
        {
            Cull Front
            ZWrite Off
            ZTest LEqual

            GLSLPROGRAM

#ifdef VERTEX
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

uniform mat4 u_MVP;
uniform mat4 u_Model;
uniform vec4 u_ViewportSize; // xy = width, height
uniform float u_OutlinePixels; // outline width in pixels

void main() {
    // Transform position to clip space
    gl_Position = u_MVP * vec4(a_Position, 1.0);

    // Transform a nearby point along the normal to clip space
    vec3 worldNormal = normalize(mat3(transpose(inverse(u_Model))) * a_Normal);
    vec4 clipNormal = u_MVP * vec4(a_Position + worldNormal * 0.001, 1.0);

    // Compute screen-space normal direction
    vec2 screenPos = gl_Position.xy / gl_Position.w;
    vec2 screenNorm = clipNormal.xy / clipNormal.w;
    vec2 diff = screenNorm - screenPos;
    float len = length(diff);
    vec2 dir = (len > 0.00001) ? diff / len : vec2(0.0, 1.0);

    // Offset in NDC by fixed pixel amount
    vec2 pixelOffset = dir * (u_OutlinePixels / u_ViewportSize.xy) * 2.0;
    gl_Position.xy += pixelOffset * gl_Position.w;
}
#endif

#ifdef FRAGMENT
out vec4 FragColor;
uniform vec4 u_OutlineColor;

void main() {
    FragColor = u_OutlineColor;
}
#endif

            ENDGLSL
        }
    }
}
