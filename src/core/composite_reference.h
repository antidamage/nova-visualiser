// The composite formula, stated once so both engines can be tested against it.
//
// TEST-ONLY. Nothing on the render path includes this header: the real
// composite is a fragment shader on each side
// (`src/shaders/composite.frag` here, `phonoscope_composite` in
// `PhonoscopeShader.metal` on tvOS), and the visual pipeline stays entirely on
// the GPU. This is the arithmetic those two shaders must agree on, in a form a
// conformance case can evaluate.
//
// It exists because the conformance corpus digests simulation particle state
// only -- it links against `nova_visualiser_core` with no GL and no CUDA, so it
// cannot see rendering at all. That left the composite as a two-engine surface
// with no drift detection, which is how the two sides came to disagree about
// whether bloom alpha means coverage. The formula is what actually drifts, so
// the formula is what gets locked.
//
// Changing this means changing all three: this header, both shaders, and the
// recorded expectation in `tests/conformance/composite/`.
#pragma once

#include <algorithm>

#include "core/effect_scale.h"
#include "core/vec.h"

namespace nova {

struct CompositeInput {
  // Scene pass output. RGB is premultiplied emissive colour; W is coverage --
  // how much of the background this pixel hides.
  Vec4 scene;
  // Bloom chain output, before the intensity multiply. W is deliberately
  // ignored: a glow is additive light and occludes nothing. Carrying bloom
  // alpha into coverage is the specific bug this case exists to catch.
  Vec4 bloom;
  // Background colour and its own alpha. A module that draws its background
  // elsewhere (tvOS letterboxes `particle-ripples` behind a separate fluid
  // layer) passes zero here and the whole term drops out.
  Vec4 background;
  float intensity = 1.45f;
  // How the scene layer meets the backdrop. `Linear` is the original term.
  SceneBlendMode blendMode = SceneBlendMode::Linear;
};

inline Vec4 compositeReference(const CompositeInput& in) {
  const float glowR = in.bloom.x * in.intensity;
  const float glowG = in.bloom.y * in.intensity;
  const float glowB = in.bloom.z * in.intensity;

  const float foregroundR = in.scene.x + glowR;
  const float foregroundG = in.scene.y + glowG;
  const float foregroundB = in.scene.z + glowB;

  // Coverage from the scene pass alone. See the note on `bloom` above.
  const float foregroundAlpha = clampValue(in.scene.w, 0.0f, 1.0f);
  const float backgroundAlpha = clampValue(in.background.w, 0.0f, 1.0f);
  const float reveal = backgroundAlpha * (1.0f - foregroundAlpha);

  const float sourceOverR = foregroundR + in.background.x * reveal;
  const float sourceOverG = foregroundG + in.background.y * reveal;
  const float sourceOverB = foregroundB + in.background.z * reveal;
  const float outAlpha = clampValue(foregroundAlpha + reveal, 0.0f, 1.0f);

  if (in.blendMode == SceneBlendMode::Linear) {
    return {std::max(0.0f, sourceOverR), std::max(0.0f, sourceOverG),
            std::max(0.0f, sourceOverB), outAlpha};
  }

  // The other three are display-referred blends, so both sides are brought into
  // 0-1 first: the scene target is HDR and an unclamped multiply of two >1
  // values is not a multiply of anything meaningful.
  //
  // BOTH sides enter NOT premultiplied by their own coverage. Multiplying by a
  // partly-covered layer would otherwise read as multiplying by black, so the
  // layer would darken rather than blend where it is thin. The backdrop always
  // had this treatment; the scene did not, which is why a lattice covering a
  // fraction of the frame behaved like an opaque black plate under multiply and
  // overlay -- the uncovered pixels blended black against the backdrop instead
  // of leaving it alone.
  //
  // Bloom is deliberately NOT part of the blended base. It carries no coverage
  // (see the note on `bloom` above), so it has no un-premultiplied form; it is
  // additive light and is added back after the blend, which is the same
  // property this file already asserts, honoured in the one place it was not.
  const float inverseCoverage = foregroundAlpha > 0 ? 1.0f / foregroundAlpha : 0.0f;

  auto blend = [&](float scene, float backdrop, float glow, float sourceOver) {
    const float s = clampValue(scene * inverseCoverage, 0.0f, 1.0f);
    const float b = clampValue(backdrop, 0.0f, 1.0f);
    float blended = s;
    switch (in.blendMode) {
      case SceneBlendMode::Screen: blended = s + b - s * b; break;
      case SceneBlendMode::Multiply: blended = s * b; break;
      case SceneBlendMode::Overlay:
        // Photoshop overlay, with the BACKDROP choosing the branch -- the
        // backdrop is the base layer here, the scene is what is laid over it.
        // The glow overlay picks the branch off its own base for the same
        // reason; the two agree on the shape, not on which input is the base.
        blended = b < 0.5f ? 2.0f * s * b : 1.0f - 2.0f * (1.0f - s) * (1.0f - b);
        break;
      case SceneBlendMode::Linear: break;
    }
    // The scene's own alpha is the mask. Where it does not cover, the backdrop
    // passes through untouched; where it covers fully, the mode applies at full
    // strength. This is the step that makes the particle layer an alpha mask
    // rather than a black plate.
    const float overBackdrop = (blended * foregroundAlpha + b * (1.0f - foregroundAlpha)) * outAlpha;
    // Where the backdrop does not cover, there is nothing to blend with, so the
    // result falls back to plain source-over. This is what keeps the non-fluid
    // path -- a flat palette colour that may be fully transparent -- from
    // collapsing to black under multiply.
    return overBackdrop * backgroundAlpha + sourceOver * (1.0f - backgroundAlpha) + glow;
  };

  const float backdropR =
      backgroundAlpha > 0 ? in.background.x : 0.0f;
  const float backdropG = backgroundAlpha > 0 ? in.background.y : 0.0f;
  const float backdropB = backgroundAlpha > 0 ? in.background.z : 0.0f;

  // The source-over fallback without the glow term, so adding the glow once
  // inside `blend` cannot double it.
  const float baseR = in.scene.x + in.background.x * reveal;
  const float baseG = in.scene.y + in.background.y * reveal;
  const float baseB = in.scene.z + in.background.z * reveal;

  return {std::max(0.0f, blend(in.scene.x, backdropR, glowR, baseR)),
          std::max(0.0f, blend(in.scene.y, backdropG, glowG, baseG)),
          std::max(0.0f, blend(in.scene.z, backdropB, glowB, baseB)), outAlpha};
}

}  // namespace nova
