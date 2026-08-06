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
// How the scene layer meets the backdrop: 0 linear, 1 screen, 2 overlay,
// 3 multiply, already snapped off the driven `__sceneBlend` axis by
// `sceneBlendModeFor`. Mirrors core/composite_reference.h exactly.
uniform int sceneBlendMode;
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

  // `backdropColor` is premultiplied, so its own coverage is already in it --
  // unlike core/composite_reference.h, which carries the backdrop straight and
  // folds `backdropAlpha` into `reveal` instead. Same result, stated in the
  // terms each side actually holds.
  float reveal = 1.0 - foregroundAlpha;
  vec3 sourceOver = foreground + backdropColor * reveal;
  float alpha = clamp(foregroundAlpha + backdropAlpha * reveal, 0.0, 1.0);

  if (sceneBlendMode == 0) {
    outColor = vec4(max(sourceOver, vec3(0.0)), alpha);
    return;
  }

  // Display-referred blends, so both sides come into 0-1 first: the scene target
  // is HDR and an unclamped multiply of two >1 values is not a multiply of
  // anything meaningful.
  //
  // BOTH sides enter un-premultiplied by their own coverage -- multiplying by a
  // partly-covered layer would otherwise read as multiplying by black. The
  // backdrop always had this; the scene did not, which is why a lattice covering
  // a fraction of the frame behaved like an opaque black plate under multiply
  // and overlay. Bloom is excluded from the blended base because it carries no
  // coverage and so has no un-premultiplied form; it is additive light and is
  // added back after the blend. Mirrors core/composite_reference.h exactly.
  vec3 straightBackdrop = backdropAlpha > 0.0 ? backdropColor / backdropAlpha : vec3(0.0);
  vec3 s = clamp(base.rgb * (foregroundAlpha > 0.0 ? 1.0 / foregroundAlpha : 0.0), 0.0, 1.0);
  vec3 b = clamp(straightBackdrop, 0.0, 1.0);

  vec3 blended = s;
  if (sceneBlendMode == 1) {
    blended = s + b - s * b;
  } else if (sceneBlendMode == 3) {
    blended = s * b;
  } else {
    // Photoshop overlay, with the BACKDROP choosing the branch: the backdrop is
    // the base layer here and the scene is what is laid over it.
    vec3 low = 2.0 * s * b;
    vec3 high = 1.0 - 2.0 * (1.0 - s) * (1.0 - b);
    blended = mix(low, high, step(vec3(0.5), b));
  }

  // The scene's own alpha is the mask. Where it does not cover, the backdrop
  // passes through untouched; where it covers fully, the mode applies at full
  // strength. This is the step that makes the particle layer an alpha mask
  // rather than a black plate.
  vec3 overBackdrop = mix(b, blended, foregroundAlpha) * alpha;
  // Where the backdrop does not cover, there is nothing to blend with, so the
  // result falls back to plain source-over. This keeps the non-fluid path -- a
  // flat palette colour that may be fully transparent -- from collapsing to
  // black under multiply. `sourceOver` carries no glow here, so adding it once
  // below cannot double it.
  vec3 sourceOverBase = base.rgb + backdropColor * reveal;
  vec3 color = mix(sourceOverBase, overBackdrop, backdropAlpha) + glow.rgb;
  outColor = vec4(max(color, vec3(0.0)), alpha);
}
