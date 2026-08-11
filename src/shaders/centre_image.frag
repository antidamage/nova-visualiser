#version 460 core

// Centre-image composite.
//
// The other half of the centre slot: where `text_overlay.frag` draws a message,
// this draws a transparent PNG, at the same place, scaled by the same
// `__messageScale` axis. Only one of the two ever draws in a frame -- which one
// is the simulation's decision, not the renderer's.
//
// Two planes so a colour-theme change can transition between images. `progress`
// runs 0 to 1 over the change, already shaped by the authored ramp
// (`nova::transitionRamp`), so the picture's centrepiece settles exactly as its
// colours do. What that progress MEANS depends on the mode:
//
//   0 cross-fade -- progress is the incoming plane's weight, both planes drawn;
//   1 flip       -- the image collapses along the axis and the planes SWAP at
//                   the exact midpoint, so only ever one is drawn;
//   2 slide      -- the outgoing plane leaves over the first half and the
//                   incoming one arrives over the second, optionally cut into
//                   counter-travelling segments.
//
// The arithmetic is stated once in core/centre_image_transition.h and mirrored
// here and in the tvOS Metal port; the conformance corpus locks the three
// against each other.
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
uniform float progress;
uniform bool hasFrom;
// The latched transition. `segments` is divisions + 1, resolved on the CPU.
uniform int mode;
uniform float axisRadians;
uniform int segments;
uniform bool returnFromOrigin;
uniform float frameAspect;

const int MODE_CROSSFADE = 0;
const int MODE_FLIP = 1;
const int MODE_SLIDE = 2;
const float FLIP_EPSILON = 1e-4;
const float PI = 3.14159265358979323846;

// One plane, sampled inside its own rectangle and clear outside it.
//
// `incoming` says which side of the change this plane is: it selects the leg of
// a slide, and nothing else. The transform is applied in ASPECT-CORRECTED space
// -- x scaled by the frame's aspect before the rotation and unscaled after --
// because the axis is an angle on screen, and rotating in raw normalised
// coordinates would skew it on anything but a square frame.
vec4 plane(sampler2D image, vec2 halfExtent, bool incoming) {
  if (halfExtent.x <= 0.0 || halfExtent.y <= 0.0) return vec4(0.0);

  vec2 centred = (uv - vec2(0.5)) * vec2(frameAspect, 1.0);
  if (mode != MODE_CROSSFADE) {
    vec2 along = vec2(cos(axisRadians), sin(axisRadians));
    vec2 across = vec2(-along.y, along.x);
    vec2 local = vec2(dot(centred, along), dot(centred, across));

    if (mode == MODE_FLIP) {
      // Collapse to nothing at the midpoint and back out again. Below the
      // epsilon the plane is edge-on: there is no image left to sample, and
      // dividing by it would smear one column of texels across the frame.
      float scale = abs(cos(PI * clamp(progress, 0.0, 1.0)));
      if (scale < FLIP_EPSILON) return vec4(0.0);
      local.x /= scale;
    } else {
      // The segment index comes from the ACROSS coordinate, which displacement
      // never changes -- so a fragment can be asked which segment it belongs to
      // before knowing where that segment has moved to, and no search is needed.
      vec2 halfLocal = vec2(
        abs(halfExtent.x * frameAspect * along.x) + abs(halfExtent.y * along.y),
        abs(halfExtent.x * frameAspect * across.x) + abs(halfExtent.y * across.y));
      int index = 0;
      if (halfLocal.y > 0.0) {
        float position = (local.y / halfLocal.y) * 0.5 + 0.5;
        index = clamp(int(floor(position * float(segments))), 0, segments - 1);
      }
      // Alternating by parity: 0 divisions is a solid image, 1 pushes the two
      // halves apart, 2 sends the outer sections one way and the middle the
      // other.
      float direction = (index % 2 == 0) ? 1.0 : -1.0;
      float frameSpan = 0.5 * (abs(along.x) * frameAspect + abs(along.y));
      float clearDistance = frameSpan + halfLocal.x;

      float clamped = clamp(progress, 0.0, 1.0);
      float offset;
      if (!incoming) {
        offset = direction * clearDistance * (clamped * 2.0);
      } else {
        float arriving = clamped * 2.0 - 1.0;
        // Opposite edge carries on the way it left and enters from the far
        // side; origin edge reverses and comes back the way it went. Named
        // `travel` rather than `sign` so it cannot shadow the GLSL built-in.
        float travel = returnFromOrigin ? -direction : direction;
        offset = travel * clearDistance * (arriving - 1.0);
      }
      local.x -= offset;
    }

    centred = along * local.x + across * local.y;
  }

  vec2 texel = centred / vec2(frameAspect, 1.0) / (halfExtent * 2.0) + vec2(0.5);
  // Outside the fitted rectangle there is no image, which is what makes a scale
  // above 1 read as a crop rather than a squash -- and what carries a slid
  // segment off frame rather than wrapping it.
  if (any(lessThan(texel, vec2(0.0))) || any(greaterThan(texel, vec2(1.0)))) return vec4(0.0);
  // CPU rows are top-down; GL texture coordinates are bottom-up.
  return texture(image, vec2(texel.x, 1.0 - texel.y));
}

void main() {
  vec4 accumulated = vec4(0.0);
  float weight = clamp(progress, 0.0, 1.0);

  if (mode == MODE_FLIP && hasFrom) {
    // Exactly one plane, and the swap IS the midpoint. That instant is what
    // makes a flip read as one object turning over rather than as two images
    // blending through each other.
    accumulated = weight >= 0.5
      ? plane(imageTo, halfExtentTo, true)
      : plane(imageFrom, halfExtentFrom, false);
  } else if (mode == MODE_SLIDE && hasFrom) {
    // Also one at a time, for the same reason: the outgoing image is off frame
    // by the time the incoming one starts arriving, so the two legs never
    // overlap and neither needs fading.
    accumulated = weight >= 0.5
      ? plane(imageTo, halfExtentTo, true)
      : plane(imageFrom, halfExtentFrom, false);
  } else {
    accumulated = plane(imageTo, halfExtentTo, true) * weight;
    if (hasFrom) {
      // Both sides are premultiplied and the weights sum to 1, so a straight
      // weighted sum is the cross-dissolve -- no over-composite, which would
      // make the outgoing image show through the incoming one's transparent
      // parts at full strength for the whole transition.
      accumulated += plane(imageFrom, halfExtentFrom, false) * (1.0 - weight);
    }
  }

  if (accumulated.a <= 0.001) discard;
  outColor = accumulated;
}
