#version 460 core

// The last pass over the picture: lay a blurred copy of the finished frame back
// over itself with a Photoshop blend mode.
//
// "Finished" is meant literally -- this runs after the composite *and* after
// the centre message, so the text blooms with the rest of the image instead of
// floating outside the effect.
//
// The arithmetic is locked by src/core/glow_overlay_reference.h and mirrored by
// `phonoscope_glow_overlay` in PhonoscopeShader.metal.

layout(binding = 0) uniform sampler2D base;
layout(binding = 1) uniform sampler2D glow;
uniform float opacity;
// The `__glowBlend` axis itself: 0 screen, 1 multiply, 2 overlay.
uniform int blendMode;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 baseColor = texture(base, uv);
  // Blend modes are defined on display-referred colour. The composite target is
  // HDR, so an unclamped highlight would saturate `screen` to white across the
  // whole frame and stop `multiply` from darkening anything.
  vec3 glowColor = clamp(texture(glow, uv).rgb, 0.0, 1.0);
  vec3 baseRgb = max(baseColor.rgb, vec3(0.0));
  float amount = clamp(opacity, 0.0, 1.0);

  // Photoshop overlay: multiply where the base is dark, screen where it is
  // light, with the base choosing which. `step` keeps that per channel.
  vec3 overlaid = mix(2.0 * baseRgb * glowColor,
                      1.0 - 2.0 * (1.0 - baseRgb) * (1.0 - glowColor),
                      step(0.5, baseRgb));

  vec3 blended = blendMode == 1
      ? baseRgb * (1.0 - amount + glowColor * amount)
      : blendMode == 2
          ? baseRgb + amount * (overlaid - baseRgb)
          : baseRgb + amount * (glowColor - baseRgb * glowColor);

  // Coverage is passed through untouched: this is a look on the picture, not a
  // layer of its own, and the letterboxed modules composite over a separate
  // backdrop that must still show through.
  outColor = vec4(blended, baseColor.a);
}
