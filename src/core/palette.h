// Palette slots and the chase interpolation shared with tvOS.
//
// Port of `PhonoscopePalette` plus `phonoscopeChaseAmount` /
// `phonoscopeSettingInterpolationAction`. The interpolation rules are asserted
// by `ParitySelfTests.testPhonoscopeSettingInterpolation` on the tvOS side and
// by the conformance corpus here; keep both in step.
#pragma once

#include <map>
#include <string>

#include "core/vec.h"

namespace nova {

class Palette {
 public:
  Palette() = default;
  explicit Palette(std::map<std::string, Vec4> colors) : colors_(std::move(colors)) {}

  static Palette defaults();

  Vec4 color(const std::string& id) const;
  Vec4 color(const std::string& id, const Vec4& fallback) const;

  Vec4 accent() const { return color("primary", Vec4{0.45f, 0.45f, 0.45f, 1.0f}); }
  Vec4 highlight() const { return color("secondary", Vec4{0.85f, 0.85f, 0.85f, 1.0f}); }
  Vec4 background() const {
    return color("backgroundPrimary", color("background", Vec4{0, 0, 0, 1}));
  }

  Palette approached(const Palette& target, double amount) const;

  const std::map<std::string, Vec4>& colors() const { return colors_; }
  void set(const std::string& id, const Vec4& value) { colors_[id] = value; }

 private:
  std::map<std::string, Vec4> colors_;
};

// Frame-rate independent exponential chase. `settlingDuration` is the time
// constant: after that many seconds roughly 63% of the gap has closed.
double chaseAmount(double delta, double settlingDuration);

enum class SettingInterpolationAction { Hold, Transition, ApplyImmediately };

SettingInterpolationAction settingInterpolationAction(bool targetChanged,
                                                      bool wasDriverInterpolated,
                                                      bool isDriverInterpolated);

}  // namespace nova
