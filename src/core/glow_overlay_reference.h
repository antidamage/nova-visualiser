// The final glow-overlay formula, stated once so both engines can be tested
// against it.
//
// TEST-ONLY, in the same sense as `composite_reference.h`: nothing on the
// render path includes this header. The real pass is a fragment shader on each
// side (`src/shaders/glow_overlay.frag` here, `phonoscope_glow_overlay` in
// `PhonoscopeShader.metal` on tvOS). This is the arithmetic those two shaders
// must agree on, in a form a conformance case can evaluate.
//
// The pass runs last, over the finished picture -- after the composite and
// after the centre message -- so the message glows with everything else rather
// than sitting outside the effect.
//
// Changing this means changing all of: this header, both shaders, the recorded
// expectation in `tests/conformance/glow-overlay/`, and
// `ParitySelfTests.testGlowOverlayParity()`.
#pragma once

#include <algorithm>
#include <cmath>

#include "core/effect_scale.h"
#include "core/vec.h"

namespace nova {

// The blur's sigma mapping and quarter-resolution divisor are render-path
// contracts rather than test-only arithmetic, so they live in
// `core/effect_scale.h` alongside the other resolution adaptation. Only the tap
// layout and the blend formula are here.
//
// Separable 13-tap Gaussian, taps at i * (sigma/3) texels for i in -6..6, which
// covers +/-2 sigma. Because the tap stride is proportional to sigma the
// weights are independent of it: w(i) = exp(-i^2 / 18). That is what makes this
// a *fast* Gaussian -- the sigma only scales the offsets, so no weight table
// has to be rebuilt when a driver moves the blur amount every frame.
inline constexpr int kGlowBlurTapRadius = 6;

inline float glowBlurTapWeight(int tap) {
  return std::exp(-static_cast<float>(tap * tap) / 18.0f);
}

// `GlowBlendMode` and the axis it sits on are render-path contracts, so they
// live in `core/effect_scale.h`; only the blend arithmetic is here.
struct GlowOverlayInput {
  // The finished frame: composite plus centre message. W is coverage.
  Vec4 base;
  // The blurred copy of that same frame.
  Vec4 glow;
  // 0-1. The dashboard authors 0-100 and divides.
  float opacity = 0;
  GlowBlendMode mode = GlowBlendMode::Screen;
};

inline Vec4 glowOverlayReference(const GlowOverlayInput& in) {
  const float opacity = clampValue(in.opacity, 0.0f, 1.0f);
  // The glow is clamped because the composite target is HDR: an unclamped
  // highlight put through `screen` saturates the whole frame to white, and put
  // through `multiply` is not a darkening at all. Blend modes are defined on
  // display-referred colour, so this pass works there.
  const float glowR = clampValue(in.glow.x, 0.0f, 1.0f);
  const float glowG = clampValue(in.glow.y, 0.0f, 1.0f);
  const float glowB = clampValue(in.glow.z, 0.0f, 1.0f);
  const float baseR = std::max(0.0f, in.base.x);
  const float baseG = std::max(0.0f, in.base.y);
  const float baseB = std::max(0.0f, in.base.z);

  auto blend = [&](float base, float glow) {
    if (in.mode == GlowBlendMode::Multiply) {
      // Photoshop multiply, faded towards the untouched base by opacity.
      return base * (1.0f - opacity + glow * opacity);
    }
    if (in.mode == GlowBlendMode::Overlay) {
      // Photoshop overlay: multiply where the base is dark and screen where it
      // is light, with the *base* choosing which -- that is what makes it a
      // contrast blend rather than either half on its own. Doubled because each
      // half only has the corresponding half of the output range to fill.
      const float overlaid = base < 0.5f
                                 ? 2.0f * base * glow
                                 : 1.0f - 2.0f * (1.0f - base) * (1.0f - glow);
      return base + opacity * (overlaid - base);
    }
    // Photoshop screen: base + glow - base*glow, likewise faded.
    return base + opacity * (glow - base * glow);
  };

  // Coverage is deliberately untouched. This is a look applied to the picture,
  // not a layer of its own, so it must not change what the frame occludes --
  // the tvOS letterbox path composites the visualiser over a separate backdrop
  // and would otherwise gain an opaque rectangle.
  return {blend(baseR, glowR), blend(baseG, glowG), blend(baseB, glowB), in.base.w};
}

}  // namespace nova
