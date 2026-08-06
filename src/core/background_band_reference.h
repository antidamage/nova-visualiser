// The backdrop band and frame vignette, stated once so both engines can be
// tested against it.
//
// TEST-ONLY. Nothing on the render path includes this header: the real band is a
// fragment shader on each side (`src/shaders/fluid_background.frag` here,
// `phonoscope_fluid_background` in `PhonoscopeShader.metal` on tvOS). This is
// the geometry and coverage arithmetic those two shaders must agree on, in a
// form a conformance case can evaluate without a GPU.
//
// It covers the part that is pure per-pixel geometry -- where the band is, how
// the four edge gradients combine, and what the frame outside the band is. The
// blob field itself is deliberately NOT here: it is a sum of four animated
// noise-ish peaks with no fixed answer to lock, and drift in it is visible as a
// different picture rather than as a different layout.
//
// Why it exists: the band used to be a hardcoded one-third letterbox on this
// side and a SwiftUI frame of `geometry.size.height / 3` on the other. Two
// unrelated expressions of the same constant is exactly the shape that drifts,
// and now that height, width, vignette opacity and vignette size are all driven
// parameters there are four more of them.
//
// Changing this means changing all three: this header, both shaders, and the
// recorded expectation in `tests/conformance/background-band/`.
#pragma once

#include <algorithm>

#include "core/vec.h"

namespace nova {

struct BackgroundBandInput {
  // Frame-space sample point, top-left origin, 0-1 on both axes. The GL shader
  // flips Y into this convention before doing anything else, because SwiftUI's
  // origin is top-left and the two must agree frame for frame.
  float u = 0;
  float v = 0;
  // Band extent as fractions of the frame. Driven (`__bgHeight` / `__bgWidth`).
  float heightFraction = 1.0f / 3.0f;
  float widthFraction = 1.0f;
  // The band's own pixel size, which sets how soft the one-pixel clip edge is.
  float bandPixelWidth = 3840;
  float bandPixelHeight = 720;
  // Vignette. Colour is the theme's `vignette` palette slot; the other two are
  // driven (`__vignetteOpacity` / `__vignetteSize`).
  Vec3 vignetteColor{0, 0, 0};
  float vignetteOpacity = 0.96f;
  float vignetteSize = 1.0f;
  // Stands in for the blob field, so the case can lock how the vignette and the
  // band edge act ON a colour without having to reproduce the field itself.
  Vec3 fieldColor{0.5f, 0.5f, 0.5f};
};

struct BackgroundBandOutput {
  // Band coverage: 1 inside, 0 outside, ramping over about a pixel at the clip.
  float inBand = 0;
  // Combined coverage of the four edge gradients, in band-local space.
  float vignetteShade = 0;
  // The finished premultiplied pixel. Alpha is always 1: the band is opaque and
  // so is the frame around it.
  Vec4 color{0, 0, 0, 1};
};

// One SwiftUI LinearGradient stop pair, as coverage. Runs from `opacity` at the
// edge to fully clear over `extent` of the band, scaled by `vignetteSize`.
inline float backgroundBandEdge(float t, float extent, float opacity, float size) {
  const float span = std::max(0.0001f, extent * size);
  return opacity * clampValue(1.0f - t / span, 0.0f, 1.0f);
}

inline BackgroundBandOutput backgroundBandReference(const BackgroundBandInput& in) {
  const float heightFraction = clampValue(in.heightFraction, 0.0f, 1.0f);
  const float widthFraction = clampValue(in.widthFraction, 0.0f, 1.0f);
  const float opacity = clampValue(in.vignetteOpacity, 0.0f, 1.0f);
  const float size = std::max(0.0f, in.vignetteSize);

  // The band is centred on both axes.
  const float bandTop = 0.5f - heightFraction * 0.5f;
  const float bandLeft = 0.5f - widthFraction * 0.5f;
  const float bandLocalY = (in.v - bandTop) / std::max(0.0001f, heightFraction);
  const float bandLocalX = (in.u - bandLeft) / std::max(0.0001f, widthFraction);

  // The band edge is a hard clip on tvOS (a SwiftUI frame). Softened by about
  // one pixel: at 4K a hard cut here shimmers under the encoder.
  const float softX = 1.0f / std::max(1.0f, in.bandPixelWidth);
  const float softY = 1.0f / std::max(1.0f, in.bandPixelHeight);
  auto smooth = [](float edge0, float edge1, float x) {
    const float t = clampValue((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  const float inBand = smooth(-softY, softY, bandLocalY) * smooth(-softY, softY, 1.0f - bandLocalY) *
                       smooth(-softX, softX, bandLocalX) * smooth(-softX, softX, 1.0f - bandLocalX);

  BackgroundBandOutput out;
  out.inBand = inBand;
  if (inBand <= 0.0f) {
    // Outside the band is the vignette colour at full coverage, not a hole. The
    // bars and the gradient inside the band are then the same surface, and the
    // composite has a defined backdrop everywhere.
    out.vignetteShade = 1.0f;
    out.color = {in.vignetteColor.x, in.vignetteColor.y, in.vignetteColor.z, 1.0f};
    return out;
  }

  const float bandU = clampValue(bandLocalX, 0.0f, 1.0f);
  const float bandV = clampValue(bandLocalY, 0.0f, 1.0f);

  // PhonoscopeEdgeVignette: four gradients in BAND-local space (the 0.28 stop is
  // 28% of the band, not of the screen). SwiftUI's ZStack composites them
  // source-over, so they combine as 1 - prod(1 - a), not as a sum. The authored
  // extents keep their ratio under `vignetteSize`: wider top-and-bottom than
  // side-to-side, as drawn.
  const float left = backgroundBandEdge(bandU, 0.18f, opacity, size);
  const float right = backgroundBandEdge(1.0f - bandU, 0.18f, opacity, size);
  const float top = backgroundBandEdge(bandV, 0.28f, opacity, size);
  const float bottom = backgroundBandEdge(1.0f - bandV, 0.28f, opacity, size);
  const float shade = 1.0f - (1.0f - left) * (1.0f - right) * (1.0f - top) * (1.0f - bottom);
  out.vignetteShade = shade;

  // Toward the vignette colour rather than a plain darken, so the gradient meets
  // the bars outside the band seamlessly. With the default black slot this is
  // exactly the original `color *= (1 - shade)`.
  auto channel = [&](float field, float vignette) {
    const float shaded = field + (vignette - field) * shade;
    // The one-pixel clip edge fades the shaded band into the surrounding frame.
    return vignette + (shaded - vignette) * inBand;
  };
  out.color = {channel(in.fieldColor.x, in.vignetteColor.x),
               channel(in.fieldColor.y, in.vignetteColor.y),
               channel(in.fieldColor.z, in.vignetteColor.z), 1.0f};
  return out;
}

}  // namespace nova
