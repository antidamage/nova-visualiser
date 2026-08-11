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

#include "core/centre_image_transition.h"
#include "core/image_fit_reference.h"

namespace nova {

// The centre slot's scale bounds, kept as names because text_overlay.frag and
// the tvOS message path both read them. They ARE the shared image bounds --
// text and image share the one `__messageScale` axis, and the slot is scaled,
// not whatever happens to be in it.
inline constexpr float kCentreScaleMinimum = kImageScaleMinimum;
inline constexpr float kCentreScaleMaximum = kImageScaleMaximum;

// The centre image's default base height, as a percentage of the frame.
//
// A centre image is a centrepiece, not a backdrop: it sits in the middle of the
// picture at a legible size rather than covering it. A third of the frame's
// height is that size.
inline constexpr float kCentreImageDefaultHeightPercent = 33.0f;

// The extent arithmetic itself now lives in `core/image_fit_reference.h`, as
// `imageHalfExtent`: the centre slot and the background image are sized by the
// same control set -- a mode, a width, a height, a scale and a proportional
// flag -- and having answered the same question twice is exactly how the two
// drifted apart. This header keeps the centre's own constants and its fade.
//
// The rule inverted when they were unified: the centre used to be
// height-authored with the width derived, and is now width-authored with the
// height derived (while proportional is on, which is the default and preserves
// the look). `phonoscope-migrate-v6.ts` converts stored configurations.

// A transition's progress against its own duration, 0 to 1.
//
// A ramp over the rotation's own transition, NOT the exponential chase the
// palette uses. A chase only ever approaches its target, so the outgoing image
// would never quite reach zero and could never be released; a transition has to
// finish.
//
// The shape within that duration is the authored ramp, from
// `centre_image_transition.h`: the whole-duration form here is what a caller
// with nothing but a total reaches for, and it degenerates to the pure ease-out
// every configuration written before the ramp meant this already had.
//
// For the cross-fade mode this IS the incoming image's weight; for the other
// two it is the position along their geometry.
inline float centreImageFade(float elapsedSeconds, float transitionSeconds) {
  return transitionRamp(elapsedSeconds, 0.0f, 0.0f, transitionSeconds);
}

}  // namespace nova
