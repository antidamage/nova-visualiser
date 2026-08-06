// How the centre image is fitted and scaled, stated once so both engines can be
// tested against it.
//
// TEST-ONLY, in the same sense as composite_reference.h: the real fit is a
// uniform handed to `centre_image.frag` here and a SwiftUI `.scaledToFit()` plus
// `.scaleEffect()` on tvOS. This is the arithmetic those two must agree on, in a
// form a conformance case can evaluate.
//
// Changing this means changing all three: this header, the shader's uniform, and
// the recorded expectation in `tests/conformance/centre-image/`.
#pragma once

#include <algorithm>
#include <cmath>

namespace nova {

// Local rather than added to vec.h: that header exists to mirror the SIMD3/SIMD4
// types the tvOS engine computes particles with, and this is a pair of screen
// fractions that belongs to this contract alone.
struct CentreImageExtent {
  float halfWidth = 0;
  float halfHeight = 0;
};

// The centre slot's scale bounds. Identical to the clamp in text_overlay.frag,
// because text and image share the one `__messageScale` axis -- the slot is
// scaled, not whatever happens to be in it.
inline constexpr float kCentreScaleMinimum = 0.1f;
inline constexpr float kCentreScaleMaximum = 5.0f;

// The centre image's default base height, as a percentage of the frame.
//
// A centre image is a centrepiece, not a backdrop: it sits in the middle of the
// picture at a legible size rather than covering it. A third of the frame's
// height is that size.
inline constexpr float kCentreImageDefaultHeightPercent = 33.0f;

// Half-extents of the drawn image, in normalised frame coordinates where the
// whole frame is 1 x 1 and the centre is (0.5, 0.5).
//
// TWO independent inputs, deliberately:
//
//  - `heightFraction` is the base size -- how tall the image is as a share of
//    the frame. It is set directly and rests where it is put.
//  - `scale` is the driven `__messageScale` effect, a multiplier on top that a
//    driver lane can sweep with the music.
//
// Height is the authored axis and width follows from the source's proportions,
// so the picture is never distorted: set how tall you want it and the aspect
// does the rest. The result is allowed past the frame edge -- the shader
// discards what falls outside, which is how a large value reads as a crop
// rather than a squash.
inline CentreImageExtent centreImageHalfExtent(float frameAspect, float imageAspect,
                                               float heightFraction, float scale) {
  const float clampedScale = std::max(kCentreScaleMinimum,
                                      std::min(kCentreScaleMaximum, scale));
  const float clampedHeight = std::max(0.0f, std::min(1.0f, heightFraction));
  // A zero or negative aspect is a decode that produced no pixels. Draw nothing
  // rather than dividing by it.
  if (!(frameAspect > 0.0f) || !(imageAspect > 0.0f)) return {0.0f, 0.0f};

  const float halfHeight = 0.5f * clampedHeight * clampedScale;
  // `imageAspect / frameAspect` converts the source's pixel aspect into frame
  // fractions: both axes are normalised to the frame, so the ratio has to be
  // rescaled by the frame's own shape or a square image would not come out
  // square.
  const float halfWidth = halfHeight * (imageAspect / frameAspect);
  return {halfWidth, halfHeight};
}

// The cross-fade between a rotation's outgoing and incoming centre images.
//
// A linear ramp over the rotation's own transition, NOT the exponential chase
// the palette uses. A chase only ever approaches its target, so the outgoing
// image would never quite reach zero and could never be released; a dissolve
// has to finish. Returns the incoming image's weight, 0 to 1.
inline float centreImageFade(float elapsedSeconds, float transitionSeconds) {
  if (!(transitionSeconds > 0.0f)) return 1.0f;
  return std::max(0.0f, std::min(1.0f, elapsedSeconds / transitionSeconds));
}

}  // namespace nova
