#version 450
layout(location = 0) in vec3 v_Color;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec3 v_FragPos;

layout(location = 0) out vec4 FragColor;

// Hardcoded directional light (matches OpenGL lit shader)
const vec3 LIGHT_DIR = normalize(vec3(0.3, 1.0, 0.5));
const vec3 AMBIENT   = vec3(0.15);

void main() {
    float diff = max(dot(v_Normal, LIGHT_DIR), 0.0);
    vec3 lighting = AMBIENT + vec3(1.0) * diff;

    // Simple specular (view-independent approximation with half-vector toward light)
    vec3 halfDir = normalize(LIGHT_DIR + vec3(0.0, 0.0, 1.0));
    float spec   = pow(max(dot(v_Normal, halfDir), 0.0), 32.0);
    lighting += vec3(0.3) * spec;

    FragColor = vec4(v_Color * lighting, 1.0);
}
