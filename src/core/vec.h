// Small float vector types matching the SIMD3/SIMD4<Float> semantics the tvOS
// engine uses. Kept deliberately plain so conformance results are bit-stable
// across compilers rather than depending on an auto-vectoriser.
#pragma once

#include <algorithm>
#include <cmath>

namespace nova {

struct Vec3 {
  float x = 0, y = 0, z = 0;

  Vec3() = default;
  Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

  float& operator[](int axis) { return axis == 0 ? x : (axis == 1 ? y : z); }
  float operator[](int axis) const { return axis == 0 ? x : (axis == 1 ? y : z); }

  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;

  Vec4() = default;
  Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
  explicit Vec4(float v) : x(v), y(v), z(v), w(v) {}
};

inline float lengthSquared(const Vec3& v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
inline float length(const Vec3& v) { return std::sqrt(lengthSquared(v)); }
inline float distance(const Vec3& a, const Vec3& b) { return length(a - b); }

inline Vec3 normalize(const Vec3& v) {
  float len = length(v);
  return len > 0 ? v / len : Vec3{};
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline Vec4 mix(const Vec4& a, const Vec4& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
          a.w + (b.w - a.w) * t};
}

inline Vec3 mix(const Vec3& a, const Vec3& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

template <typename T>
inline T clampValue(T value, T low, T high) {
  return std::min(std::max(value, low), high);
}

}  // namespace nova
