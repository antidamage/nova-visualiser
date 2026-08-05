#version 460 core

// Centre-message composite.
//
// Two planes, because the halves are coloured differently. `glyphMask` is text
// coverage, tinted per frame by the chased palette so a theme change costs no
// re-rasterisation. `glyphColor` is premultiplied colour emoji straight from
// the font, which must composite as authored rather than being tinted.
//
// This used to be a hand-built 5x7 bitmap table with three hand-drawn symbols,
// which is why the message had to be drawn client-side on tvOS to get real
// emoji. It is now FreeType with Rajdhani plus Apple Color Emoji, so every
// client -- Apple TV, the macOS viewer and the browser debug view -- sees the
// same message baked into the stream.
in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D glyphMask;
layout(binding = 1) uniform sampler2D glyphColor;
uniform vec4 textColor;
uniform float messageScale;
uniform bool hasColor;

void main() {
  float scale = clamp(messageScale, 0.1, 5.0);
  vec2 sampleUv = (uv - vec2(0.5)) / scale + vec2(0.5);
  if (any(lessThan(sampleUv, vec2(0.0))) || any(greaterThan(sampleUv, vec2(1.0)))) discard;
  // CPU rows are top-down; GL texture coordinates are bottom-up.
  vec2 texUv = vec2(sampleUv.x, 1.0 - sampleUv.y);

  float mask = texture(glyphMask, texUv).r;
  // Output is premultiplied (blend func GL_ONE, GL_ONE_MINUS_SRC_ALPHA), so the
  // text contributes colour scaled by its own coverage.
  float textAlpha = mask * textColor.a;
  vec4 accumulated = vec4(textColor.rgb * textAlpha, textAlpha);

  if (hasColor) {
    vec4 emoji = texture(glyphColor, texUv);
    // Already premultiplied by FreeType. Over-composite it on the text.
    accumulated = emoji + accumulated * (1.0 - emoji.a);
  }

  if (accumulated.a <= 0.001) discard;
  outColor = accumulated;
}
