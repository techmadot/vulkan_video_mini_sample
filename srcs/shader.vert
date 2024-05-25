#version 450

layout(location=0) out vec3 outColor;
layout(location=1) out vec2 outUV0;

vec2 positions[4] = vec2[](
  vec2(-0.5, 0.5),
  vec2( 0.5, 0.5),
  vec2(-0.5,-0.5),
  vec2( 0.5,-0.5)
);

vec3 colors[4] = vec3[](
  vec3(1.0, 0.0, 0.0),
  vec3(0.0, 1.0, 0.0),
  vec3(0.0, 0.0, 1.0),
  vec3(0.0, 0.0, 0.0)
);

vec2 texcoords[4] = vec2[](
  vec2(0.0, 1.0),
  vec2(1.0, 1.0),
  vec2(0.0, 0.0),
  vec2(1.0, 0.0)
);

void main()
{
  gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1);
  outColor = colors[gl_VertexIndex];
  outUV0 = texcoords[gl_VertexIndex];
}
