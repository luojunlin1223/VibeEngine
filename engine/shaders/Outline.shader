// VibeEngine ShaderLab — Outline Shader
// Renders only the silhouette by extruding back-faces along normals.
// Cull Front means only back-facing triangles are drawn; after normal
// extrusion they form a visible shell only at the silhouette edge.

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
uniform float u_OutlineWidth;

void main() {
    // Extrude along object-space normal
    vec3 pos = a_Position + normalize(a_Normal) * u_OutlineWidth;
    gl_Position = u_MVP * vec4(pos, 1.0);
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
