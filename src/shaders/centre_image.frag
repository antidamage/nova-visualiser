#version 460 core

// Centre-image composite.
//
// The other half of the centre slot: where `text_overlay.frag` draws a message,
// this draws a transparent PNG, at the same place, scaled by the same
// `__messageScale` axis. Only one of the two ever draws in a frame -- which one
// is the simulation's decision, not the renderer's.
//
// Two planes so a colour-theme change can dissolve between images. `fade` is the
// incoming one's weight and reaches 1 on a linear ramp over the rotation's own
// transition, so the picture's centrepiece settles exactly as its colours do.
//
// Both textures are premultiplied at decode time, matching the blend func
// (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) the message's emoji plane already relies on.
in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D imageTo;
layout(binding = 1) uniform sampler2D imageFrom;
// Half-extents in normalised frame coordinates, from
// core/centre_image_reference.h. Contain-fit and scale are resolved on the CPU
// so the arithmetic is testable by the conformance corpus rather than living
// only here.
uniform vec2 halfExtentTo;
uniform vec2 halfExtentFrom;
uniform float fade;
uniform bool hasFrom;

// One plane, sampled inside its own rectangle and clear outside it.
vec4 plane(sampler2D image, vec2 halfExtent) {
  if (halfExtent.x <= 0.0 || halfExtent.y <= 0.0) return vec4(0.0);
  vec2 local = (uv - vec2(0.5)) / (halfExtent * 2.0) + vec2(0.5);
  // Outside the fitted rectangle there is no image, which is what makes a scale
  // above 1 read as a crop rather than a squash.
  if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0)))) return vec4(0.0);
  // CPU rows are top-down; GL texture coordinates are bottom-up.
  return texture(image, vec2(local.x, 1.0 - local.y));
}

void main() {
  vec4 accumulated = plane(imageTo, halfExtentTo) * clamp(fade, 0.0, 1.0);
  if (hasFrom) {
    // Both sides are premultiplied and the weights sum to 1, so a straight
    // weighted sum is the cross-dissolve -- no over-composite, which would make
    // the outgoing image show through the incoming one's transparent parts at
    // full strength for the whole transition.
    accumulated += plane(imageFrom, halfExtentFrom) * (1.0 - clamp(fade, 0.0, 1.0));
  }

  if (accumulated.a <= 0.001) discard;
  outColor = accumulated;
}
