Shader "Outline"
{
    Properties
    {
        _OutlineColor ("Outline Color", Color) = (1.0, 0.5, 0.0, 1.0)
    }

    SubShader
    {
        Cull Off
        ZWrite Off
        ZTest LessEqual

        Pass
        {
            GLSLPROGRAM

            #ifdef VERTEX
            layout(location = 0) in vec3 a_Position;

            uniform mat4 u_MVP;

            void main() {
                gl_Position = u_MVP * vec4(a_Position, 1.0);
            }
            #endif

            #ifdef FRAGMENT
            out vec4 FragColor;
            uniform vec4 u_OutlineColor;

            void main() {
                FragColor = u_OutlineColor;
            }
            #endif

            ENDGLSL
        }
    }
}
