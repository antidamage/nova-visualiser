#version 460 core

// Instanced particle expansion. Port of `phonoscope_vertex` in
// PhonoscopeShader.metal, with one addition: the vertex shader interpolates
// between the two most recent simulation states instead of drawing whichever
// tick happened to land last. Simulation runs at a fixed 120 Hz substep and the
// renderer at 60 Hz, so without this the motion beats against the sim rate.

struct Particle {
  vec4 positionSize;
  vec4 color;
  vec4 colorEnd;
  vec4 glowColor;
  vec4 glowColorEnd;
  vec4 meta;   // x glow, y primitive, z material
  vec4 trail;  // xyz direction, w length
};

layout(std430, binding = 0) readonly buffer CurrentParticles { Particle current[]; };
layout(std430, binding = 1) readonly buffer PreviousParticles { Particle previous[]; };

layout(std140, binding = 2) uniform Uniforms {
  vec4 viewport;   // x width, y height, z renderScale
  vec4 boundsMin;
  vec4 boundsMax;
  vec4 signalData;  // x time, y beatPulse, z is3D, w quality
  vec4 blend;       // x interpolation alpha, y 1 when interpolation is valid
};

out VertexData {
  vec2 local;
  vec4 color;
  vec4 colorEnd;
  vec4 glowColor;
  vec4 glowColorEnd;
  float glow;
  flat float primitive;
  flat float material;
  flat float effectScale;
} vertexOut;

const vec2 corners[6] = vec2[6](
  vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
  vec2(-1, 1), vec2(1, -1), vec2(1, 1)
);

void main() {
  Particle particle = current[gl_InstanceID];

  // Only positions, radius and the trail vector are interpolated. Colours are
  // already chased on the simulation thread, and interpolating the primitive or
  // material codes would produce nonsense in the fragment branch.
  if (blend.y > 0.5) {
    Particle old = previous[gl_InstanceID];
    float alpha = blend.x;
    particle.positionSize = mix(old.positionSize, particle.positionSize, alpha);
    particle.trail.xyz = mix(old.trail.xyz, particle.trail.xyz, alpha);
  }

  vec3 p = particle.positionSize.xyz;
  float is3D = signalData.z;
  if (is3D > 0.5) {
    float angle = signalData.x * 0.055;
    float c = cos(angle);
    float s = sin(angle);
    p.xz = vec2(p.x * c - p.z * s, p.x * s + p.z * c);
    float depth = max(1.0, p.z + 3.2);
    p.xy /= depth * 0.62;
  } else {
    vec2 center = (boundsMin.xy + boundsMax.xy) * 0.5;
    vec2 extent = max(vec2(0.0001), (boundsMax.xy - boundsMin.xy) * 0.5);
    p.xy = (p.xy - center) / extent;
  }
  float aspect = max(0.01, viewport.x / max(1.0, viewport.y));

  vec2 corner = corners[gl_VertexID];
  vec2 clipPosition;
  vec2 local = corner;
  // The original Metal surface was authored against a 1080-line drawable.
  // Grow resolution-sensitive effects at 4K while keeping core dot and wire
  // geometry in exactly the same clip-space footprint.
  float effectScale = max(1.0, viewport.w);

  if (particle.meta.y > 5.5 && particle.trail.w > 0.0) {
    // Grid wire: a quad spanning source to destination.
    vec2 delta = particle.trail.xy;
    if (is3D <= 0.5) {
      vec2 extent = max(vec2(0.0001), (boundsMax.xy - boundsMin.xy) * 0.5);
      delta /= extent;
    }
    vec2 screenDelta = vec2(delta.x * aspect, delta.y);
    float deltaLength = length(screenDelta);
    vec2 screenDirection = deltaLength > 0.00001 ? screenDelta / deltaLength : vec2(1.0, 0.0);
    vec2 clipNormal = vec2(-screenDirection.y / aspect, screenDirection.x);
    float progress = (corner.x + 1.0) * 0.5;
    vec2 lineCenter = p.xy - delta * (1.0 - progress);
    clipPosition = lineCenter + clipNormal * corner.y * particle.positionSize.w;
  } else if (particle.meta.y > 4.5 && particle.trail.w > 0.0) {
    // Trail: a tapered wake behind the dot.
    vec2 direction = particle.trail.xy;
    if (is3D <= 0.5) {
      vec2 extent = max(vec2(0.0001), (boundsMax.xy - boundsMin.xy) * 0.5);
      direction /= extent;
    }
    vec2 screenDirection = vec2(direction.x * aspect, direction.y);
    float directionLength = length(screenDirection);
    screenDirection = directionLength > 0.00001 ? screenDirection / directionLength : vec2(1.0, 0.0);
    vec2 clipDirection = vec2(screenDirection.x / aspect, screenDirection.y);
    vec2 clipNormal = vec2(-screenDirection.y / aspect, screenDirection.x);
    float progress = (corner.x + 1.0) * 0.5;
    float sourceRadius = particle.positionSize.w;
    vec2 trailHead = p.xy - clipDirection * sourceRadius * 0.9;
    vec2 trailTail = trailHead - clipDirection * sourceRadius * particle.trail.w * effectScale;
    vec2 trailCenter = mix(trailTail, trailHead, progress);
    float halfWidth = sourceRadius * progress * effectScale;
    clipPosition = trailCenter + clipNormal * corner.y * halfWidth;
  } else {
    local = corner * effectScale;
    vec2 offset = corner * particle.positionSize.w * effectScale;
    offset.x /= aspect;
    clipPosition = p.xy + offset;
  }

  gl_Position = vec4(clipPosition, 0.0, 1.0);
  vertexOut.local = local;
  vertexOut.color = particle.color;
  vertexOut.colorEnd = particle.colorEnd;
  vertexOut.glowColor = particle.glowColor;
  vertexOut.glowColorEnd = particle.glowColorEnd;
  vertexOut.glow = particle.meta.x;
  vertexOut.primitive = particle.meta.y;
  vertexOut.material = particle.meta.z;
  vertexOut.effectScale = effectScale;
}
