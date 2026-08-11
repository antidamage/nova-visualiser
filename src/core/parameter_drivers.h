// The Phonoscope driver-lane evaluator.
//
// Port of `nova-ha-dashboard/lib/phonoscope-drivers.ts`, which is the reference
// implementation. The Swift twin is `resolvedLaneValues` in
// `NovaAppleTVDashboard/PhonoscopeStore.swift`. All three must agree, and
// `tests/conformance/parameter-drivers` is what proves it.
//
// This lives in `nova_visualiser_core` rather than beside the engine so the
// conformance runner can reach it without a GPU. The pre-lane evaluator lived
// in `service/engine.cpp`, which is exactly why driver resolution had no
// conformance coverage at all.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/signal.h"

namespace nova {

// Private picture-level effects. Household configuration, declared by no module
// manifest, resolved through the same lanes as everything else.
inline constexpr const char* kGlowBlurEffect = "__glowBlur";
inline constexpr const char* kGlowOpacityEffect = "__glowOpacity";
inline constexpr const char* kGlowBlendEffect = "__glowBlend";
inline constexpr const char* kMessageScaleEffect = "__messageScale";
// `__hueOffset` and `__themeChange` are resolved by the dashboard rather than
// here: one drives House Party lighting, the other advances the rotation.

// A combined value may exceed the effect's declared maximum -- stacking lanes is
// meant to be able to overshoot. This is only the guard that keeps the
// simulation finite: at most four full ranges above the resting value.
inline constexpr double kOvershootRanges = 4.0;
// The most a lane's summed driver signal can reach before it is clamped.
inline constexpr double kMaxLaneSignal = 4.0;

struct EffectDeclaration {
  std::string id;
  double min = 0;
  double max = 1;
  double step = 0;
  double defaultValue = 0;
};

struct Driver {
  // "beat", "downbeat", "timer", "song", "energy", "bass", "mid", "treble",
  // "random". A raw string rather than an enum because it arrives as JSON and
  // an unrecognised value must degrade rather than fail to parse.
  std::string type;
  int every = 1;
  int offset = 0;
  // Subdivisions per pulse, 1/2/4/8 — the other direction from `every`. Only
  // `beat` and `downbeat` (and a `random` whose cadence is one of them)
  // subdivide; a subdivided driver always has `every == 1` and `offset == 0`.
  int divide = 1;
  double intervalSeconds = 4;
  // `random` only: the pulse whose interval is the window it fires somewhere
  // inside. `every` and `divide` size that window rather than selecting which
  // pulses count.
  std::string cadence = "beat";
};

// Sparse on purpose: an unset optional inherits the effect's declaration, so a
// binding stores only what the user actually chose to change.
struct EffectBinding {
  std::string id;
  std::string effect;
  bool hasMin = false;
  double min = 0;
  bool hasMax = false;
  double max = 0;
  bool hasAttack = false;
  double attackSeconds = 0.05;
  bool hasHold = false;
  double holdSeconds = 0;
  bool hasRelease = false;
  double releaseSeconds = 0.6;
  // Draw the target at random from inside [min, max] on each lane event instead
  // of always driving to max. No has* pair: there is no declared default to
  // inherit, so absent simply means off. The envelope is untouched -- it still
  // shapes the approach from the bottom of the range up to whatever was drawn.
  bool randomValue = false;
  std::unordered_map<std::string, double> params;
};

struct DriverLane {
  std::string id;
  Driver driver;
  std::vector<Driver> modifiers;
  std::vector<EffectBinding> bindings;
};

// A lane paired with the settings group it came from, because the state key and
// the layering order both depend on it.
struct ScopedLane {
  std::string groupId;
  DriverLane lane;
};

// How an effect resolves when more than one lane drives it at once.
//
//  - Add        sums every contribution above the shared resting floor.
//  - Strongest  gives it to the LEAST frequent firing lane outright.
//  - Common     gives it to the MOST frequent firing lane outright.
//  - Override   REPLACES the value with the last contributing lane's, resting
//               value included -- the one mode that is not a contribution.
//
// The wire values `add` and `strongest` are unchanged because saved
// configurations already hold them; the dashboard labels the four Sum, Least
// frequent lane wins, Most frequent lane wins and Override.
enum class CombineMode { Add, Strongest, Common, Override };

// Effects that always combine by Override whatever a settings group stored.
//
// A transition is one indivisible instruction: half a flip summed with half a
// slide is not a transition, it is a fault. Mirrors
// PHONOSCOPE_OVERRIDE_ONLY_EFFECTS in the dashboard.
bool isOverrideOnlyEffect(const std::string& effect);

struct SettingsGroup {
  std::string id;
  std::string name;
  std::string moduleId;
  std::vector<DriverLane> lanes;
  std::unordered_map<std::string, CombineMode> combine;
  std::unordered_map<std::string, double> staticSettings;
  bool isDefault = false;
};

struct MergedSettingsGroups {
  std::vector<ScopedLane> lanes;
  std::unordered_map<std::string, CombineMode> combine;
  std::unordered_map<std::string, double> staticSettings;
};

// Per driver-slot runtime state. A slot is one driver of one binding, so the
// primary driver and each modifier keep independent envelope phases.
struct DriverSlotState {
  enum class Phase { Idle, Attack, Hold, Release };
  double level = 0;
  Phase phase = Phase::Idle;
  double holdRemaining = 0;
  std::string eventKey;
  double current = 0;
  double target = 0;
  // `song` has no natural index, so the slot counts track changes itself.
  int eventCount = 0;
  uint64_t lastTrackSeed = 0;
  bool seenTrack = false;
  // `random` timing: the window `target` holds a threshold for, and whether that
  // window's one fire has already happened.
  std::string windowKey;
  bool fired = false;
  // `randomValue` slots only: whether a value has ever been drawn. Without it a
  // lane whose driver never writes an event key -- every continuous driver --
  // would sit on the zero `target` forever and hold the effect at its floor.
  bool seeded = false;
};

using DriverStates = std::unordered_map<std::string, DriverSlotState>;

struct LaneEvaluation {
  std::unordered_map<std::string, double> values;
  std::unordered_set<std::string> driven;
};

// Whether event `index` is one the driver fires on. `every` is the cycle length
// and `offset` picks which event within it.
bool driverFiresOn(int index, int every, int offset);

// How many times per pulse the driver fires: 1, 2, 4 or 8. Any other stored
// value reads as the whole pulse.
int driverDivide(const Driver& driver);

// How rarely a lane fires, in seconds, used to rank lanes when an effect
// combines by `strongest` or `common`. Longer wins under `strongest`.
// Continuous drivers return 0 and never win outright either way; `random`
// returns its cadence's period, because it fires exactly once per window.
double driverPeriodSeconds(const Driver& driver, const SignalFrame& frame);

// One of the four literal pulse types -- not `random`, which borrows one.
bool isPulseDriver(const Driver& driver);

// The lanes and scalars of several settings groups, merged in the order the
// colour group entry named them: lanes stack, scalars layer with the last
// group winning.
MergedSettingsGroups mergeSettingsGroups(const std::vector<const SettingsGroup*>& groups);

// Resolve every effect the given lanes drive. `declarations` supplies the range
// and default each binding inherits; a binding naming an effect the active
// module does not declare is skipped.
LaneEvaluation evaluateDriverLanes(
    const std::vector<ScopedLane>& lanes,
    const std::unordered_map<std::string, CombineMode>& combine,
    const std::unordered_map<std::string, EffectDeclaration>& declarations,
    const SignalFrame& frame, DriverStates& states);

}  // namespace nova
