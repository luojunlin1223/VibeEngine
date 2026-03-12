#version 450

layout(location = 0) in vec3 v_Color;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec3 v_FragPos;
layout(location = 3) in vec2 v_TexCoord;

layout(set = 0, binding = 0) uniform sampler2D u_Texture;

layout(push_constant) uniform PushConstants {
    layout(offset = 128) int useTexture;
    layout(offset = 144) vec3 lightDir;
    layout(offset = 160) vec3 lightColor;
    layout(offset = 172) float lightIntensity;
    layout(offset = 176) vec3 viewPos;
    layout(offset = 192) vec4 entityColor;
    layout(offset = 208) float metallic;
    layout(offset = 212) float roughness;
    layout(offset = 216) float ao;
} pc;

layout(location = 0) out vec4 FragColor;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float r) {
    float a = r * r;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float r) {
    float k = ((r + 1.0) * (r + 1.0)) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float r) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), r)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), r);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec3 albedo = v_Color;
    if (pc.useTexture == 1)
        albedo = texture(u_Texture, v_TexCoord).rgb;
    albedo *= pc.entityColor.rgb;

    float met = pc.metallic;
    float rou = max(pc.roughness, 0.04);
    float ao  = pc.ao;

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(pc.viewPos - v_FragPos);
    vec3 F0 = mix(vec3(0.04), albedo, met);

    vec3 L = normalize(pc.lightDir);
    vec3 H = normalize(V + L);
    vec3 radiance = pc.lightColor * pc.lightIntensity;

    float NDF = DistributionGGX(N, H, rou);
    float G   = GeometrySmith(N, V, L, rou);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - met);

    float NdotL = max(dot(N, L), 0.0);
    vec3 Lo = (kD * albedo / PI + spec) * radiance * NdotL;

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, pc.entityColor.a);
}
