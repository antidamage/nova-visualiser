// How an image is fitted and scaled into a slot, stated once so both engines can
// be tested against it.
//
// TEST-ONLY, in the same sense as centre_image_reference.h: the real fit is a
// uniform handed to `centre_image.frag` / `fluid_background.frag` here and the
// matching Metal passes on tvOS. This is the arithmetic those must agree on, in
// a form a conformance case can evaluate.
//
// ONE function for two slots. The centre image and the background image are
// sized by the same controls -- a mode, a width, a height, a scale and a
// proportional flag -- because they are the same question asked about two
// rectangles, and having answered it twice is how the two drifted apart in the
// first place (the centre was height-authored, the backdrop width-authored, and
// neither had a mode or a scale).
//
// Changing this means changing all of: this header, both shaders' uniforms, the
// Metal ports, and the recorded expectation in `tests/conformance/image-fit/`.
#pragma once

#include <algorithm>
#include <cmath>

namespace nova {

// Half-extents of the drawn image, in normalised frame coordinates where the
// whole frame is 1 x 1 and the centre is (0.5, 0.5). Named for the slot rather
// than the centre, because both slots use it.
struct ImageExtent {
  float halfWidth = 0;
  float halfHeight = 0;
};

// How the image is sized against the frame. APPEND-ONLY: a stored binding keeps
// a numeric range on the `__bgFit` / `__centreFit` axes, so renumbering silently
// repoints configurations that were authored against the old numbers.
enum class ImageFit {
  Manual = 0,
  Fit = 1,
  Fill = 2,
};

inline constexpr int kImageFitModeCount = 3;

inline ImageFit imageFitFor(double value) {
  if (value >= 1.5) return ImageFit::Fill;
  if (value >= 0.5) return ImageFit::Fit;
  return ImageFit::Manual;
}

// The scale axis's bounds, shared by `__messageScale` and `__bgScale`. Identical
// to the clamp in text_overlay.frag, because the SLOT is scaled, not whatever
// happens to be in it.
inline constexpr float kImageScaleMinimum = 0.1f;
inline constexpr float kImageScaleMaximum = 5.0f;

// Half-extents of the drawn image.
//
// FOUR inputs, and they do not all apply at once -- which is the whole point of
// the mode leading the control set:
//
//  - `fit` decides where the base size comes from. Manual takes it from the
//    width and height below; fit and fill DERIVE both from the image's own
//    proportions and ignore them.
//  - `widthFraction` is the authored axis under Manual: how wide the image is as
//    a share of the frame.
//  - `heightFraction` is the other one, used only under Manual with
//    `proportional` off. With it on, height follows the width and the source's
//    shape, so the picture can never be squashed.
//  - `scale` is the driven multiplier on top, and it applies in EVERY mode --
//    that is what lets a fitted or filled backdrop still thump on the beat.
//
// The result is allowed past the frame edge: the shaders discard what falls
// outside, which is how a large value reads as a crop rather than a squash, and
// is exactly what makes `Fill` a cover rather than a letterbox.
inline ImageExtent imageHalfExtent(float frameAspect, float imageAspect,
                                   float widthFraction, float heightFraction,
                                   float scale, ImageFit fit, bool proportional) {
  // A zero or negative aspect is a decode that produced no pixels, or a frame
  // with no area. Draw nothing rather than dividing by it.
  if (!(frameAspect > 0.0f) || !(imageAspect > 0.0f)) return {0.0f, 0.0f};

  const float clampedScale =
      std::max(kImageScaleMinimum, std::min(kImageScaleMaximum, scale));

  // The height a proportional image of a given width must have. Both axes are
  // normalised to the frame, so the source's pixel aspect has to be rescaled by
  // the frame's own shape or a square image would not come out square.
  const float heightPerWidth = frameAspect / imageAspect;

  if (fit == ImageFit::Manual) {
    const float halfWidth = 0.5f * std::max(0.0f, widthFraction) * clampedScale;
    const float halfHeight = proportional
        ? halfWidth * heightPerWidth
        : 0.5f * std::max(0.0f, heightFraction) * clampedScale;
    return {halfWidth, halfHeight};
  }

  // Fit and fill are the same construction with opposite extremes: the image
  // keeps its proportions and is grown until one axis touches the frame (fit)
  // or until both cover it (fill). Half-extents of 0.5 are exactly the frame, so
  // the two candidates are "half height if width is the limit" and "half height
  // if height is the limit", and the mode picks which one wins.
  const float heightWhenWidthFills = 0.5f * heightPerWidth;
  const float halfHeight = fit == ImageFit::Fit
      ? std::min(0.5f, heightWhenWidthFills)
      : std::max(0.5f, heightWhenWidthFills);
  const float scaled = halfHeight * clampedScale;
  return {scaled / heightPerWidth, scaled};
}

}  // namespace nova
