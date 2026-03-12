#version 450

layout(location = 0) in vec3 v_Dir;

layout(set = 0, binding = 0) uniform sampler2D u_Texture;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec3 topColor;
    layout(offset = 80) vec3 bottomColor;
    layout(offset = 92) int useTexture;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec3 dir = normalize(v_Dir);
    if (pc.useTexture == 1) {
        float u = atan(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
        float v = asin(clamp(dir.y, -1.0, 1.0)) / 3.14159265 + 0.5;
        FragColor = vec4(texture(u_Texture, vec2(u, v)).rgb, 1.0);
    } else {
        float t = dir.y * 0.5 + 0.5;
        vec3 color = mix(pc.bottomColor, pc.topColor, t);
        FragColor = vec4(color, 1.0);
    }
}
