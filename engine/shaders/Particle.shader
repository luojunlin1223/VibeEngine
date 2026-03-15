// VibeEngine ShaderLab — Particle Shader
// Billboard quads with alpha blending, soft fade near edges.
// Vertex format matches SpriteBatchRenderer: Position(3) + Color(4) + TexCoord(2) + TexIndex(1)

Shader "VibeEngine/Particle" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }

        Pass {
            Name "Default"
            Tags { "LightMode"="Always" }

            Cull Off
            ZWrite Off
            ZTest LEqual
            Blend SrcAlpha OneMinusSrcAlpha

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in float a_TexIndex;

uniform mat4 u_ViewProjection;

out vec4 v_Color;
out vec2 v_TexCoord;
out float v_TexIndex;

void main() {
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;
    v_TexIndex = a_TexIndex;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}
#endif

#ifdef FRAGMENT
in vec4 v_Color;
in vec2 v_TexCoord;
in float v_TexIndex;

uniform sampler2D u_Textures[16];

out vec4 FragColor;

void main() {
    int idx = int(v_TexIndex);
    vec4 texColor = vec4(1.0);

    switch (idx) {
        case  0: texColor = texture(u_Textures[ 0], v_TexCoord); break;
        case  1: texColor = texture(u_Textures[ 1], v_TexCoord); break;
        case  2: texColor = texture(u_Textures[ 2], v_TexCoord); break;
        case  3: texColor = texture(u_Textures[ 3], v_TexCoord); break;
        case  4: texColor = texture(u_Textures[ 4], v_TexCoord); break;
        case  5: texColor = texture(u_Textures[ 5], v_TexCoord); break;
        case  6: texColor = texture(u_Textures[ 6], v_TexCoord); break;
        case  7: texColor = texture(u_Textures[ 7], v_TexCoord); break;
        case  8: texColor = texture(u_Textures[ 8], v_TexCoord); break;
        case  9: texColor = texture(u_Textures[ 9], v_TexCoord); break;
        case 10: texColor = texture(u_Textures[10], v_TexCoord); break;
        case 11: texColor = texture(u_Textures[11], v_TexCoord); break;
        case 12: texColor = texture(u_Textures[12], v_TexCoord); break;
        case 13: texColor = texture(u_Textures[13], v_TexCoord); break;
        case 14: texColor = texture(u_Textures[14], v_TexCoord); break;
        case 15: texColor = texture(u_Textures[15], v_TexCoord); break;
    }

    vec4 finalColor = texColor * v_Color;

    // Soft circular falloff for untextured particles (when using default white texture)
    // Creates a smooth circle instead of a hard-edged quad
    if (idx == 0) {
        vec2 center = v_TexCoord - vec2(0.5);
        float dist = length(center) * 2.0; // 0 at center, 1 at edge
        float softEdge = 1.0 - smoothstep(0.8, 1.0, dist);
        finalColor.a *= softEdge;
    }

    if (finalColor.a < 0.005)
        discard;
    FragColor = finalColor;
}
#endif

            ENDGLSL
        }
    }

    FallBack Off
}
