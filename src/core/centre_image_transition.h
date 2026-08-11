// How the centre image CHANGES: the ramp that times a transition, and the
// geometry of the three modes it can take.
//
// TEST-ONLY, in the same sense as centre_image_reference.h: the real transform
// is a handful of uniforms consumed by `centre_image.frag` and by the tvOS
// Metal centre pass. This is the arithmetic those two must agree on, in a form
// a conformance case can evaluate.
//
// Changing this means changing all of: this header, the shader's uniforms, the
// Metal port, `ParitySelfTests.testCentreImageParity()`, and the recorded
// expectation in `tests/conformance/centre-image/`.
#pragma once

#include <algorithm>
#include <cmath>

namespace nova {

// The three transition modes. APPEND-ONLY: a stored binding keeps a numeric
// range on the `__centreTransition` axis, so renumbering silently repoints
// configurations that were authored against the old numbers.
enum class CentreTransition {
  CrossFade = 0,
  Flip = 1,
  Slide = 2,
};

inline CentreTransition centreTransitionFor(double value) {
  if (value >= 1.5) return CentreTransition::Slide;
  if (value >= 0.5) return CentreTransition::Flip;
  return CentreTransition::CrossFade;
}

// The most a sliding image can be cut into. Ten cuts is eleven sections, by
// which point the picture reads as a shutter rather than as an object leaving.
inline constexpr int kCentreTransitionMaxDivisions = 10;

// Everything a transition was told at the moment it started.
//
// LATCHED, not sampled. The event that initiates a change owns the whole of it:
// these come from the settings groups that were in effect when the pulse fired,
// which is the entry being LEFT, and they are held for the run. Reading them
// live would mean the outgoing half of a change played under one rule and the
// incoming half under another, because advancing the rotation also swaps which
// settings groups apply.
struct CentreTransitionParams {
  CentreTransition mode = CentreTransition::CrossFade;
  // Radians, converted from the authored whole degrees exactly once, here.
  float axisRadians = 0.0f;
  // Cuts, not sections: 0 is a solid image, 1 splits it in half.
  int divisions = 0;
  // A slid section returns from the edge it left by rather than the far one.
  bool returnFromOrigin = false;
};

// The ramp control read as a MOTION PROFILE, for one-shot linear transitions.
//
// A pulse envelope and a transition are two different things wearing the same
// three-thumb control, and this is the second reading:
//
//   - attack is the EASE-IN, the stretch spent accelerating,
//   - hold is the FLAT middle, constant velocity,
//   - release is the EASE-OUT, the stretch spent decelerating,
//
// so the transition lasts exactly attack + hold + release. Returns progress
// from 0 to 1.
//
// Concretely a trapezoidal velocity profile integrated once. Peak velocity is
// whatever makes the area under it exactly 1, so the transition always
// completes on time however the phases are proportioned: lengthening the
// ease-in does not overshoot the end, it makes the middle faster.
//
// Zero-length phases are skipped rather than divided by, so a bare release is a
// pure ease-out and an all-zero ramp is an instant cut. Mirrored by
// `phonoscopeTransitionRamp` in the dashboard and `phonoscopeTransitionRamp` on
// tvOS.
inline float transitionRamp(float elapsedSeconds, float attackSeconds,
                            float holdSeconds, float releaseSeconds) {
  const float attack = std::max(0.0f, attackSeconds);
  const float hold = std::max(0.0f, holdSeconds);
  const float release = std::max(0.0f, releaseSeconds);
  const float total = attack + hold + release;
  const float elapsed = std::max(0.0f, elapsedSeconds);
  if (!(total > 0.0f) || elapsed >= total) return 1.0f;
  const auto clamp01 = [](float value) {
    return std::max(0.0f, std::min(1.0f, value));
  };
  // Half of each ramp's span carries half its velocity: the area of a triangle.
  const float peak = 1.0f / (attack * 0.5f + hold + release * 0.5f);
  if (elapsed < attack) return clamp01(peak * elapsed * elapsed / (2.0f * attack));
  if (elapsed < attack + hold) return clamp01(peak * (attack * 0.5f + (elapsed - attack)));
  const float decelerating = elapsed - attack - hold;
  return clamp01(peak * (attack * 0.5f + hold + decelerating
                         - decelerating * decelerating / (2.0f * release)));
}

// How far a segment must travel to be completely off frame.
//
// The frame's half-extent projected onto the travel direction, plus the image's
// own, both measured in the aspect-corrected space the transform works in. The
// smallest offset that always clears, at any angle -- a fixed distance would
// either leave a corner showing on the diagonal or waste most of the leg
// covering ground the image had already left.
inline float centreSlideClearDistance(float axisRadians, float frameAspect,
                                      float halfWidth, float halfHeight) {
  const float along = std::abs(std::cos(axisRadians));
  const float across = std::abs(std::sin(axisRadians));
  const float frameSpan = 0.5f * (along * std::max(0.0f, frameAspect) + across);
  const float imageSpan = along * std::abs(halfWidth) * std::max(0.0f, frameAspect)
                          + across * std::abs(halfHeight);
  return frameSpan + imageSpan;
}

// Which segment a point falls in, indexed along the axis's perpendicular.
//
// The perpendicular coordinate is the one displacement never changes, which is
// what makes this a direct lookup rather than a search: a fragment can be asked
// which segment it belongs to before knowing where that segment has moved to.
inline int centreSlideSegment(float across, float halfAcross, int divisions) {
  const int segments = std::max(1, std::min(kCentreTransitionMaxDivisions, divisions) + 1);
  if (!(halfAcross > 0.0f)) return 0;
  const float position = (across / halfAcross) * 0.5f + 0.5f;
  const int index = static_cast<int>(std::floor(position * static_cast<float>(segments)));
  return std::max(0, std::min(segments - 1, index));
}

// Which way a segment travels. Alternating by parity, so 0 divisions is a solid
// image, 1 pushes the two halves apart, and 2 sends the outer sections one way
// and the middle the other.
inline float centreSlideDirection(int segment) {
  return (segment % 2 == 0) ? 1.0f : -1.0f;
}

// How far along the axis a segment has been displaced, in aspect-corrected
// units, for the plane named by `incoming`.
//
// Two legs of one movement, each linear in `progress`: the outgoing image
// leaves over the first half and the incoming one arrives over the second. All
// the acceleration comes from the ramp, which is what makes an ease-in read as
// the image accelerating away and an ease-out as it settling into place.
inline float centreSlideOffset(float progress, bool incoming, float direction,
                               float clearDistance, bool returnFromOrigin) {
  const float clamped = std::max(0.0f, std::min(1.0f, progress));
  if (!incoming) return direction * clearDistance * (clamped * 2.0f);
  const float arriving = clamped * 2.0f - 1.0f;
  // Returning from the OPPOSITE edge carries on in the direction it left, so it
  // enters from the far side and the movement reads as one continuous sweep.
  // Returning from the ORIGIN edge reverses and comes back the way it went.
  const float sign = returnFromOrigin ? -direction : direction;
  return sign * clearDistance * (arriving - 1.0f);
}

// The flip's collapse factor along the axis: 1 at each end, 0 at the midpoint.
//
// Below the epsilon there is no image left to sample -- the plane is edge-on --
// and dividing by it would smear one row of texels across the frame.
inline constexpr float kCentreFlipEpsilon = 1e-4f;

inline float centreFlipScale(float progress) {
  const float clamped = std::max(0.0f, std::min(1.0f, progress));
  return std::abs(std::cos(3.14159265358979323846f * clamped));
}

// Which plane a flip draws this frame. Exactly one, and the swap is the exact
// midpoint -- that instant is what makes the flip read as one object turning
// over rather than as two images blending through each other.
inline bool centreFlipShowsIncoming(float progress) {
  return progress >= 0.5f;
}

}  // namespace nova
