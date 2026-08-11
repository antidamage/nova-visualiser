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

// Band geometry, in full-frame terms. Both fractions are driven parameters
// (`__bgHeight` / `__bgWidth`), so the band is no longer the fixed one-third
// letterbox it was authored as -- it can be anything from nothing to the whole
// frame, and a driver lane can sweep it on the beat.
uniform vec2 bandResolution;       // pixel size of the band itself, not the frame
uniform float bandFraction;        // band height as a fraction of the frame
uniform float bandWidthFraction;   // band width as a fraction of the frame

// The frame vignette. Colour comes from the theme's `vignette` palette slot, so
// the bars around the band and the gradient inside it are the same colour and
// read as one continuous frame rather than a band sitting on a black mat.
uniform vec3 vignetteColor;
uniform float vignetteOpacity;  // peak coverage of the edge gradients
uniform float vignetteSize;     // multiplies the authored gradient extents

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

// The colour theme's background image, when it names one. Two planes so a theme
// change can transition between them, on exactly the terms `centre_image.frag`
// already carries -- the arithmetic is stated once in
// core/centre_image_transition.h and both shaders mirror it.
//
// This sits INSIDE the backdrop pass rather than in one of its own, which is
// what puts it under the vignette: the band clip and the four edge gradients at
// the bottom of this shader run over whatever `field()` produced, and they do
// not care whether that was blobs or a photograph. `hasImage` 0 is the original
// shader exactly, so a theme with no background image pays nothing.
layout(binding = 0) uniform sampler2D imageTo;
layout(binding = 1) uniform sampler2D imageFrom;
uniform int hasImage;
uniform int hasImageFrom;
uniform vec2 imageHalfExtentTo;
uniform vec2 imageHalfExtentFrom;
uniform float imageProgress;
uniform int imageMode;
uniform float imageAxisRadians;
uniform int imageSegments;
uniform int imageReturnOrigin;
uniform float frameAspect;
// The colour an uncovered part of the frame falls back to, so a fitted image
// smaller than the band has something defined behind it rather than a hole.
uniform vec3 imageBackdrop;

const int MODE_CROSSFADE = 0;
const int MODE_FLIP = 1;
const int MODE_SLIDE = 2;
const float FLIP_EPSILON = 1e-4;
const float PI = 3.14159265358979323846;

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

// One plane of the background image, sampled inside its own rectangle and clear
// outside it.
//
// A copy of `plane()` in centre_image.frag, deliberately: the two shaders are
// separate compilation units with no include mechanism between them, and the
// arithmetic they must both match is stated once in
// core/centre_image_transition.h with a conformance case locking it. `uv` here
// is already in the top-left-origin frame space the rest of this shader works
// in, which is the one difference -- the centre pass flips at the end instead.
vec4 imagePlane(sampler2D image, vec2 halfExtent, bool incoming, vec2 frameUv) {
  if (halfExtent.x <= 0.0 || halfExtent.y <= 0.0) return vec4(0.0);

  vec2 centred = (frameUv - vec2(0.5)) * vec2(frameAspect, 1.0);
  if (imageMode != MODE_CROSSFADE) {
    vec2 along = vec2(cos(imageAxisRadians), sin(imageAxisRadians));
    vec2 across = vec2(-along.y, along.x);
    vec2 local = vec2(dot(centred, along), dot(centred, across));

    if (imageMode == MODE_FLIP) {
      float collapse = abs(cos(PI * clamp(imageProgress, 0.0, 1.0)));
      if (collapse < FLIP_EPSILON) return vec4(0.0);
      local.x /= collapse;
    } else {
      vec2 halfLocal = vec2(
        abs(halfExtent.x * frameAspect * along.x) + abs(halfExtent.y * along.y),
        abs(halfExtent.x * frameAspect * across.x) + abs(halfExtent.y * across.y));
      int index = 0;
      if (halfLocal.y > 0.0) {
        float position = (local.y / halfLocal.y) * 0.5 + 0.5;
        index = clamp(int(floor(position * float(imageSegments))), 0, imageSegments - 1);
      }
      float direction = (index % 2 == 0) ? 1.0 : -1.0;
      float frameSpan = 0.5 * (abs(along.x) * frameAspect + abs(along.y));
      float clearDistance = frameSpan + halfLocal.x;

      float clamped = clamp(imageProgress, 0.0, 1.0);
      float offset;
      if (!incoming) {
        offset = direction * clearDistance * (clamped * 2.0);
      } else {
        float arriving = clamped * 2.0 - 1.0;
        float travel = (imageReturnOrigin != 0) ? -direction : direction;
        offset = travel * clearDistance * (arriving - 1.0);
      }
      local.x -= offset;
    }

    centred = along * local.x + across * local.y;
  }

  vec2 texel = centred / vec2(frameAspect, 1.0) / (halfExtent * 2.0) + vec2(0.5);
  // Outside the fitted rectangle there is no image, which is what makes a fill
  // read as a crop rather than a squash -- and what carries a slid segment off
  // frame rather than wrapping it.
  if (any(lessThan(texel, vec2(0.0))) || any(greaterThan(texel, vec2(1.0)))) return vec4(0.0);
  // CPU rows are top-down and `frameUv` is already top-left origin, so unlike
  // the centre pass there is nothing to flip here.
  return texture(image, texel);
}

// The background image as one premultiplied colour, transitions resolved.
//
// Mirrors main() in centre_image.frag: a flip and a slide draw exactly ONE
// plane, swapping at the midpoint, and only a cross-fade draws both. Where
// nothing covers, the theme's backdrop colour shows through, so a fitted image
// smaller than the frame sits on the palette rather than on a hole.
vec3 imageField(vec2 frameUv) {
  vec4 accumulated = vec4(0.0);
  float weight = clamp(imageProgress, 0.0, 1.0);

  if (imageMode != MODE_CROSSFADE && hasImageFrom != 0) {
    accumulated = weight >= 0.5
      ? imagePlane(imageTo, imageHalfExtentTo, true, frameUv)
      : imagePlane(imageFrom, imageHalfExtentFrom, false, frameUv);
  } else {
    accumulated = imagePlane(imageTo, imageHalfExtentTo, true, frameUv) * weight;
    if (hasImageFrom != 0) {
      accumulated += imagePlane(imageFrom, imageHalfExtentFrom, false, frameUv) * (1.0 - weight);
    }
  }

  // Both planes are premultiplied at decode time, so this IS the source-over
  // term -- not a mix, which would darken the image by its own coverage a
  // second time.
  float coverage = clamp(accumulated.a, 0.0, 1.0);
  return accumulated.rgb + imageBackdrop * (1.0 - coverage);
}

// The frame the backdrop is drawn into: the four edge gradients, then the band's
// soft clip into the bars around it.
//
// Shared by both backdrop paths on purpose. The vignette is the LAST thing that
// happens to a backdrop whatever the backdrop is, which is precisely what "the
// background image sits behind the vignette" has to mean -- if the image had its
// own copy of this, the two could drift and the frame would look different
// depending on whether a theme happened to name a picture.
vec3 framedBackdrop(vec3 color, vec2 local, float inBand);

// One SwiftUI LinearGradient stop pair, as coverage. Each of the four vignette
// gradients runs from `vignetteOpacity` coverage at the edge to fully clear over
// `extent` of the band. The authored extents (0.18 across, 0.28 down) are
// multiplied by `vignetteSize`, which keeps their ratio -- the frame stays
// wider top-and-bottom than side-to-side as it grows, as it was drawn.
float edge(float t, float extent) {
  return vignetteOpacity * clamp(1.0 - t / max(0.0001, extent * vignetteSize), 0.0, 1.0);
}

vec3 framedBackdrop(vec3 color, vec2 local, float inBand) {
  // PhonoscopeEdgeVignette: four gradients in BAND-local space (the 0.28 stop is
  // 28% of the band, not of the screen). SwiftUI's ZStack composites them
  // source-over, so they combine as 1 - prod(1 - a), not as a sum.
  float left = edge(local.x, 0.18);
  float right = edge(1.0 - local.x, 0.18);
  float top = edge(local.y, 0.28);
  float bottom = edge(1.0 - local.y, 0.28);
  float shade = 1.0 - (1.0 - left) * (1.0 - right) * (1.0 - top) * (1.0 - bottom);
  // Toward the vignette colour rather than a plain darken, so the gradient meets
  // the bars outside the band seamlessly. With the default black slot this is
  // exactly the original `color *= (1.0 - shade)`.
  vec3 shaded = mix(color, vignetteColor, shade);
  // The band is opaque and so are the bars around it, so coverage is 1 across
  // the frame; `inBand` only survives as the one-pixel soft edge.
  return mix(vignetteColor, shaded, inBand);
}

void main() {
  // GL's framebuffer origin is bottom-left and SwiftUI's is top-left. Flip here
  // rather than in the composite, so the blob motion matches the local engine
  // frame for frame when the two are compared side by side.
  vec2 frameUv = vec2(uv.x, 1.0 - uv.y);

  // A background image REPLACES the field rather than layering over it, and it
  // brings its own geometry with it: the width, height and scale sized the band
  // when the band was the backdrop, and they size the IMAGE when the image is.
  // So there is no band to clip here -- the fitted rectangle is the geometry,
  // and the vignette closes over the whole frame around it.
  if (hasImage != 0) {
    outColor = vec4(framedBackdrop(imageField(frameUv), frameUv, 1.0), 1.0);
    return;
  }

  // Band-local coordinates. The band is centred on both axes.
  float bandTop = 0.5 - bandFraction * 0.5;
  float bandLeft = 0.5 - bandWidthFraction * 0.5;
  float bandLocalY = (frameUv.y - bandTop) / max(0.0001, bandFraction);
  float bandLocalX = (frameUv.x - bandLeft) / max(0.0001, bandWidthFraction);

  // The band edge is a hard clip on tvOS (a SwiftUI frame). Soften it by about
  // one pixel: at 4K a hard cut here shimmers under the encoder.
  vec2 edgeSoftness = 1.0 / max(vec2(1.0), bandResolution);
  float inBand = smoothstep(-edgeSoftness.y, edgeSoftness.y, bandLocalY) *
                 smoothstep(-edgeSoftness.y, edgeSoftness.y, 1.0 - bandLocalY) *
                 smoothstep(-edgeSoftness.x, edgeSoftness.x, bandLocalX) *
                 smoothstep(-edgeSoftness.x, edgeSoftness.x, 1.0 - bandLocalX);
  if (inBand <= 0.0) {
    // Outside the band is the vignette colour at full coverage, not a hole. The
    // bars and the gradient inside the band are then the same surface, and the
    // composite has a defined backdrop everywhere -- which is what lets the
    // scene blend modes mean something across the whole frame. Premultiplied,
    // so the colour is already the output.
    outColor = vec4(vignetteColor, 1.0);
    return;
  }

  // The field is computed in the band's own aspect, because that is the
  // drawable tvOS hands the shader.
  float aspect = max(0.0001, bandResolution.x / max(1.0, bandResolution.y));
  vec2 bandUv = vec2(clamp(bandLocalX, 0.0, 1.0), clamp(bandLocalY, 0.0, 1.0));
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

  outColor = vec4(framedBackdrop(color, vec2(bandUv.x, bandLocalY), inBand), 1.0);
}
