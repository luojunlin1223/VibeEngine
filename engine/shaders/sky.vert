#version 450

layout(location = 0) in vec3 a_Position;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

layout(location = 0) out vec3 v_Dir;

void main() {
    v_Dir = a_Position;
    vec4 pos = pc.mvp * vec4(a_Position, 1.0);
    gl_Position = pos.xyww; // depth = 1.0 (far plane)
}
