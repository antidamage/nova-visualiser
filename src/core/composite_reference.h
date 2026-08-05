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

  const Vec4 out{foregroundR + in.background.x * reveal, foregroundG + in.background.y * reveal,
                 foregroundB + in.background.z * reveal,
                 clampValue(foregroundAlpha + reveal, 0.0f, 1.0f)};
  return {std::max(0.0f, out.x), std::max(0.0f, out.y), std::max(0.0f, out.z), out.w};
}

}  // namespace nova
