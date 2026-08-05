#version 460 core

// One axis of the glow overlay's separable Gaussian, run on a
// quarter-resolution copy of the finished frame.
//
// The taps sit at i * (sigma/3) texels for i in -6..6, so they always cover
// +/-2 sigma no matter how wide the blur is. Because the stride is
// proportional to sigma, the weights are constant -- exp(-i^2/18) -- and only
// the offsets scale. That is what keeps this cheap enough to sit on the end of
// every frame with a parameter driver moving `sigma` continuously.
//
// Mirrors `phonoscope_glow_blur` in PhonoscopeShader.metal and the tap contract
// in src/core/glow_overlay_reference.h.

layout(binding = 0) uniform sampler2D source;
// Texel size on the blur axis only; the other component is zero.
uniform vec2 axisTexel;
// Gaussian sigma, in texels of this target.
uniform float sigma;
in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 result = texture(source, uv) * 1.0;
  float weightSum = 1.0;
  // Sigma 0 collapses every tap onto the centre, so skip straight to the copy
  // rather than summing thirteen identical samples.
  if (sigma > 0.0) {
    float stride = sigma / 3.0;
    for (int tap = 1; tap <= 6; ++tap) {
      float weight = exp(-float(tap * tap) / 18.0);
      vec2 offset = axisTexel * (float(tap) * stride);
      result += (texture(source, uv + offset) + texture(source, uv - offset)) * weight;
      weightSum += weight * 2.0;
    }
  }
  outColor = result / weightSum;
}
