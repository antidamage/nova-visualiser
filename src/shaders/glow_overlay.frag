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
// 1-10. Multiplies the glow's RGB.
uniform float overdrive;
// 1 clamps the overdriven glow to 0-1; 0 lets it run past 1 towards white.
uniform int glowClamped;
// The `__glowBlend` axis itself: 0 screen, 1 multiply, 2 overlay.
uniform int blendMode;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 baseColor = texture(base, uv);
  // Blend modes are defined on display-referred colour and the composite target
  // is HDR, so the glow is normally brought back into 0-1 first.
  // Overdrive multiplies the glow's RGB. Clamped, that saturates it; unclamped
  // the excess carries into the blend and blows the picture out to white.
  vec3 driven = max(texture(glow, uv).rgb * clamp(overdrive, 1.0, 10.0), vec3(0.0));
  vec3 glowColor = glowClamped != 0 ? min(driven, vec3(1.0)) : driven;
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
