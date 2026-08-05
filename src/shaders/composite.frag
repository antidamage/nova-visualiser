#version 460 core

// Scene + bloom + background composite. Port of `phonoscope_composite`, kept in
// linear HDR: unlike tvOS this does not write to an 8-bit drawable, because the
// encode pass downstream needs headroom to dither into 10-bit.

layout(binding = 0) uniform sampler2D scene;
layout(binding = 1) uniform sampler2D bloom;
// Animated backdrop, premultiplied by its own coverage. Only modules that
// declare `renderer.fluidBackground.speed` use it; everything else composites
// over the flat palette colour. Sampled outside the shared composite arithmetic
// on purpose -- see the note by `foregroundAlpha`.
layout(binding = 2) uniform sampler2D backgroundField;
uniform int useFluid;
uniform float intensity;
uniform vec4 background;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 base = texture(scene, uv);
  vec4 glow = texture(bloom, uv) * intensity;
  vec3 foreground = base.rgb + glow.rgb;
  // Coverage comes from the scene pass alone. Bloom is additive light and
  // occludes nothing, so folding its alpha in here (as `max(base.a, glow.a)`)
  // made every bright halo read as fully covered and cut the background out
  // around it. Kept as a named step because both engines must agree on it.
  float foregroundAlpha = clamp(base.a, 0.0, 1.0);

  // Backdrop, always premultiplied by its own coverage so the two sources
  // combine identically. With `useFluid` off this reduces exactly to the
  // original `background.rgb * background.a` term, which is what
  // core/composite_reference.h and the tvOS composite both encode.
  vec3 backdropColor;
  float backdropAlpha;
  if (useFluid != 0) {
    vec4 field = texture(backgroundField, uv);
    backdropColor = field.rgb;
    backdropAlpha = clamp(field.a, 0.0, 1.0);
  } else {
    backdropAlpha = clamp(background.a, 0.0, 1.0);
    backdropColor = background.rgb * backdropAlpha;
  }

  float reveal = 1.0 - foregroundAlpha;
  vec3 color = foreground + backdropColor * reveal;
  float alpha = foregroundAlpha + backdropAlpha * reveal;
  outColor = vec4(max(color, vec3(0.0)), clamp(alpha, 0.0, 1.0));
}
