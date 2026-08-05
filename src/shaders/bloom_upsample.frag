#version 460 core

// Progressive bloom, upsample half: a 3x3 tent filter accumulated additively
// back up the mip chain.

layout(binding = 0) uniform sampler2D source;
uniform vec2 texelSize;
uniform float radius;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec2 t = texelSize * radius;

  // Alpha rides along with colour for the same reason as the downsample: it is
  // the coverage signal the composite reads.
  vec4 a = texture(source, uv + vec2(-t.x,  t.y));
  vec4 b = texture(source, uv + vec2( 0.0,  t.y));
  vec4 c = texture(source, uv + vec2( t.x,  t.y));
  vec4 d = texture(source, uv + vec2(-t.x,  0.0));
  vec4 e = texture(source, uv);
  vec4 f = texture(source, uv + vec2( t.x,  0.0));
  vec4 g = texture(source, uv + vec2(-t.x, -t.y));
  vec4 h = texture(source, uv + vec2( 0.0, -t.y));
  vec4 i = texture(source, uv + vec2( t.x, -t.y));

  vec4 result = e * 4.0;
  result += (b + d + f + h) * 2.0;
  result += (a + c + g + i);
  result *= 1.0 / 16.0;

  outColor = result;
}
