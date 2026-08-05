// Fixed-step Phonoscope scene simulation.
//
// Port of `PhonoscopeSimulation.swift`, with two deliberate changes:
//
//  1. The tick is a fixed 1/120 s substep driven by an accumulator instead of a
//     variable wall-clock delta. Simulation outcome no longer depends on render
//     rate, which is what makes the conformance corpus reproducible and lets
//     the renderer interpolate between states.
//  2. The per-module random seed uses an explicit FNV-1a over the module id
//     rather than Swift's `String.hashValue`, which is randomly salted per
//     process and so could never agree between engines -- or between two
//     launches of the same engine.
//
// Everything else -- effect propagation budgets, Gerstner wave shaping, bounds
// handling, palette resolution, the render particle emission order -- is a
// faithful reproduction, because divergence shows up as a visible difference
// between the streamed render and the tvOS fallback.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/expression.h"
#include "core/module.h"
#include "core/palette.h"
#include "core/signal.h"
#include "core/snapshot.h"

namespace nova {

// Fixed simulation substep. Also the value exposed to modules as `delta`.
inline constexpr double kSimulationStep = 1.0 / 120.0;

struct SimulationInput {
  std::shared_ptr<const Module> module;
  SignalFrame signal;
  std::unordered_map<std::string, double> settings;
  std::unordered_set<std::string> driverInterpolatedSettings;
  Palette palette;
  std::string message;
  double messageScale = 1.0;
  // Final glow overlay. Blur amount 0-20 and opacity 0-100 as authored; the
  // blend mode is Photoshop's, "screen", "multiply" or "overlay".
  double glowBlurAmount = 0.0;
  double glowOpacity = 0.0;
  GlowBlendMode glowBlendMode = GlowBlendMode::Screen;
  double transitionDuration = 0.6;
  bool transitionPaused = false;
  int reloadGeneration = 0;
};

class Simulation {
 public:
  Simulation();

  // Replaces the pending input. Cheap; called from the config thread.
  void submit(SimulationInput input);

  // Advances by exactly one fixed step and returns the published snapshot.
  SnapshotPtr step();

  uint64_t structureSerial() const { return structureSerial_; }

 private:
  struct Entity {
    Vec3 position;
    Vec3 origin;
    Vec3 velocity;
    float energy = 0;
    float phase = 0;
    float age = 0;
    float lifetime = 0;
    float size = 0.012f;
    float energySize = 0.028f;
    float beatSize = 0.004f;
    float flareThreshold = 2;
    float flareSize = 0;
    float flareGlow = 0;
    float glow = 0.4f;
    float trailLength = 0;
    float primitive = 0;
    float material = 0;
    Vec4 color{0.22f, 0.72f, 1.0f, 0.8f};
    bool usesThemePalette = false;
    std::string paletteStartSlot = "primary";
    std::string paletteEndSlot = "primary";
    std::string paletteGlowStartSlot = "primary";
    std::string paletteGlowEndSlot = "primary";
    std::string paletteTrailStartSlot = "primary";
    std::string paletteTrailEndSlot = "primary";
    bool usesAnalyticWave = false;
    Vec3 waveOffset;
    float waveTarget = 0;
    float waveEnergy = 0;
    float waveAttack = 0.04f;
    float waveRelease = 0.55f;
    float inverseMass = 1;
    float inertia = 0.985f;
    float drag = 0;
  };

  struct FieldWaveSource {
    json::Value strength, speed, falloff, wavelength, steepness;
    json::Value anticipation, attack, release, flashPower;
    bool present = false;
  };

  struct FieldRange {
    size_t begin = 0;
    size_t end = 0;
    int columns = 1;
    int rows = 1;
    int depth = 1;
    std::string topology = "grid";
    float spacing = 1;
    json::Value wireframe;
    std::string lineStartSlot = "linePrimary";
    std::string lineEndSlot = "lineSecondary";
    FieldWaveSource radialBeatWave;

    size_t count() const { return end - begin; }
    bool contains(size_t index) const { return index >= begin && index < end; }
  };

  struct FieldWave {
    size_t fieldIndex = 0;
    Vec3 center;
    float speed = 0;
    float falloff = 0;
    float strength = 0;
    float wavelength = 0;
    float steepness = 0;
    float anticipation = 0;
    float attack = 0;
    float release = 0;
    float flashPower = 0;
    float maximumRadius = 0;
    float radius = 0;
  };

  struct EffectDelivery {
    size_t receiver = 0;
    size_t source = 0;
    uint64_t token = 0;
    int hop = 0;
    float strength = 0;
  };

  struct Emitter {
    std::string templateId;
    Vec3 origin;
    int burst = 1;
    int maximum = 1;
    int emitted = 0;
  };

  // Per-entity render fields a module setting may drive live, i.e. without a
  // structural rebuild. Discovered from each setting's `affects` paths, which
  // generalises what the tvOS engine hard-codes for `particle-ripples`.
  enum class LiveField { FlareThreshold, FlareGlow, TrailLength, FlareSize, Glow };

  // A tiny deterministic LCG matching the tvOS `random()` closures.
  struct Random {
    uint64_t state = 0;
    uint64_t multiplier = 6364136223846793005ULL;
    uint64_t increment = 1;
    float next() {
      state = state * multiplier + increment;
      return static_cast<float>((state >> 40) & 0x00ffffff) / static_cast<float>(0x00ffffff);
    }
  };

  void ingest();
  void rebuild();
  json::Value resolveTemplate(const json::Value& value) const;
  void instantiateEntity(const json::Value& value, const Vec3& position, Random& random, int depth);
  Entity styledEntity(const json::Value& value, const Vec3& position, Random& random) const;
  ExpressionInputs expressionInputs(double random) const;
  FieldWaveSource radialBeatWave(const json::Value& scene) const;
  void advanceConfiguration(double delta);
  void emitBeatParticles();
  void enqueueRootEffects();
  void advanceFieldWaves(float dt);
  void processEffects(Diagnostics& diagnostics);
  std::vector<size_t> neighbours(size_t index) const;
  void integrate(float dt);
  void applyBounds(size_t index, const Vec3& minimum, const Vec3& maximum, const std::string& mode,
                   float restitution, bool is3D);
  SnapshotPtr publish(Diagnostics diagnostics, double elapsedMilliseconds);

  static float primitiveCode(const std::string& name);
  static float materialCode(const std::string& name);

  // Pending input, swapped in at the top of each tick.
  std::mutex inputMutex_;
  SimulationInput pending_;
  bool hasPending_ = false;

  // Live state.
  std::shared_ptr<const Module> module_;
  SignalFrame signal_ = SignalFrame::idle();
  std::string message_;
  double messageScale_ = 1.0;
  double glowBlurAmount_ = 0.0;
  double glowOpacity_ = 0.0;
  GlowBlendMode glowBlendMode_ = GlowBlendMode::Screen;
  std::unordered_map<std::string, double> settings_;
  std::unordered_map<std::string, double> targetSettings_;
  std::unordered_set<std::string> driverInterpolatedSettings_;
  Palette palette_ = Palette::defaults();
  Palette targetPalette_ = Palette::defaults();
  double transitionDuration_ = 0.6;
  bool transitionPaused_ = false;
  int reloadGeneration_ = 0;
  std::string moduleKey_;

  std::vector<Entity> entities_;
  std::vector<FieldRange> fields_;
  std::vector<FieldWave> fieldWaves_;
  std::vector<uint64_t> visitedTokens_;
  std::vector<EffectDelivery> effectQueue_;
  std::vector<Emitter> emitters_;
  std::vector<std::pair<LiveField, std::string>> liveFlareSettings_;

  uint64_t serial_ = 0;
  uint64_t structureSerial_ = 0;
  uint64_t effectSequence_ = 1;
  long lastBeatIndex_ = -2147483648L;
  double simulationTime_ = 0;
};

}  // namespace nova
