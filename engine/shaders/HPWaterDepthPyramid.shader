// VibeEngine ShaderLab - HPWater opaque scene-depth pyramid
// Copies the opaque G-buffer depth into an R32F mip chain and downsamples with
// min depth so refraction can conservatively march against nearest opaque hits.

Shader "VibeEngine/HPWaterDepthPyramid" {
    Properties {
    }

    SubShader {
        Tags { "RenderType"="Opaque" "Queue"="Overlay" "LightMode"="HPWaterDepthPyramid" }

        Pass {
            Name "HPWaterDepthPyramidPass"

            Cull Off
            ZWrite Off
            ZTest Always

            GLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag

#version 460 core

#ifdef VERTEX
layout(location = 0) out vec2 v_UV;
void main() {
    vec2 pos = vec2((gl_VertexID & 1) * 2.0, (gl_VertexID & 2) * 1.0);
    v_UV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_SourceDepth;
uniform int u_FirstMip;
uniform int u_SourceMip;

void main() {
    if (u_FirstMip == 1) {
        float depth = texture(u_SourceDepth, v_UV).r;
        FragColor = vec4(depth, depth, depth, 1.0);
        return;
    }

    ivec2 sourceSize = max(textureSize(u_SourceDepth, u_SourceMip), ivec2(1));
    vec2 texel = 1.0 / vec2(sourceSize);

    float depth = 1.0;
    depth = min(depth, textureLod(u_SourceDepth, v_UV + texel * vec2(-0.5, -0.5), float(u_SourceMip)).r);
    depth = min(depth, textureLod(u_SourceDepth, v_UV + texel * vec2( 0.5, -0.5), float(u_SourceMip)).r);
    depth = min(depth, textureLod(u_SourceDepth, v_UV + texel * vec2(-0.5,  0.5), float(u_SourceMip)).r);
    depth = min(depth, textureLod(u_SourceDepth, v_UV + texel * vec2( 0.5,  0.5), float(u_SourceMip)).r);

    FragColor = vec4(depth, depth, depth, 1.0);
}
#endif

            ENDGLSL
        }
    }

    FallBack "VibeEngine/DeferredLighting"
}
