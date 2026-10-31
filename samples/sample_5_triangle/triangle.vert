#version 450
layout(location=0) in vec3 position;
layout(push_constant) uniform Draw { mat4 world; } draw;
void main() { gl_Position = draw.world * vec4(position, 1.0); }
