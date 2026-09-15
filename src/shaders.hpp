#pragma once

#include <string>

inline const std::string WOBBLY_VERTEX_SHADER = R"#(
#version 300 es
precision highp float;

uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
}
)#";

inline const std::string WOBBLY_FRAGMENT_SHADER = R"#(
#version 300 es
precision highp float;

uniform sampler2D tex;
in vec2 v_texcoord;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(tex, v_texcoord);
}
)#";
