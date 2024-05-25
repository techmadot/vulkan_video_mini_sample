#version 450

layout(location=0) in vec3 inColor;
layout(location=1) in vec2 inUV0;

layout(location=0) out vec4 outColor;

layout(set=0,binding=1)
uniform sampler2D texVideoYCbCr;

void main()
{
  //outColor = vec4(inColor, 1);
  outColor = vec4(inUV0, 0, 1);
  vec3 color = texture(texVideoYCbCr, inUV0).rgb;
  outColor = vec4(color, 1);
}
