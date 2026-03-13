// VibeEngine ShaderLab — Sprite Batch Shader
// Used by SpriteBatchRenderer for efficient 2D sprite rendering.
// Vertex format: Position(3) + Color(4) + TexCoord(2) + TexIndex(1) = 10 floats

Shader "VibeEngine/Sprite" {
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

    // Use switch for dynamically uniform expression requirement
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
    if (finalColor.a < 0.01)
        discard;
    FragColor = finalColor;
}
#endif

            ENDGLSL
        }
    }

    FallBack Off
}
