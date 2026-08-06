#include "core/parameter_drivers.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nova {
namespace {

double finite(double value, double fallback) {
  return std::isfinite(value) ? value : fallback;
}

double clamp01(double value) {
  return std::max(0.0, std::min(1.0, value));
}

struct EnvelopeTimes {
  double attackSeconds;
  double holdSeconds;
  double releaseSeconds;
};

EnvelopeTimes envelopeTimes(const EffectBinding& binding) {
  return {
      std::max(0.0, binding.hasAttack ? finite(binding.attackSeconds, 0.05) : 0.05),
      std::max(0.0, binding.hasHold ? finite(binding.holdSeconds, 0) : 0.0),
      std::max(0.0, binding.hasRelease ? finite(binding.releaseSeconds, 0.6) : 0.6),
  };
}

double clampToDeclaration(const EffectDeclaration& declaration, double value) {
  const double low = std::min(declaration.min, declaration.max);
  const double high = std::max(declaration.min, declaration.max);
  const double bounded = std::max(low, std::min(high, finite(value, declaration.defaultValue)));
  if (!(declaration.step > 0)) return bounded;
  const double stepped = low + std::round((bounded - low) / declaration.step) * declaration.step;
  return std::max(low, std::min(high, stepped));
}

// The raw 0..1 level a continuous driver carries this tick.
double levelSignal(const Driver& driver, const SignalFrame& frame) {
  if (driver.type == "energy") return clamp01(finite(frame.energy, 0));
  std::size_t first = 0;
  std::size_t last = frame.spectrum.size();
  if (driver.type == "bass") {
    last = std::min<std::size_t>(last, 8);
  } else if (driver.type == "mid") {
    first = std::min<std::size_t>(8, last);
    last = std::min<std::size_t>(last, 20);
  } else if (driver.type == "treble") {
    first = std::min<std::size_t>(20, last);
  }
  double peak = 0;
  for (std::size_t index = first; index < last; ++index) {
    peak = std::max(peak, static_cast<double>(frame.spectrum[index]));
  }
  return clamp01(peak);
}

// The event key a pulse driver is currently on, or an empty string when this
// tick is not one it fires on. `song` counts its own events because a track
// seed is an identity, not an ordinal.
std::string pulseEventKey(const Driver& driver, const SignalFrame& frame, DriverSlotState& state) {
  if (driver.type == "song") {
    if (!state.seenTrack) {
      state.seenTrack = true;
      state.lastTrackSeed = frame.trackSeed;
    } else if (frame.trackSeed != state.lastTrackSeed) {
      state.lastTrackSeed = frame.trackSeed;
      state.eventCount += 1;
    }
    return driverFiresOn(state.eventCount, driver.every, driver.offset)
               ? "s:" + std::to_string(state.eventCount)
               : std::string();
  }
  if (driver.type == "timer") {
    const double interval = std::max(0.25, finite(driver.intervalSeconds, 4));
    const long long index = static_cast<long long>(std::floor(finite(frame.time, 0) / interval));
    return driverFiresOn(static_cast<int>(index), driver.every, driver.offset)
               ? "t:" + std::to_string(index)
               : std::string();
  }
  if (driver.type == "downbeat") {
    return driverFiresOn(frame.barIndex, driver.every, driver.offset)
               ? "d:" + std::to_string(frame.barIndex)
               : std::string();
  }
  return driverFiresOn(frame.beatIndex, driver.every, driver.offset)
             ? "b:" + std::to_string(frame.beatIndex)
             : std::string();
}

// A pulse driver runs a triggered attack/hold/release envelope: the event
// starts the attack, and the shape from there is entirely the authored
// envelope. That is what lets "every 4th downbeat" mean something a decaying
// beat pulse could not express, and why timer and song can be drivers at all --
// they supply an instant, not a shape.
double advancePulseEnvelope(DriverSlotState& state, const EnvelopeTimes& times, double delta,
                            const std::string& eventKey) {
  const bool triggered = !eventKey.empty() && eventKey != state.eventKey;
  if (triggered) {
    state.eventKey = eventKey;
    state.phase = DriverSlotState::Phase::Attack;
    state.holdRemaining = std::max(0.0, times.holdSeconds);
  }
  // Phases are walked within the tick, each consuming the time it needs, so a
  // zero-length attack or hold does not cost a frame.
  double remaining = delta;
  for (int step = 0; step < 4 && remaining > 0 && state.phase != DriverSlotState::Phase::Idle;
       ++step) {
    if (state.phase == DriverSlotState::Phase::Attack) {
      if (times.attackSeconds <= 0) {
        state.level = 1;
        state.phase = DriverSlotState::Phase::Hold;
        continue;
      }
      const double needed = (1 - state.level) * times.attackSeconds;
      if (remaining >= needed) {
        state.level = 1;
        remaining -= needed;
        state.phase = DriverSlotState::Phase::Hold;
      } else {
        state.level += remaining / times.attackSeconds;
        remaining = 0;
      }
    } else if (state.phase == DriverSlotState::Phase::Hold) {
      // An envelope never starts releasing on the tick it was triggered, so
      // even an all-zero envelope reads as one frame at full strength rather
      // than vanishing between samples.
      if (triggered) break;
      if (state.holdRemaining <= 0) {
        state.phase = DriverSlotState::Phase::Release;
        continue;
      }
      const double used = std::min(remaining, state.holdRemaining);
      state.holdRemaining -= used;
      remaining -= used;
      if (state.holdRemaining <= 0) state.phase = DriverSlotState::Phase::Release;
    } else {
      if (times.releaseSeconds <= 0) {
        state.level = 0;
        state.phase = DriverSlotState::Phase::Idle;
        continue;
      }
      const double needed = state.level * times.releaseSeconds;
      if (remaining >= needed) {
        state.level = 0;
        remaining = 0;
        state.phase = DriverSlotState::Phase::Idle;
      } else {
        state.level -= remaining / times.releaseSeconds;
        remaining = 0;
      }
    }
  }
  if (state.phase == DriverSlotState::Phase::Idle) state.level = 0;
  return clamp01(state.level);
}

// A continuous driver follows its level instead of being triggered by it, so
// the envelope acts as a rate limit. This is the behaviour the pre-lane
// parameter sources had, preserved exactly.
double advanceFollower(DriverSlotState& state, const EnvelopeTimes& times, double delta,
                       double signal) {
  state.target = signal;
  const bool rising = state.target >= state.current;
  if (rising) {
    state.holdRemaining = std::max(0.0, times.holdSeconds);
  } else if (state.holdRemaining > 0) {
    state.holdRemaining -= delta;
    return state.current;
  }
  const double duration = rising ? times.attackSeconds : times.releaseSeconds;
  if (duration <= 0) {
    state.current = state.target;
  } else {
    const double step = delta / duration;
    state.current = rising ? std::min(state.target, state.current + step)
                           : std::max(state.target, state.current - step);
  }
  return clamp01(state.current);
}

// `random` samples on its cadence and glides over `transitionSeconds`, ignoring
// the binding envelope -- the cadence and the glide are the shape. The sample is
// seeded from the slot key and the event so every engine picks the same value.
double advanceRandom(DriverSlotState& state, const Driver& driver, const SignalFrame& frame,
                     double delta, const std::string& key) {
  Driver cadence = driver;
  cadence.type = (driver.cadence == "beat" || driver.cadence == "downbeat" ||
                  driver.cadence == "timer" || driver.cadence == "song")
                     ? driver.cadence
                     : std::string("beat");
  const std::string eventKey = pulseEventKey(cadence, frame, state);
  if (!eventKey.empty() && eventKey != state.eventKey) {
    state.eventKey = eventKey;
    const uint64_t seed = stableSeed(key + ":" + eventKey);
    state.target = static_cast<double>(seed % 1000003) / 1000002.0;
  }
  const double duration = std::max(0.0, finite(driver.transitionSeconds, 0.5));
  const double amount = duration == 0 ? 1.0 : std::min(1.0, delta / duration);
  state.current += (state.target - state.current) * amount;
  return clamp01(state.current);
}

double driverSignal(const Driver& driver, const EffectBinding& binding, const SignalFrame& frame,
                    DriverStates& states, const std::string& key) {
  DriverSlotState& state = states[key];
  const double delta = std::max(1.0 / 120.0, std::min(0.25, finite(frame.delta, 1.0 / 60.0)));
  if (driver.type == "random") return advanceRandom(state, driver, frame, delta, key);
  if (isPulseDriver(driver)) {
    return advancePulseEnvelope(state, envelopeTimes(binding), delta,
                                pulseEventKey(driver, frame, state));
  }
  return advanceFollower(state, envelopeTimes(binding), delta, levelSignal(driver, frame));
}

struct Contribution {
  double amount;
  double period;
  double restingValue;
};

}  // namespace

bool isPulseDriver(const Driver& driver) {
  return driver.type == "beat" || driver.type == "downbeat" || driver.type == "timer" ||
         driver.type == "song";
}

bool driverFiresOn(int index, int every, int offset) {
  const int cycle = std::max(1, every);
  if (cycle == 1) return true;
  const int phase = std::max(0, std::min(cycle - 1, offset));
  return (((index - phase) % cycle) + cycle) % cycle == 0;
}

double driverPeriodSeconds(const Driver& driver, const SignalFrame& frame) {
  const int every = std::max(1, driver.every);
  const double secondsPerBeat = std::max(1e-6, 60.0 / std::max(1.0, finite(frame.bpm, 72)));
  const double beatsPerBar = std::max(1, frame.timeSignature);
  // A song is the rarest thing that can happen, and its length is unknown ahead
  // of time, so it always outranks a counted pulse.
  if (driver.type == "song") return std::numeric_limits<double>::infinity();
  if (driver.type == "timer") {
    return every * std::max(0.25, finite(driver.intervalSeconds, 4));
  }
  if (driver.type == "downbeat") return every * beatsPerBar * secondsPerBeat;
  if (driver.type == "beat") return every * secondsPerBeat;
  return 0;
}

MergedSettingsGroups mergeSettingsGroups(const std::vector<const SettingsGroup*>& groups) {
  MergedSettingsGroups merged;
  for (const SettingsGroup* group : groups) {
    if (group == nullptr) continue;
    for (const DriverLane& lane : group->lanes) merged.lanes.push_back({group->id, lane});
    // A later group in the entry's list wins any scalar the earlier ones also
    // set, so reading the list top to bottom reads as layering.
    for (const auto& [effect, mode] : group->combine) merged.combine[effect] = mode;
    for (const auto& [id, value] : group->staticSettings) merged.staticSettings[id] = value;
  }
  return merged;
}

LaneEvaluation evaluateDriverLanes(
    const std::vector<ScopedLane>& lanes,
    const std::unordered_map<std::string, CombineMode>& combine,
    const std::unordered_map<std::string, EffectDeclaration>& declarations,
    const SignalFrame& frame, DriverStates& states) {
  std::unordered_map<std::string, std::vector<Contribution>> contributions;

  for (const ScopedLane& scoped : lanes) {
    const double lanePeriod = driverPeriodSeconds(scoped.lane.driver, frame);
    for (const EffectBinding& binding : scoped.lane.bindings) {
      const auto declared = declarations.find(binding.effect);
      if (declared == declarations.end()) continue;
      const EffectDeclaration& declaration = declared->second;
      const double low =
          clampToDeclaration(declaration, binding.hasMin ? binding.min : declaration.min);
      const double high = std::max(
          low, clampToDeclaration(declaration, binding.hasMax ? binding.max : declaration.max));
      const std::string slot = scoped.groupId + ":" + scoped.lane.id + ":" + binding.id;
      double signal = driverSignal(scoped.lane.driver, binding, frame, states, slot + ":0");
      // Modifiers add to the main driver rather than gating it, so "downbeat
      // plus bass" reads as the hit sitting on top of whatever the bass is
      // already doing.
      for (std::size_t index = 0; index < scoped.lane.modifiers.size(); ++index) {
        signal += driverSignal(scoped.lane.modifiers[index], binding, frame, states,
                               slot + ":" + std::to_string(index + 1));
      }
      signal = std::max(0.0, std::min(kMaxLaneSignal, signal));
      contributions[binding.effect].push_back({(high - low) * signal, lanePeriod, low});
    }
  }

  LaneEvaluation result;
  for (const auto& [effect, list] : contributions) {
    const auto declared = declarations.find(effect);
    if (declared == declarations.end() || list.empty()) continue;
    const EffectDeclaration& declaration = declared->second;
    double resting = -std::numeric_limits<double>::infinity();
    for (const Contribution& entry : list) resting = std::max(resting, entry.restingValue);

    const auto mode = combine.find(effect);
    double total = 0;
    if (mode != combine.end() && mode->second == CombineMode::Strongest) {
      // The rarest lane that is actually firing takes the effect outright.
      // Equal periods fall back to lane order, last one winning, matching the
      // way colliding scalars layer.
      const Contribution* best = nullptr;
      for (const Contribution& entry : list) {
        if (entry.amount == 0) continue;
        if (best == nullptr || entry.period >= best->period) best = &entry;
      }
      total = best != nullptr ? best->amount : 0.0;
    } else {
      for (const Contribution& entry : list) total += entry.amount;
    }

    const double range = std::max(0.0, declaration.max - declaration.min);
    const double ceiling = resting + range * kOvershootRanges;
    const double value = resting + total;
    result.values[effect] = std::isfinite(value)
                                ? std::max(declaration.min, std::min(ceiling, value))
                                : declaration.defaultValue;
    result.driven.insert(effect);
  }
  return result;
}

}  // namespace nova
