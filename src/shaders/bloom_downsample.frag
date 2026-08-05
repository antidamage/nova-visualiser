#version 460 core

// Progressive bloom, downsample half. The tvOS renderer uses a single
// quarter-resolution 5-tap blur, which is adequate at 1080p but visibly banded
// and undersampled at 4K. A 13-tap partial Karis downsample through a mip chain
// costs less bandwidth than one full-resolution blur and produces a far wider,
// smoother bloom -- which matters because bloom is most of this visualiser's
// look.

layout(binding = 0) uniform sampler2D source;
uniform vec2 texelSize;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec2 t = texelSize;

  // Alpha is filtered alongside colour, not discarded. The composite uses the
  // bloom's alpha as coverage, so writing a constant 1.0 here would make the
  // whole frame read as fully covered and hide the background entirely.
  vec4 a = texture(source, uv + vec2(-2.0 * t.x,  2.0 * t.y));
  vec4 b = texture(source, uv + vec2( 0.0,        2.0 * t.y));
  vec4 c = texture(source, uv + vec2( 2.0 * t.x,  2.0 * t.y));
  vec4 d = texture(source, uv + vec2(-2.0 * t.x,  0.0));
  vec4 e = texture(source, uv);
  vec4 f = texture(source, uv + vec2( 2.0 * t.x,  0.0));
  vec4 g = texture(source, uv + vec2(-2.0 * t.x, -2.0 * t.y));
  vec4 h = texture(source, uv + vec2( 0.0,       -2.0 * t.y));
  vec4 i = texture(source, uv + vec2( 2.0 * t.x, -2.0 * t.y));

  vec4 j = texture(source, uv + vec2(-t.x,  t.y));
  vec4 k = texture(source, uv + vec2( t.x,  t.y));
  vec4 l = texture(source, uv + vec2(-t.x, -t.y));
  vec4 m = texture(source, uv + vec2( t.x, -t.y));

  vec4 result = e * 0.125;
  result += (a + c + g + i) * 0.03125;
  result += (b + d + f + h) * 0.0625;
  result += (j + k + l + m) * 0.125;

  outColor = result;
}
