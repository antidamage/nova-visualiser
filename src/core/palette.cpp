#include "core/palette.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace nova {

Palette Palette::defaults() {
  return Palette({
      {"primary", Vec4{0.45f, 0.45f, 0.45f, 1.0f}},
      {"secondary", Vec4{0.85f, 0.85f, 0.85f, 1.0f}},
      {"tertiary", Vec4{0.65f, 0.65f, 0.65f, 1.0f}},
      {"background", Vec4{0, 0, 0, 1}},
      {"backgroundPrimary", Vec4{0, 0, 0, 1}},
      {"backgroundSecondary", Vec4{0.05f, 0.07f, 0.11f, 1.0f}},
  });
}

Vec4 Palette::color(const std::string& id, const Vec4& fallback) const {
  auto it = colors_.find(id);
  if (it != colors_.end()) return it->second;
  if (id == "accent") {
    auto primary = colors_.find("primary");
    if (primary != colors_.end()) return primary->second;
  }
  if (id == "highlight") {
    auto secondary = colors_.find("secondary");
    if (secondary != colors_.end()) return secondary->second;
  }
  return fallback;
}

Vec4 Palette::color(const std::string& id) const {
  auto it = colors_.find(id);
  if (it != colors_.end()) return it->second;
  if (id == "accent") {
    auto primary = colors_.find("primary");
    if (primary != colors_.end()) return primary->second;
  }
  if (id == "highlight") {
    auto secondary = colors_.find("secondary");
    if (secondary != colors_.end()) return secondary->second;
  }
  return accent();
}

Palette Palette::approached(const Palette& target, double rawAmount) const {
  const float amount = static_cast<float>(std::min(1.0, std::max(0.0, rawAmount)));
  std::set<std::string> keys;
  for (const auto& [key, value] : colors_) keys.insert(key);
  for (const auto& [key, value] : target.colors_) keys.insert(key);

  std::map<std::string, Vec4> result;
  for (const std::string& key : keys) {
    auto currentIt = colors_.find(key);
    auto targetIt = target.colors_.find(key);
    // A slot that only exists on one side fades in/out through alpha rather
    // than popping: the missing side is treated as the same hue at alpha 0.
    Vec4 current;
    if (currentIt != colors_.end()) {
      current = currentIt->second;
    } else if (targetIt != target.colors_.end()) {
      current = Vec4{targetIt->second.x, targetIt->second.y, targetIt->second.z, 0};
    }
    Vec4 destination = targetIt != target.colors_.end()
                           ? targetIt->second
                           : Vec4{current.x, current.y, current.z, 0};
    result[key] = mix(current, destination, amount);
  }
  return Palette(std::move(result));
}

double chaseAmount(double delta, double settlingDuration) {
  if (settlingDuration <= 0) return 1;
  // Three time constants settle to roughly 95%. Unlike a fixed endpoint tween,
  // this response can be retargeted every frame without a jump.
  return 1 - std::exp(-3 * std::max(0.0, delta) / settlingDuration);
}

SettingInterpolationAction settingInterpolationAction(bool targetChanged,
                                                      bool wasDriverInterpolated,
                                                      bool isDriverInterpolated) {
  if (isDriverInterpolated) return SettingInterpolationAction::ApplyImmediately;
  if (targetChanged || wasDriverInterpolated) return SettingInterpolationAction::Transition;
  return SettingInterpolationAction::Hold;
}

}  // namespace nova
