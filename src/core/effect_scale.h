#pragma once

#include <algorithm>
#include <cmath>

namespace nova {

// Resolution adaptation shared by the GLSL renderer's uniforms and the tvOS
// parity corpus. Phonoscope was authored at 1080p. Pixel-sized soft effects
// grow linearly at denser outputs, while normalized scene geometry does not.
inline float visualEffectScale(float outputHeight) {
  return std::max(1.0f, outputHeight / 1080.0f);
}

// Glow-overlay blur geometry. Authored 0-20 in the dashboard; one unit is this
// many pixels of Gaussian sigma at the 1080p authoring resolution, scaled with
// output density like every other pixel-sized soft effect.
inline constexpr float kGlowBlurSigmaPerUnit = 1.2f;

// Both engines blur a quarter-resolution copy, so the sigma the shaders receive
// is in *that* target's texels. Splitting these two apart is a silent softness
// mismatch rather than a visible failure, which is why they live together.
inline constexpr int kGlowBlurDownsample = 4;

inline float glowBlurSigmaPixels(float blurAmount, float outputHeight) {
  const float amount = std::max(0.0f, std::min(20.0f, blurAmount));
  return amount * kGlowBlurSigmaPerUnit * visualEffectScale(outputHeight);
}

inline float glowBlurSigmaTexels(float blurAmount, float outputHeight) {
  return glowBlurSigmaPixels(blurAmount, outputHeight) /
         static_cast<float>(kGlowBlurDownsample);
}

// Glow-overlay blend mode as a driven parameter. The mode is one of a few
// discrete looks, but every Phonoscope driver produces a continuous number, so
// the dashboard authors it on a whole-numbered axis (`__glowBlend`) and both
// engines snap to the nearest mode. There is no cross-fade between the blends
// by design -- a swap on the beat is meant to read as a switch, not a
// dissolve.
//
// The numbering is the axis itself, so it is also what the shaders' `blendMode`
// uniform carries: 0 screen, 1 multiply, 2 overlay. Modes are only ever
// appended, because a stored driver range is a pair of numbers on this axis.
//
// Like the sigma mapping above this is a render-path contract rather than
// test-only arithmetic, which is why it lives here and not in
// `core/glow_overlay_reference.h`.
enum class GlowBlendMode { Screen = 0, Multiply = 1, Overlay = 2 };

inline constexpr int kGlowBlendModeCount = 3;

// Snapping to the nearest whole mode keeps the original two-mode cut exactly
// where it was: anything below 0.5 is still screen, and 0.5 is still multiply.
inline GlowBlendMode glowBlendModeFor(float blendValue) {
  const float clamped =
      std::max(0.0f, std::min(static_cast<float>(kGlowBlendModeCount - 1), blendValue));
  return static_cast<GlowBlendMode>(static_cast<int>(std::floor(clamped + 0.5f)));
}

// How the scene layer meets the backdrop, as a driven parameter on the same
// kind of whole-numbered axis as `__glowBlend`. This is a separate axis from
// the glow's, so the numbering is free rather than inherited -- but it is still
// append-only for the same reason: a stored driver range is a pair of numbers
// on it.
//
// `Linear` is the original source-over term and therefore the default, so an
// undriven picture is composited exactly as it always was. The other three are
// only definable at the composite: the particle pass renders to its own target
// with no backdrop to blend against, so multiply and overlay have nothing to
// operate on until the two layers meet.
//
// The mode applies to the whole scene layer -- dots, trails, wires and bloom
// together -- against the backdrop. It is not a blend between individual
// particles, which accumulate additively among themselves as they always have.
enum class SceneBlendMode { Linear = 0, Screen = 1, Overlay = 2, Multiply = 3 };

inline constexpr int kSceneBlendModeCount = 4;

inline SceneBlendMode sceneBlendModeFor(float blendValue) {
  const float clamped =
      std::max(0.0f, std::min(static_cast<float>(kSceneBlendModeCount - 1), blendValue));
  return static_cast<SceneBlendMode>(static_cast<int>(std::floor(clamped + 0.5f)));
}

// Dot diameter in true device pixels, as a clip-space radius.
//
// This is the ONE place in the engine where an authored dot size meets the
// output resolution, and it deliberately does NOT go through
// `visualEffectScale` above. Every other pixel-sized quantity here is authored
// against the 1080-line reference and MULTIPLIED by the ratio at denser outputs
// so its visual weight stays constant; a dot is DIVIDED by the height instead.
// A dot is an object with a size, not a weight. 50px is 50 real pixels at 1080p
// and 50 real pixels at 4K, so it reads proportionally smaller on a denser
// output. See `nova-visualiser-modules/specs/particle-grid-dot-size.md`.
//
// The identity that makes the divide exact: `particle.vert` sets local space to
// `corner * effectScale` and the quad offset to `corner * positionSize.w *
// effectScale`, so `offset = local * w`, and `particle.frag` cuts the core at
// `|local| = 1`. The drawn core radius in clip is therefore `w` exactly,
// whatever `effectScale` is. Clip Y spans `outputHeight` pixels, so a radius of
// `w` is a DIAMETER of `w * outputHeight` pixels.
//
// The 200px rail is an order of magnitude above the module's declared 50, so a
// corrupt setting cannot allocate a screen-filling quad per entity.
inline constexpr float kMaxDotSizePixels = 200.0f;

inline float dotSizeClip(float dotSizePixels, float outputHeight) {
  const float pixels = std::max(0.0f, std::min(kMaxDotSizePixels, dotSizePixels));
  return pixels / std::max(1.0f, outputHeight);
}

struct EffectDimensions {
  float scale = 1;
  float dotCore = 0;
  float wireWidth = 0;
  float haloRadius = 0;
  float trailLength = 0;
  float trailWidth = 0;
  float bloomRadius = 0;
  float backgroundFeatureRadius = 0;
};

inline EffectDimensions resolveEffectDimensions(float outputHeight, float dotCore,
                                                 float wireWidth, float haloRadius,
                                                 float trailLength, float trailWidth,
                                                 float bloomRadius,
                                                 float backgroundFeatureRadius) {
  const float scale = visualEffectScale(outputHeight);
  return {scale, dotCore, wireWidth, haloRadius * scale, trailLength * scale,
          trailWidth * scale, bloomRadius * scale, backgroundFeatureRadius};
}

}  // namespace nova
