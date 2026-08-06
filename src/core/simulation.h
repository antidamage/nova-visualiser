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
#include "core/image.h"
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
  // The live colour theme's centre image, already decoded. A non-blank message
  // wins over it; that is `Simulation::submit`'s decision.
  std::shared_ptr<const DecodedImage> themeImage;
  // The centre slot's two size axes. `centreHeight` is how tall the image is as
  // a percentage of the frame; `messageScale` is the driven multiplier on top,
  // shared with the message.
  double centreHeight = 33.0;
  double messageScale = 1.0;
  // Final glow overlay. Blur amount 0-20 and opacity 0-100 as authored; the
  // blend mode is Photoshop's, "screen", "multiply" or "overlay".
  double glowBlurAmount = 0.0;
  double glowOpacity = 0.0;
  double glowOverdrive = 1.0;
  bool glowClamped = true;
  GlowBlendMode glowBlendMode = GlowBlendMode::Screen;
  // Frame geometry and vignette. Driven, so they arrive already resolved for
  // this frame. The first three are PERCENTAGES of the render view, as authored
  // -- `Simulation::submit` is where they become fractions, and everything
  // downstream of it is in unit space. The defaults are the original fixed
  // letterbox and its authored edge gradients. Vignette size stays a plain
  // multiplier: it is allowed past 1, which is how it closes the band to a slit.
  double backgroundHeight = 33.0;
  double backgroundWidth = 100.0;
  double vignetteOpacity = 96.0;
  double vignetteSize = 1.0;
  SceneBlendMode sceneBlendMode = SceneBlendMode::Linear;
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

    // Extent gating. A field that declares `extentX`/`extentY` is allocated at
    // its full 100% size and then gated down to a centred sub-rectangle every
    // tick, because extent is a driven parameter and a driven parameter must
    // never trigger a structural rebuild. Fields that declare neither leave
    // these at the allocated size, which is the same thing as no gating.
    bool gated = false;
    json::Value extentX;
    json::Value extentY;
    int liveColumns = 1;
    int liveRows = 1;
    // Index of the first live column/row inside the allocated grid. Always
    // `(allocated - live) / 2` exactly: `live` is snapped to the allocated
    // count's parity so the sub-rectangle is centred on a whole cell rather
    // than sliding half a gap as the extent sweeps.
    int columnBegin = 0;
    int rowBegin = 0;

    size_t count() const { return end - begin; }
    bool contains(size_t index) const { return index >= begin && index < end; }

    // Whether an allocated cell is inside the live sub-rectangle. Dead cells are
    // not integrated, receive no effects and emit no particles, so the running
    // cost tracks the live extent even though the allocation is worst-case.
    bool cellIsLive(int x, int y) const {
      if (!gated) return true;
      return x >= columnBegin && x < columnBegin + liveColumns && y >= rowBegin &&
             y < rowBegin + liveRows;
    }
    bool indexIsLive(size_t index) const {
      if (!gated) return true;
      const int local = static_cast<int>(index - begin);
      const int x = local % columns;
      const int y = (local / columns) % rows;
      return cellIsLive(x, y);
    }
    size_t liveCount() const {
      return gated ? static_cast<size_t>(liveColumns) * static_cast<size_t>(liveRows) *
                         static_cast<size_t>(depth)
                   : count();
    }
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
  // Resolves each gated field's live sub-rectangle from the current settings.
  // Runs every tick, before anything reads the field, because extent is driven.
  void applyFieldExtents();
  bool entityLive(size_t index) const {
    return index >= entityLive_.size() || entityLive_[index] != 0;
  }
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
  // The centre slot. `centreImage_` is what should be on screen now and
  // `centreImagePrev_` what is fading out behind it; `centreImageFade_` is the
  // incoming one's weight, ramped linearly over the rotation's transition. A
  // finished fade releases the outgoing image, which is why this is a ramp and
  // not the exponential chase the palette uses -- a chase never arrives.
  std::shared_ptr<const DecodedImage> centreImage_;
  std::shared_ptr<const DecodedImage> centreImagePrev_;
  double centreImageFade_ = 1.0;
  double centreImageFadeSeconds_ = 0.0;
  double centreHeight_ = 0.33;
  double messageScale_ = 1.0;
  double glowBlurAmount_ = 0.0;
  double glowOpacity_ = 0.0;
  double glowOverdrive_ = 1.0;
  bool glowClamped_ = true;
  GlowBlendMode glowBlendMode_ = GlowBlendMode::Screen;
  double backgroundHeight_ = 1.0 / 3.0;
  double backgroundWidth_ = 1.0;
  double vignetteOpacity_ = 0.96;
  double vignetteSize_ = 1.0;
  SceneBlendMode sceneBlendMode_ = SceneBlendMode::Linear;
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
  // Parallel to `entities_`: 1 while the entity is inside its field's live
  // extent. Recomputed by `applyFieldExtents` rather than derived per read,
  // because integrate, effect propagation and publish all consult it per entity
  // per tick. Entities outside any gated field -- beat-emitted particles, every
  // ungated module -- are permanently 1.
  std::vector<uint8_t> entityLive_;
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
