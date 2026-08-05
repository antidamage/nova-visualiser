#version 460 core

// Port of `phonoscope_fragment`. Every primitive branch is reproduced exactly,
// including the premultiplied output the source-one blend relies on.
//
// One deliberate change: the point and ring branches now anti-alias their own
// edges with fwidth instead of relying on 4x MSAA. At 4K an MSAA colour target
// costs ~265 MB of VRAM plus a resolve every frame, and this GPU is shared with
// the voice stack. Analytic coverage is both cheaper and smoother on the round
// shapes that dominate every module.

in VertexData {
  vec2 local;
  vec4 color;
  vec4 colorEnd;
  vec4 glowColor;
  vec4 glowColorEnd;
  float glow;
  flat float primitive;
  flat float material;
  flat float effectScale;
} fragmentIn;

layout(location = 0) out vec4 outColor;

void main() {
  vec2 local = fragmentIn.local;
  float radius = length(local);
  float gradientProgress = clamp(radius, 0.0, 1.0);
  float core;
  float halo;
  float haloRadius = radius / max(1.0, fragmentIn.effectScale);

  // One pixel expressed in local quad units, for analytic edge coverage.
  float pixel = max(fwidth(radius), 0.0001);

  if (fragmentIn.primitive < 0.5) {
    // point
    if (radius > fragmentIn.effectScale + pixel) discard;
    core = smoothstep(1.0, 0.08, radius) * (1.0 - smoothstep(1.0 - pixel, 1.0 + pixel, radius));
    halo = exp(-haloRadius * haloRadius * 3.2) * fragmentIn.glow;
  } else if (fragmentIn.primitive < 1.5) {
    // ring
    if (radius > fragmentIn.effectScale + pixel) discard;
    core = smoothstep(0.16, 0.02, abs(radius - 0.70));
    halo = exp(-haloRadius * haloRadius * 3.2) * fragmentIn.glow;
  } else if (fragmentIn.primitive < 2.5) {
    // square / sprite
    core = smoothstep(1.0, 0.78, max(abs(local.x), abs(local.y)));
    halo = exp(-haloRadius * haloRadius * 3.2) * fragmentIn.glow;
  } else if (fragmentIn.primitive < 3.5) {
    // triangle
    float edge = 1.0 - abs(local.x);
    bool insideCore = local.y >= -1.0 && local.y <= edge * 2.0 - 1.0;
    core = insideCore
        ? smoothstep(0.08, 0.22, min(local.y + 1.0, edge * 2.0 - 1.0 - local.y))
        : 0.0;
    halo = exp(-haloRadius * haloRadius * 3.2) * fragmentIn.glow;
  } else if (fragmentIn.primitive < 4.5) {
    // wireframe box
    float edge = max(abs(local.x), abs(local.y));
    core = smoothstep(0.15, 0.015, abs(edge - 0.82));
    halo = exp(-haloRadius * haloRadius * 3.2) * fragmentIn.glow;
  } else if (fragmentIn.primitive < 5.5) {
    // trail: starts at the dot (quad head at progress 1) and ends at the tail,
    // so Primary remains the start colour.
    float progress = clamp((local.x + 1.0) * 0.5, 0.0, 1.0);
    gradientProgress = 1.0 - progress;
    float brightness = pow(progress, 1.45);
    core = smoothstep(1.0, 0.08, abs(local.y)) * brightness;
    halo = exp(-local.y * local.y * 3.2) * fragmentIn.glow * brightness;
  } else {
    // grid wire, emitted source-to-destination
    gradientProgress = clamp((local.x + 1.0) * 0.5, 0.0, 1.0);
    float signedEdgeDistance = 1.0 - abs(local.y);
    float edgePixelWidth = max(fwidth(local.y), 0.0001);
    core = smoothstep(-edgePixelWidth * 0.5, edgePixelWidth * 0.5, signedEdgeDistance);
    halo = 0.0;
  }

  float lighting = 1.0;
  if (fragmentIn.material > 0.5 && fragmentIn.material < 1.5 && radius <= 1.0) {
    float z = sqrt(max(0.0, 1.0 - radius * radius));
    lighting = 0.28 + 0.72 * max(0.0, dot(normalize(vec3(local, z)),
                                          normalize(vec3(-0.35, 0.45, 1.0))));
  }

  vec4 coreColor = mix(fragmentIn.color, fragmentIn.colorEnd, gradientProgress);
  vec4 glowColor = mix(fragmentIn.glowColor, fragmentIn.glowColorEnd, gradientProgress);
  float coreAlpha = coreColor.a * core;
  float glowAlpha = glowColor.a * halo * 0.38;
  float alpha = clamp(coreAlpha + glowAlpha, 0.0, 1.0);
  vec3 rgb = coreColor.rgb * coreAlpha * lighting + glowColor.rgb * glowAlpha;
  outColor = vec4(rgb, alpha);
}
