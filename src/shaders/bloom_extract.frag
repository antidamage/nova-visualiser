#version 460 core

// Bright-pass. Port of `phonoscope_bloom_extract`.

layout(binding = 0) uniform sampler2D source;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 texel = texture(source, uv);
  float brightness = max(texel.r, max(texel.g, texel.b));
  float contribution = max(0.0, brightness - 0.16);
  // Alpha is coverage -- how much of the background this pixel hides -- and a
  // glow hides nothing: it is light added on top of whatever is behind it. This
  // used to carry `contribution`, an unbounded HDR value, which the composite
  // then read as coverage and saturated to 1, erasing the background colour in
  // a wide halo around everything bright. The scene pass alone decides
  // coverage; bloom only ever adds colour.
  outColor = vec4(texel.rgb * contribution * 1.45, 0.0);
}
