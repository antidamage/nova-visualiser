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

// The event index a subdivided pulse is on: the whole-pulse index plus how far
// through it the frame is, scaled by the subdivision. At `divide == 1` this is
// exactly the whole-pulse index, so an undivided driver is untouched.
int subdividedIndex(int index, double phase, int divide) {
  if (divide <= 1) return index;
  const double position = index + std::max(0.0, std::min(1.0, finite(phase, 0)));
  return static_cast<int>(std::floor(position * divide));
}

// The pulse a `random` driver's window is measured in. An unrecognised cadence
// reads as `beat`, so a configuration from a newer dashboard degrades to the
// commonest window rather than to silence.
Driver randomCadenceDriver(const Driver& driver) {
  Driver cadence = driver;
  cadence.type = (driver.cadence == "beat" || driver.cadence == "downbeat" ||
                  driver.cadence == "timer" || driver.cadence == "song")
                     ? driver.cadence
                     : std::string("beat");
  return cadence;
}

struct WindowPosition {
  long long index;
  double fraction;
};

// Where the frame sits inside the driver's firing window, as a whole window
// index and a 0..1 fraction through it.
//
// The window is the whole span between one firing opportunity and the next --
// `every` windows of the pulse, or one `divide`th of it -- which is exactly the
// span `driverPeriodSeconds` measures. That is what makes "every 4th downbeat"
// mean one fire somewhere in four bars rather than a fire inside the fourth.
//
// `song` has no position: a track's length is not known until it ends, so there
// is no "fraction through" it to place anything at. Returns false there, and the
// caller falls back to firing on the track change itself.
bool driverWindowPosition(const Driver& driver, const SignalFrame& frame, WindowPosition& out) {
  if (driver.type == "song") return false;
  double position = 0;
  if (driver.type == "timer") {
    position = finite(frame.time, 0) / std::max(0.25, finite(driver.intervalSeconds, 4));
  } else {
    const int whole = driver.type == "downbeat" ? frame.barIndex : frame.beatIndex;
    const double phase = driver.type == "downbeat" ? frame.barPhase : frame.beatPhase;
    // Same continuous position `subdividedIndex` floors, kept unfloored: the
    // fractional part is the whole point here.
    position = (whole + std::max(0.0, std::min(1.0, finite(phase, 0)))) * driverDivide(driver);
  }
  // `every` and `divide` are the two directions of one control and never both
  // apply, so this scales by whichever is in play.
  const int every = std::max(1, driver.every);
  const int offset = std::max(0, std::min(every - 1, driver.offset));
  const double cycles = (position - offset) / every;
  const double index = std::floor(cycles);
  out.index = static_cast<long long>(index);
  out.fraction = cycles - index;
  return true;
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
  // A subdivided driver carries its subdivision in the key so that changing the
  // subdivision live always reads as a new event, and so the keys an undivided
  // driver produces are byte-for-byte the ones the conformance corpus recorded.
  const int divide = driverDivide(driver);
  const std::string suffix = divide > 1 ? "/" + std::to_string(divide) : std::string();
  if (driver.type == "downbeat") {
    const int index = subdividedIndex(frame.barIndex, frame.barPhase, divide);
    return driverFiresOn(index, driver.every, driver.offset)
               ? "d" + suffix + ":" + std::to_string(index)
               : std::string();
  }
  const int index = subdividedIndex(frame.beatIndex, frame.beatPhase, divide);
  return driverFiresOn(index, driver.every, driver.offset)
             ? "b" + suffix + ":" + std::to_string(index)
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

// `random` is a pulse whose timing is jittered: it fires exactly once per
// cadence window, at a point drawn at random from inside that window, and draws
// a new point when the window rolls over. So a `downbeat` random driver fires
// somewhere before the next downbeat, and the downbeat resets where it will fire
// next time.
//
// It runs the binding's envelope like every other pulse -- the randomness is in
// *when*, not in the shape. Randomising the value it drives to is a separate,
// stackable thing: the binding's `randomValue`.
//
// The threshold is seeded from the slot key and the window, so every engine
// jitters identically for the same window of the same track.
double advanceJitteredPulse(DriverSlotState& state, const Driver& driver,
                            const EffectBinding& binding, const SignalFrame& frame, double delta,
                            const std::string& key) {
  const Driver cadence = randomCadenceDriver(driver);
  const EnvelopeTimes times = envelopeTimes(binding);
  WindowPosition position;
  // A song has no interior to place a fire inside, so a song-cadence random
  // driver is simply the song pulse. Better than pretending to jitter.
  if (!driverWindowPosition(cadence, frame, position)) {
    return advancePulseEnvelope(state, times, delta, pulseEventKey(cadence, frame, state));
  }

  const int divide = driverDivide(cadence);
  const std::string prefix =
      cadence.type == "downbeat" ? "d" : cadence.type == "timer" ? "t" : "b";
  const std::string suffix = divide > 1 ? "/" + std::to_string(divide) : std::string();
  const std::string windowKey =
      "r" + prefix + suffix + ":" + std::to_string(position.index);
  if (windowKey != state.windowKey) {
    state.windowKey = windowKey;
    const uint64_t seed = stableSeed(key + ":" + windowKey);
    state.target = static_cast<double>(seed % 1000003) / 1000002.0;
    state.fired = false;
  }

  std::string eventKey;
  if (!state.fired && position.fraction >= state.target) {
    state.fired = true;
    // The `!` keeps a fire distinct from the window it belongs to, so the
    // envelope's "is this a new event" test can never confuse the two.
    eventKey = windowKey + "!";
  }
  return advancePulseEnvelope(state, times, delta, eventKey);
}

double driverSignal(const Driver& driver, const EffectBinding& binding, const SignalFrame& frame,
                    DriverStates& states, const std::string& key) {
  DriverSlotState& state = states[key];
  const double delta = std::max(1.0 / 120.0, std::min(0.25, finite(frame.delta, 1.0 / 60.0)));
  if (driver.type == "random") {
    return advanceJitteredPulse(state, driver, binding, frame, delta, key);
  }
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

// How far up its range a binding reaches this tick, 0..1.
//
// Normally 1 -- the lane sweeps the whole authored range. With `randomValue` the
// top of the sweep is drawn at random on each lane event and held until the next
// one, so the envelope still ramps from the bottom of the range but stops
// somewhere new every time.
//
// The draw is keyed off the primary driver's event key, which changes exactly
// when the lane fires. Each binding draws from its own slot key, so two
// randomised effects in one lane move independently rather than in lockstep.
//
// A continuous driver never writes an event key, so a level-driven lane draws
// once and holds it -- there is no event to re-draw on. The dashboard says so.
double randomValueScale(const EffectBinding& binding, DriverStates& states,
                        const std::string& slot) {
  if (!binding.randomValue) return 1.0;
  DriverSlotState& roll = states[slot + ":rnd"];
  const auto primary = states.find(slot + ":0");
  const std::string eventKey = primary != states.end() ? primary->second.eventKey : std::string();
  if (!roll.seeded || eventKey != roll.eventKey) {
    roll.seeded = true;
    roll.eventKey = eventKey;
    const uint64_t seed = stableSeed(slot + ":rnd:" + eventKey);
    roll.target = static_cast<double>(seed % 1000003) / 1000002.0;
  }
  return roll.target;
}

}  // namespace

bool isPulseDriver(const Driver& driver) {
  return driver.type == "beat" || driver.type == "downbeat" || driver.type == "timer" ||
         driver.type == "song";
}

int driverDivide(const Driver& driver) {
  // Anything other than a supported subdivision reads as the whole pulse, so a
  // configuration this build does not understand degrades to the behaviour it
  // had before subdivisions existed rather than to silence.
  switch (driver.divide) {
    case 2:
    case 4:
    case 8: return driver.divide;
    default: return 1;
  }
}

bool driverFiresOn(int index, int every, int offset) {
  const int cycle = std::max(1, every);
  if (cycle == 1) return true;
  const int phase = std::max(0, std::min(cycle - 1, offset));
  return (((index - phase) % cycle) + cycle) % cycle == 0;
}

double driverPeriodSeconds(const Driver& driver, const SignalFrame& frame) {
  // A random driver fires exactly once per window, so its rarity IS its
  // window -- the same period the cadence pulse would have had.
  if (driver.type == "random") return driverPeriodSeconds(randomCadenceDriver(driver), frame);
  const int every = std::max(1, driver.every);
  const double secondsPerBeat = std::max(1e-6, 60.0 / std::max(1.0, finite(frame.bpm, 72)));
  const double beatsPerBar = std::max(1, frame.timeSignature);
  // A song is the rarest thing that can happen, and its length is unknown ahead
  // of time, so it always outranks a counted pulse.
  if (driver.type == "song") return std::numeric_limits<double>::infinity();
  if (driver.type == "timer") {
    return every * std::max(0.25, finite(driver.intervalSeconds, 4));
  }
  // Subdividing makes a lane commoner, which is exactly what `strongest` ranks
  // by, so a quarter-beat lane loses to a plain beat the same way a beat loses
  // to a downbeat.
  const double divide = driverDivide(driver);
  if (driver.type == "downbeat") return every * beatsPerBar * secondsPerBeat / divide;
  if (driver.type == "beat") return every * secondsPerBeat / divide;
  return 0;
}

bool isOverrideOnlyEffect(const std::string& effect) {
  return effect == "__centreTransition" || effect == "__centreTransitionAxis" ||
         effect == "__centreTransitionDivisions" || effect == "__centreTransitionReturn";
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
      const double reach = randomValueScale(binding, states, slot);
      contributions[binding.effect].push_back({(high - low) * signal * reach, lanePeriod, low});
    }
  }

  LaneEvaluation result;
  for (const auto& [effect, list] : contributions) {
    const auto declared = declarations.find(effect);
    if (declared == declarations.end() || list.empty()) continue;
    const EffectDeclaration& declaration = declared->second;
    double resting = -std::numeric_limits<double>::infinity();
    for (const Contribution& entry : list) resting = std::max(resting, entry.restingValue);

    const auto found = combine.find(effect);
    CombineMode mode = found != combine.end() ? found->second : CombineMode::Add;
    if (isOverrideOnlyEffect(effect)) mode = CombineMode::Override;
    double total = 0;
    if (mode == CombineMode::Override) {
      // A replacement, not a contribution: the last lane in merge order takes
      // the effect outright and brings its OWN resting value with it, rather
      // than sitting on the shared floor every other mode builds from. That is
      // what makes an override settings group beat the defaults instead of
      // adding to them.
      resting = list.back().restingValue;
      total = list.back().amount;
    } else if (mode == CombineMode::Strongest || mode == CombineMode::Common) {
      // One firing lane takes the effect outright, chosen by how often it
      // fires: Strongest wants the least frequent, Common the most. Equal
      // periods fall back to lane order, last one winning, matching the way
      // colliding scalars layer.
      //
      // A continuous driver has no period at all (0), so under Strongest it
      // never wins outright. Common has to exclude it explicitly for the same
      // reason inverted -- otherwise a level lane, being the "most frequent"
      // thing there is, would win every time and no pulse could be heard.
      const bool rarest = mode == CombineMode::Strongest;
      const Contribution* best = nullptr;
      for (const Contribution& entry : list) {
        if (entry.amount == 0) continue;
        if (!rarest && !(entry.period > 0)) continue;
        if (best == nullptr ||
            (rarest ? entry.period >= best->period : entry.period <= best->period)) {
          best = &entry;
        }
      }
      // Nothing but continuous lanes are contributing, so Common falls back to
      // summing them rather than going silent.
      if (best == nullptr && !rarest) {
        for (const Contribution& entry : list) total += entry.amount;
      } else {
        total = best != nullptr ? best->amount : 0.0;
      }
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
