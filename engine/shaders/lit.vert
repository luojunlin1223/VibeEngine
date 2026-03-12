#version 450

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Color;
layout(location = 3) in vec2 a_TexCoord;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec3 v_Color;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec3 v_FragPos;
layout(location = 3) out vec2 v_TexCoord;

void main() {
    v_Color    = a_Color;
    v_TexCoord = a_TexCoord;

    mat3 normalMat = transpose(inverse(mat3(pc.model)));
    v_Normal  = normalize(normalMat * a_Normal);
    v_FragPos = vec3(pc.model * vec4(a_Position, 1.0));

    gl_Position = pc.mvp * vec4(a_Position, 1.0);
}
