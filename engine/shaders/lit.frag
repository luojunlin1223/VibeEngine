#version 450

layout(location = 0) in vec3 v_Color;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec3 v_FragPos;
layout(location = 3) in vec2 v_TexCoord;

layout(set = 0, binding = 0) uniform sampler2D u_Texture;

layout(push_constant) uniform PushConstants {
    layout(offset = 128) int useTexture;
    // 12 bytes padding
    layout(offset = 144) vec3 lightDir;
    // 4 bytes padding
    layout(offset = 160) vec3 lightColor;
    layout(offset = 172) float lightIntensity;
    layout(offset = 176) vec3 viewPos;
    // 4 bytes padding
    layout(offset = 192) vec4 entityColor;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec3 baseColor = v_Color;
    if (pc.useTexture == 1)
        baseColor = texture(u_Texture, v_TexCoord).rgb;

    vec3 ambient = 0.15 * baseColor;
    float diff   = max(dot(v_Normal, pc.lightDir), 0.0);
    vec3 diffuse = diff * baseColor * pc.lightColor * pc.lightIntensity;

    vec3 viewDir    = normalize(pc.viewPos - v_FragPos);
    vec3 halfwayDir = normalize(pc.lightDir + viewDir);
    float spec      = pow(max(dot(v_Normal, halfwayDir), 0.0), 32.0);
    vec3 specular   = vec3(0.3) * spec * pc.lightColor * pc.lightIntensity;

    vec3 result = (ambient + diffuse + specular) * pc.entityColor.rgb;
    FragColor = vec4(result, pc.entityColor.a);
}
