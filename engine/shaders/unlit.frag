#version 450

layout(location = 0) in vec3 v_Color;
layout(location = 1) in vec2 v_TexCoord;

layout(set = 0, binding = 0) uniform sampler2D u_Texture;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) int useTexture;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec3 baseColor = v_Color;
    if (pc.useTexture == 1)
        baseColor = texture(u_Texture, v_TexCoord).rgb;
    FragColor = vec4(baseColor, 1.0);
}
