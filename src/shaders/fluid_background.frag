#version 460 core

// Animated fluid background band.
//
// `particle-ripples` declares `fluid_speed` with
// `affects: [renderer.fluidBackground.speed]`, and the tvOS engine has always
// honoured it: `FluidBackgroundView` draws this blob field in a centred band one
// third of the screen high, with `PhonoscopeEdgeVignette` over it, and the
// particle pass composites on top with a fully transparent background so the
// band shows through. This renderer had no equivalent pass at all -- it put the
// particles over a flat palette colour -- so the streamed image was missing the
// entire backdrop while the local engine still drew it. That is why the Apple TV
// showed one correct frame and then fell back to bare geometry.
//
// Ported from `nova-ha-dashboard/app/components/FluidBackground.tsx`, which is
// already GLSL, plus the `blobScale`/`blobSoftness` terms that exist only in
// `FluidBackgroundShader.metal` (tvOS passes 4.0 and 0.45 for this surface).
//
// The mosaic displacement texture is deliberately absent: tvOS passes
// `allowsDisplacementTexture: false` here, so `hasMosaicTexture = 0` IS the
// parity case and adding an image loader to this service would buy nothing.

in vec2 uv;
layout(location = 0) out vec4 outColor;

// Band geometry, in full-frame terms.
uniform vec2 bandResolution;  // pixel size of the band itself, not the frame
uniform float bandFraction;   // band height as a fraction of the frame

uniform float time;
uniform vec3 background;
uniform vec3 accent;
uniform vec3 highlight;
uniform float peakIntensity;
uniform float falloffPower;
uniform float warpAmplitude;
uniform float hueSpread;
uniform float apexGlow;
uniform float blobScale;
uniform float blobSoftness;

vec3 hsvToRgb(vec3 c) {
  vec4 k = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
  vec3 p = abs(fract(c.xxx + k.xyz) * 6.0 - k.www);
  return c.z * mix(k.xxx, clamp(p - k.xxx, 0.0, 1.0), c.y);
}

vec3 rgbToHsv(vec3 c) {
  vec4 k = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  vec4 p = mix(vec4(c.bg, k.wz), vec4(c.gb, k.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  float e = 1.0e-10;
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hueShift(vec3 color, float amount) {
  vec3 hsv = rgbToHsv(max(color, vec3(0.0)));
  hsv.x = fract(hsv.x + amount);
  return hsvToRgb(hsv);
}

float peakField(vec2 p, vec2 center, float radius, float seed, float warp, float falloff) {
  vec2 warped = p;
  warped.x += sin(p.y * 4.4 + time * 0.18 + seed) * 0.056 * warp;
  warped.y += cos(p.x * 3.8 - time * 0.15 + seed * 1.7) * 0.048 * warp;

  float dist = length(warped - center);
  float peak = smoothstep(radius, 0.0, dist);
  float ridge = 0.5 + 0.5 * sin((p.x * 7.0 + p.y * 5.0) + time * 0.22 + seed);
  return pow(peak, max(0.4, falloff)) * (0.70 + ridge * 0.45);
}

// One SwiftUI LinearGradient stop pair, as coverage. Each of the four vignette
// gradients runs from black at 0.96 alpha to fully clear over a fixed fraction.
float edge(float t, float extent) { return 0.96 * clamp(1.0 - t / extent, 0.0, 1.0); }

void main() {
  // GL's framebuffer origin is bottom-left and SwiftUI's is top-left. Flip here
  // rather than in the composite, so the blob motion matches the local engine
  // frame for frame when the two are compared side by side.
  vec2 frameUv = vec2(uv.x, 1.0 - uv.y);

  // Band-local coordinates. The band is centred vertically and full width.
  float bandTop = 0.5 - bandFraction * 0.5;
  float bandLocalY = (frameUv.y - bandTop) / max(0.0001, bandFraction);

  // The band edge is a hard clip on tvOS (a SwiftUI frame). Soften it by about
  // one pixel: at 4K a hard cut here shimmers under the encoder.
  float edgeSoftness = 1.0 / max(1.0, bandResolution.y);
  float inBand = smoothstep(-edgeSoftness, edgeSoftness, bandLocalY) *
                 smoothstep(-edgeSoftness, edgeSoftness, 1.0 - bandLocalY);
  if (inBand <= 0.0) {
    outColor = vec4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  // The field is computed in the band's own aspect, because that is the
  // drawable tvOS hands the shader.
  float aspect = max(0.0001, bandResolution.x / max(1.0, bandResolution.y));
  vec2 bandUv = vec2(frameUv.x, clamp(bandLocalY, 0.0, 1.0));
  vec2 p = (bandUv - 0.5) * vec2(aspect, 1.0);

  float intensity = clamp(peakIntensity, 0.4, 2.6);
  // blobSoftness scales the falloff exponent, exactly as the Metal version does.
  float falloff = clamp(falloffPower, 0.8, 3.2) * clamp(blobSoftness, 0.25, 1.5);
  float warp = clamp(warpAmplitude, 0.4, 2.2);
  float spread = clamp(hueSpread, 0.0, 1.0);
  float apex = clamp(apexGlow, 0.0, 2.4);
  // Blob radii are normalized band-space geometry. Scaling them with output
  // resolution made the authored radius 8 at 4K, causing all four fields to
  // overlap the whole band and wash it into one pale slab. The Metal fallback
  // uses blobScale directly, independent of drawable density.
  float scale = clamp(blobScale, 0.5, 4.0);

  vec3 color = background;
  const float seeds[4] = float[4](0.0, 1.8, 3.4, 5.2);
  const float radii[4] = float[4](0.48, 0.43, 0.46, 0.38);

  for (int i = 0; i < 4; ++i) {
    float seed = seeds[i];
    vec2 center = vec2(
      sin(time * 0.055 + seed) * 0.50 + sin(time * 0.019 + seed * 2.1) * 0.10,
      cos(time * 0.047 + seed * 1.3) * 0.31 + sin(time * 0.027 + seed) * 0.11
    );
    center.x *= aspect;

    float peak = peakField(p, center, radii[i] * scale, seed, warp, falloff);
    float apexMask = smoothstep(0.62, 1.0, peak);
    float pulse = 0.5 + 0.5 * sin(time * 0.12 + seed);
    vec3 tint = mix(accent, highlight, pulse);
    float hueOffset =
        (sin(seed * 12.9898 + time * 0.018) * 0.5 + sin(seed * 4.531) * 0.5) * 0.11 * spread;
    tint = hueShift(tint, hueOffset);
    color += tint * peak * (0.22 + pulse * 0.16) * intensity;
    color += tint * apexMask * 0.18 * apex;
  }

  float grain = fract(sin(dot(bandUv * bandResolution + time,
                             vec2(12.9898, 78.233))) * 43758.5453);
  color += (grain - 0.5) * 0.006;

  float vignette = smoothstep(0.34, 1.16, length(p));
  color = mix(color, background * 0.76, vignette * 0.42);
  vec3 cap = max(accent, highlight) * (0.64 + intensity * 0.12) + background * 1.05;
  color = min(color, cap);
  color = clamp(color, 0.0, 1.0);

  // PhonoscopeEdgeVignette: four black gradients in BAND-local space (the 0.28
  // stop is 28% of the band, not of the screen). SwiftUI's ZStack composites
  // them source-over, so they combine as 1 - prod(1 - a), not as a sum.
  float left = edge(bandUv.x, 0.18);
  float right = edge(1.0 - bandUv.x, 0.18);
  float top = edge(bandLocalY, 0.28);
  float bottom = edge(1.0 - bandLocalY, 0.28);
  float shade = 1.0 - (1.0 - left) * (1.0 - right) * (1.0 - top) * (1.0 - bottom);
  color *= (1.0 - shade);

  // Alpha is coverage for the composite: the band hides the flat background
  // behind it, everything outside the band does not.
  outColor = vec4(color * inBand, inBand);
}
