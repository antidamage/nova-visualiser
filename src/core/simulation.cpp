#include "core/simulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/centre_image_reference.h"

namespace nova {
namespace {

constexpr float kPi = 3.14159265358979323846f;

double now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// Picks the first non-empty slot from an ordered list of candidates, matching
// the `?? legacy ?? fallback` chains in `styledEntity`.
std::string firstSlot(std::initializer_list<const std::vector<std::string>*> candidates,
                      const std::string& fallback, size_t skip = 0) {
  for (const std::vector<std::string>* list : candidates) {
    if (list->size() > skip) return (*list)[skip];
  }
  return fallback;
}

}  // namespace

Simulation::Simulation() { effectQueue_.reserve(8192); }

void Simulation::submit(SimulationInput input) {
  std::lock_guard<std::mutex> lock(inputMutex_);
  pending_ = std::move(input);
  hasPending_ = true;
}

float Simulation::primitiveCode(const std::string& rawName) {
  std::string name = rawName;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (name == "ring") return 1;
  if (name == "square" || name == "plane" || name == "quad" || name == "sprite") return 2;
  if (name == "triangle") return 3;
  if (name == "wireframe" || name == "cube" || name == "box" || name == "icosphere") return 4;
  if (name == "trail" || name == "line") return 5;
  return 0;
}

float Simulation::materialCode(const std::string& rawName) {
  std::string name = rawName;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (name == "phong" || name == "lit") return 1;
  if (name == "wireframe") return 2;
  return 0;
}

void Simulation::ingest() {
  SimulationInput next;
  {
    std::lock_guard<std::mutex> lock(inputMutex_);
    if (!hasPending_) return;
    next = pending_;
    hasPending_ = false;
  }

  const std::string nextKey = next.module ? next.module->key() : std::string();
  const bool requiresRebuild = nextKey != moduleKey_ || next.reloadGeneration != reloadGeneration_;

  if (!requiresRebuild) {
    for (const auto& [key, target] : next.settings) {
      auto currentTarget = targetSettings_.find(key);
      const bool targetChanged =
          currentTarget == targetSettings_.end() || currentTarget->second != target;
      const SettingInterpolationAction action = settingInterpolationAction(
          targetChanged, driverInterpolatedSettings_.count(key) > 0,
          next.driverInterpolatedSettings.count(key) > 0);
      if (action == SettingInterpolationAction::ApplyImmediately) {
        settings_[key] = target;
      } else if (settings_.find(key) == settings_.end()) {
        settings_[key] = target;
      }
    }
    for (auto it = settings_.begin(); it != settings_.end();) {
      it = next.settings.count(it->first) == 0 ? settings_.erase(it) : std::next(it);
    }
  }

  targetSettings_ = next.settings;
  driverInterpolatedSettings_ = next.driverInterpolatedSettings;
  targetPalette_ = next.palette;
  transitionDuration_ = clampValue(next.transitionDuration, 0.0, 600.0);
  transitionPaused_ = next.transitionPaused;
  // Not chased toward like the settings above: a resolution change is a cut, and
  // easing a dot through the intermediate sizes would read as a glitch.
  outputHeight_ = clampValue(next.outputHeight, 1.0, 16384.0);
  moduleKey_ = nextKey;
  module_ = next.module;
  reloadGeneration_ = next.reloadGeneration;
  signal_ = next.signal;
  // What the centre of the frame holds, in one place.
  //
  //  1. a non-blank message draws text and no image;
  //  2. otherwise the live colour theme's image, if it supplies one;
  //  3. otherwise nothing.
  //
  // Emptying the text box is "stop overriding", not "show nothing", which is the
  // only reading that makes clearing it reversible -- and a theme with no image
  // and no message draws nothing at all rather than holding the last one.
  message_ = next.message;
  std::shared_ptr<const DecodedImage> centre =
      next.message.empty() ? next.themeImage : nullptr;

  if (centre != centreImage_) {
    // Identity, not contents: two entries naming the same library image are the
    // same decoded buffer, so moving between them is not a transition at all.
    centreImagePrev_ = centreImage_;
    centreImage_ = centre;
    centreImageFadeSeconds_ = 0.0;
    // THE INITIATOR OWNS THE TRANSITION. Latched here, at the instant the image
    // changes, and then held: the incoming values describe the change that is
    // starting, and reading them again next tick would let the entry being
    // arrived at rewrite a transition already halfway through.
    centreTransitionLatched_ = next.centreTransition;
    centreTransitionAttack_ = std::max(0.0, next.transitionAttack);
    centreTransitionHold_ = std::max(0.0, next.transitionHold);
    centreTransitionRelease_ = std::max(0.0, next.transitionRelease);
    // Nothing to leave from, or no time to do it in, means it is simply there --
    // a first paint should not fly on from off screen.
    const double length =
        centreTransitionAttack_ + centreTransitionHold_ + centreTransitionRelease_;
    centreImageFade_ = (centreImagePrev_ && length > 0.0) ? 0.0 : 1.0;
  }

  // The backdrop slot, on exactly the same terms and for the same reasons. No
  // message clause: nothing overrides the backdrop, so this is simply whether
  // the live theme supplies an image. Null means the procedural field draws.
  if (next.backgroundImage != backgroundImage_) {
    backgroundImagePrev_ = backgroundImage_;
    backgroundImage_ = next.backgroundImage;
    backgroundImageFadeSeconds_ = 0.0;
    backgroundTransitionLatched_ = next.backgroundTransition;
    backgroundTransitionAttack_ = std::max(0.0, next.backgroundTransitionAttack);
    backgroundTransitionHold_ = std::max(0.0, next.backgroundTransitionHold);
    backgroundTransitionRelease_ = std::max(0.0, next.backgroundTransitionRelease);
    const double length = backgroundTransitionAttack_ + backgroundTransitionHold_
                          + backgroundTransitionRelease_;
    backgroundImageFade_ = (backgroundImagePrev_ && length > 0.0) ? 0.0 : 1.0;
  }

  // Authored as a percentage, held as a fraction -- the same convention the
  // frame geometry uses, and for the same reason: everything downstream of here
  // works in unit space.
  centreHeight_ = clampValue(next.centreHeight / 100.0, 0.0, 1.0);
  centreWidth_ = clampValue(next.centreWidth / 100.0, 0.0, 1.0);
  centreFit_ = next.centreFit;
  centreProportional_ = next.centreProportional;
  messageScale_ = clampValue<double>(next.messageScale, kImageScaleMinimum, kImageScaleMaximum);
  glowBlurAmount_ = clampValue(next.glowBlurAmount, 0.0, 20.0);
  glowOpacity_ = clampValue(next.glowOpacity, 0.0, 100.0);
  glowOverdrive_ = clampValue(next.glowOverdrive, 1.0, 10.0);
  glowClamped_ = next.glowClamped;
  glowBlendMode_ = next.glowBlendMode;
  // Authored as percentages, held as fractions. The divide happens exactly here
  // so that everything downstream -- the snapshot, both shaders,
  // background_band_reference.h and its recorded digests -- keeps working in
  // unit space and none of it had to move when the controls became 0-100.
  backgroundHeight_ = clampValue(next.backgroundHeight / 100.0, 0.0, 1.0);
  backgroundWidth_ = clampValue(next.backgroundWidth / 100.0, 0.0, 1.0);
  backgroundScale_ = clampValue<double>(next.backgroundScale, kImageScaleMinimum, kImageScaleMaximum);
  backgroundFit_ = next.backgroundFit;
  backgroundProportional_ = next.backgroundProportional;
  vignetteOpacity_ = clampValue(next.vignetteOpacity / 100.0, 0.0, 1.0);
  vignetteSize_ = clampValue(next.vignetteSize, 0.0, 3.0);
  sceneBlendMode_ = next.sceneBlendMode;

  if (requiresRebuild) {
    settings_ = next.settings;
    targetSettings_ = next.settings;
    driverInterpolatedSettings_ = next.driverInterpolatedSettings;
    targetPalette_ = next.palette;
    palette_ = next.palette;
    rebuild();
  }
}

SnapshotPtr Simulation::step() {
  const double started = now();
  ingest();
  if (!module_) return publish(Diagnostics{}, (now() - started) * 1000);

  const float dt = static_cast<float>(kSimulationStep);
  simulationTime_ += kSimulationStep;
  advanceConfiguration(kSimulationStep);
  applyFieldExtents();

  Diagnostics diagnostics;
  if (signal_.beatIndex != lastBeatIndex_) {
    lastBeatIndex_ = signal_.beatIndex;
    emitBeatParticles();
    enqueueRootEffects();
  }
  advanceFieldWaves(dt);
  processEffects(diagnostics);
  integrate(dt);
  // Counted live rather than allocated: a gated field holds cells it is not
  // drawing, and reporting those would make the overlay claim work the renderer
  // is not doing.
  size_t liveEntities = 0;
  for (size_t index = 0; index < entities_.size(); ++index) {
    if (entityLive(index)) ++liveEntities;
  }
  diagnostics.entityCount = static_cast<int>(liveEntities);
  diagnostics.particleCount =
      std::min(module_->resources().maxParticles, static_cast<int>(liveEntities));
  return publish(diagnostics, (now() - started) * 1000);
}

void Simulation::advanceConfiguration(double delta) {
  // The centre image transitions on the authored RAMP over the same span the
  // palette chases across, so the picture's centrepiece and its colours settle
  // together. A ramp rather than the chase below on purpose: an exponential
  // approach only ever gets close, so the outgoing image would never reach zero
  // and could never be released.
  //
  // Deliberately ABOVE the pause check. Pausing means "stop advancing the
  // playlist", not "freeze a transition that is already in flight" -- and a
  // manual skip pauses the rotation, so leaving this below the early return
  // stranded the progress at 0 and held the OUTGOING image on screen
  // permanently. A transition that has begun always finishes.
  if (centreImageFade_ < 1.0) {
    centreImageFadeSeconds_ += delta;
    centreImageFade_ = transitionRamp(static_cast<float>(centreImageFadeSeconds_),
                                      static_cast<float>(centreTransitionAttack_),
                                      static_cast<float>(centreTransitionHold_),
                                      static_cast<float>(centreTransitionRelease_));
    if (centreImageFade_ >= 1.0) centreImagePrev_.reset();
  }

  // The backdrop's, on its own clock and its own ramp: the two slots change at
  // the same moment but run independently, so the backdrop can still be
  // dissolving after the centrepiece has landed.
  if (backgroundImageFade_ < 1.0) {
    backgroundImageFadeSeconds_ += delta;
    backgroundImageFade_ = transitionRamp(static_cast<float>(backgroundImageFadeSeconds_),
                                          static_cast<float>(backgroundTransitionAttack_),
                                          static_cast<float>(backgroundTransitionHold_),
                                          static_cast<float>(backgroundTransitionRelease_));
    if (backgroundImageFade_ >= 1.0) backgroundImagePrev_.reset();
  }

  if (transitionPaused_) return;

  const double amount = chaseAmount(delta, transitionDuration_);
  palette_ = palette_.approached(targetPalette_, amount);
  for (const auto& [key, target] : targetSettings_) {
    if (driverInterpolatedSettings_.count(key) > 0) continue;
    auto it = settings_.find(key);
    const double current = it == settings_.end() ? target : it->second;
    settings_[key] = current + (target - current) * amount;
  }

  // Live-tunable render parameters that would otherwise only apply on rebuild.
  // Driven from the module declaration rather than a hard-coded module id, so
  // any module wiring a setting to these template paths gets the same
  // behaviour instead of `particle-ripples` being special-cased.
  if (!liveFlareSettings_.empty()) {
    for (const auto& [field, settingId] : liveFlareSettings_) {
      auto it = settings_.find(settingId);
      if (it == settings_.end()) continue;
      const float value = static_cast<float>(it->second);
      // Pixels become a clip radius once, above the entity loop: the divisor is
      // the frame's height, not anything per dot. Clamped on the same 0..0.32
      // clip axis as energySize/beatSize/flareSize. Zero is legal and means no
      // dots -- there is no visible floor to fall back to.
      const float dotSize = field == LiveField::DotSizePixels
          ? clampValue(dotSizeClip(value, static_cast<float>(outputHeight_)), 0.0f, 0.32f)
          : 0.0f;
      for (Entity& entity : entities_) {
        switch (field) {
          case LiveField::DotSizePixels: entity.size = dotSize; break;
          case LiveField::FlareThreshold: entity.flareThreshold = clampValue(value, 0.0f, 2.0f); break;
          case LiveField::FlareGlow: entity.flareGlow = clampValue(value, 0.0f, 12.0f); break;
          // Match the Swift fallback: the declaration owns the range. The
          // previous 32x clamp collapsed most of the configured 0...500 range.
          case LiveField::TrailLength: entity.trailLength = std::max(0.0f, value); break;
          case LiveField::FlareSize: entity.flareSize = clampValue(value, 0.0f, 0.32f); break;
          case LiveField::Glow: entity.glow = clampValue(value, 0.0f, 3.0f); break;
        }
      }
    }
  }
}

ExpressionInputs Simulation::expressionInputs(double random) const {
  const auto& spectrum = signal_.spectrum;
  const double bass = *std::max_element(spectrum.begin(), spectrum.begin() + 8);
  const double mid = *std::max_element(spectrum.begin() + 8, spectrum.begin() + 20);
  const double high = *std::max_element(spectrum.begin() + 20, spectrum.end());

  ExpressionInputs inputs;
  inputs.reserve(64 + settings_.size());
  inputs["time"] = signal_.time;
  inputs["delta"] = signal_.delta;
  inputs["beat.phase"] = signal_.beatPhase;
  inputs["beat.pulse"] = signal_.beatPulse;
  inputs["bar.phase"] = signal_.barPhase;
  inputs["audio.energy"] = signal_.energy;
  inputs["audio.bass"] = bass;
  inputs["audio.low"] = bass;
  inputs["audio.mid"] = mid;
  inputs["audio.high"] = high;
  inputs["lyrics.progress"] = signal_.lyricProgress;
  inputs["lyrics.pulse"] = signal_.lyricPulse;
  inputs["random.x"] = random;
  for (size_t index = 0; index < spectrum.size(); ++index) {
    inputs["spectrum." + std::to_string(index)] = spectrum[index];
  }
  for (const auto& [key, value] : settings_) inputs["settings." + key] = value;
  return inputs;
}

json::Value Simulation::resolveTemplate(const json::Value& value) const {
  const json::Object* object = value.object();
  if (object == nullptr || !module_) return value;
  const json::Value* templateId = value.find("template");
  if (templateId == nullptr) return value;
  const json::Value* templateValue = module_->templateFor(templateId->stringOr(""));
  if (templateValue == nullptr) return value;
  const json::Object* templateObject = templateValue->object();
  if (templateObject == nullptr) return value;

  json::Object merged = *templateObject;
  for (const auto& [key, item] : *object) {
    if (key == "template") continue;
    merged[key] = item;
  }
  return json::Value{std::move(merged)};
}

Simulation::FieldWaveSource Simulation::radialBeatWave(const json::Value& scene) const {
  FieldWaveSource source;
  const json::Value* sources = scene.find("effectSources");
  if (sources == nullptr) return source;
  const json::Array* items = sources->array();
  if (items == nullptr) return source;

  for (const json::Value& item : *items) {
    const json::Value* trigger = item.find("trigger");
    const json::Value* kind = item.find("kind");
    const json::Value* propagation = item.find("propagation");
    if (trigger == nullptr || kind == nullptr || propagation == nullptr) continue;
    if (trigger->stringOr("") != "beat") continue;
    if (kind->stringOr("") != "wave") continue;
    if (propagation->stringOr("") != "radial") continue;

    auto copy = [&item](std::string_view key) {
      const json::Value* value = item.find(key);
      return value != nullptr ? *value : json::Value{};
    };
    source.strength = copy("strength");
    source.speed = copy("speed");
    source.falloff = copy("falloff");
    source.wavelength = copy("wavelength");
    source.steepness = copy("steepness");
    source.anticipation = copy("anticipation");
    source.attack = copy("attack");
    source.release = copy("release");
    source.flashPower = copy("flashPower");
    source.present = true;
    return source;
  }
  return source;
}

Simulation::Entity Simulation::styledEntity(const json::Value& value, const Vec3& position,
                                            Random& random) const {
  const json::Value* renderValue = value.find("render");
  const json::Value& render = renderValue != nullptr ? *renderValue : value;

  std::string primitiveName;
  if (const json::Value* primitive = render.find("primitive")) {
    primitiveName = primitive->stringOr("");
  }
  if (primitiveName.empty()) {
    if (value.find("sprite") != nullptr) {
      primitiveName = "sprite";
    } else if (value.find("trail") != nullptr) {
      primitiveName = "trail";
    } else {
      primitiveName = "point";
    }
  }
  const std::string materialName =
      render.find("material") != nullptr ? render.find("material")->stringOr("emissive") : "emissive";

  const ExpressionInputs inputs = expressionInputs(static_cast<double>(random.next()));
  auto eval = [&](std::string_view key, double fallback) {
    return Expression::evaluate(render.find(key), inputs, fallback);
  };

  Entity entity;
  entity.position = position;
  entity.origin = position;

  const float glow = static_cast<float>(eval("glow", materialName == "emissive" ? 0.65 : 0.15));
  const float energySize = static_cast<float>(eval("energySize", 0.028));
  const float beatSize = static_cast<float>(eval("beatSize", 0.004));
  const float flareThreshold = static_cast<float>(eval("flareThreshold", 2));
  const float flareSize = static_cast<float>(eval("flareSize", 0));
  const float flareGlow = static_cast<float>(eval("flareGlow", 0));
  const float trailLength = static_cast<float>(eval("trailLength", 0));
  const float lifetime =
      static_cast<float>(Expression::evaluate(value.find("lifetime"), inputs, 0));

  // `render.dotSizePixels` is a diameter in TRUE DEVICE PIXELS of the output and
  // wins wherever it is present; `transform.scale[0]` is the legacy clip-space
  // size and stays the fallback for every module that predates the key. Both are
  // still evaluated here, at build, so the very first published frame is right
  // before `advanceConfiguration` has run the live pass once.
  float size = 0.025f;
  if (const json::Value* dotPixels = render.find("dotSizePixels")) {
    size = dotSizeClip(static_cast<float>(Expression::evaluate(dotPixels, inputs, 3.8)),
                       static_cast<float>(outputHeight_));
  } else if (const json::Value* transform = value.find("transform")) {
    const json::Value* scale = transform->find("scale");
    if (scale != nullptr) {
      if (const json::Array* items = scale->array(); items != nullptr && !items->empty()) {
        size = static_cast<float>(Expression::evaluate(&(*items)[0], inputs, 0.025));
      } else {
        size = static_cast<float>(Expression::evaluate(scale, inputs, 0.025));
      }
    }
  }

  entity.phase = random.next();
  const Vec4 fallbackColor{0.12f + entity.phase * 0.32f, 0.54f + entity.phase * 0.34f, 0.92f, 0.78f};

  auto sourceOf = [&render](std::string_view key) {
    const json::Value* value = render.find(key);
    return value != nullptr ? value->exprSource() : std::string();
  };
  const std::string colorExpr = sourceOf("color");
  const std::string colorStartExpr = sourceOf("colorStart");
  const std::string colorEndExpr = sourceOf("colorEnd");
  const std::string glowExpr = sourceOf("glowColor");
  const std::string glowStartExpr = sourceOf("glowColorStart");
  const std::string glowEndExpr = sourceOf("glowColorEnd");
  const std::string trailExpr = sourceOf("trailColor");
  const std::string trailStartExpr = sourceOf("trailColorStart");
  const std::string trailEndExpr = sourceOf("trailColorEnd");

  entity.usesThemePalette = false;
  for (const std::string* expr : {&colorExpr, &colorStartExpr, &colorEndExpr, &glowExpr,
                                  &glowStartExpr, &glowEndExpr, &trailExpr, &trailStartExpr,
                                  &trailEndExpr}) {
    if (expr->find("palette.") != std::string::npos) {
      entity.usesThemePalette = true;
      break;
    }
  }

  const std::vector<std::string> legacyColor = paletteSlots(colorExpr);
  const std::vector<std::string> colorStart = paletteSlots(colorStartExpr);
  const std::vector<std::string> colorEnd = paletteSlots(colorEndExpr);
  const std::vector<std::string> legacyGlow = paletteSlots(glowExpr);
  const std::vector<std::string> glowStart = paletteSlots(glowStartExpr);
  const std::vector<std::string> glowEnd = paletteSlots(glowEndExpr);
  const std::vector<std::string> legacyTrail = paletteSlots(trailExpr);
  const std::vector<std::string> trailStart = paletteSlots(trailStartExpr);
  const std::vector<std::string> trailEnd = paletteSlots(trailEndExpr);

  entity.paletteStartSlot = firstSlot({&colorStart, &legacyColor}, "primary");
  entity.paletteEndSlot = colorEnd.empty()
                              ? (legacyColor.size() > 1 ? legacyColor[1] : entity.paletteStartSlot)
                              : colorEnd[0];
  entity.paletteGlowStartSlot = firstSlot({&glowStart, &legacyGlow}, entity.paletteStartSlot);
  entity.paletteGlowEndSlot =
      glowEnd.empty() ? (legacyGlow.size() > 1 ? legacyGlow[1] : entity.paletteGlowStartSlot)
                      : glowEnd[0];
  entity.paletteTrailStartSlot = firstSlot({&trailStart, &legacyTrail}, entity.paletteStartSlot);
  entity.paletteTrailEndSlot =
      trailEnd.empty() ? (legacyTrail.size() > 1 ? legacyTrail[1] : entity.paletteTrailStartSlot)
                       : trailEnd[0];

  if (const json::Value* colorValue = render.find("color")) {
    const std::vector<double> components = colorValue->numberArray();
    if (components.size() >= 3) {
      entity.color = Vec4{static_cast<float>(clampValue(components[0], 0.0, 1.0)),
                          static_cast<float>(clampValue(components[1], 0.0, 1.0)),
                          static_cast<float>(clampValue(components[2], 0.0, 1.0)),
                          static_cast<float>(clampValue(components.size() > 3 ? components[3] : 1.0,
                                                        0.0, 1.0))};
    } else {
      entity.color = fallbackColor;
    }
  } else {
    entity.color = fallbackColor;
  }

  const json::Value* physics = value.find("physics");
  const float mass = static_cast<float>(Expression::evaluate(json::member(physics, "mass"), inputs, 1));
  const float inertia =
      static_cast<float>(Expression::evaluate(json::member(physics, "inertia"), inputs, 0.985));
  const float drag = static_cast<float>(Expression::evaluate(json::member(physics, "drag"), inputs, 0));

  entity.lifetime = std::max(0.0f, lifetime);
  entity.size = clampValue(size, 0.003f, 0.32f);
  entity.energySize = clampValue(energySize, 0.0f, 0.32f);
  entity.beatSize = clampValue(beatSize, 0.0f, 0.32f);
  entity.flareThreshold = clampValue(flareThreshold, 0.0f, 2.0f);
  entity.flareSize = clampValue(flareSize, 0.0f, 0.32f);
  entity.flareGlow = clampValue(flareGlow, 0.0f, 12.0f);
  entity.glow = clampValue(glow, 0.0f, 3.0f);
  entity.trailLength = clampValue(trailLength, 0.0f, 32.0f);
  entity.primitive = primitiveCode(primitiveName);
  entity.material = materialCode(materialName);
  entity.inverseMass = 1.0f / std::max(0.001f, mass);
  entity.inertia = clampValue(inertia, 0.0f, 1.0f);
  entity.drag = std::max(0.0f, drag);
  return entity;
}

void Simulation::instantiateEntity(const json::Value& value, const Vec3& position, Random& random,
                                   int depth) {
  if (depth > 4 || !module_) return;
  if (static_cast<int>(entities_.size()) >= module_->resources().maxParticles) return;

  const json::Value resolved = resolveTemplate(value);
  if (resolved.find("render") != nullptr || resolved.find("sprite") != nullptr ||
      resolved.find("mesh") != nullptr || resolved.find("trail") != nullptr) {
    entities_.push_back(styledEntity(resolved, position, random));
  }

  if (const json::Value* emitter = resolved.find("emitter")) {
    const json::Value* templateId = emitter->find("template");
    if (templateId != nullptr && emitters_.size() < 256) {
      const ExpressionInputs inputs = expressionInputs(static_cast<double>(random.next()));
      const json::Value* burstValue = emitter->find("burst");
      if (burstValue == nullptr) burstValue = emitter->find("count");
      const int burst = clampValue(
          static_cast<int>(std::lround(Expression::evaluate(burstValue, inputs, 1))), 1, 512);
      const json::Value* maxValue = emitter->find("maxParticles");
      const int maximum = clampValue(
          static_cast<int>(maxValue != nullptr ? maxValue->numberOr(burst * 32) : burst * 32), 1,
          module_->resources().maxParticles);
      emitters_.push_back(Emitter{templateId->stringOr(""), position, burst, maximum, 0});
    }
  }

  if (const json::Value* children = resolved.find("children")) {
    if (const json::Array* items = children->array()) {
      const size_t limit = std::min<size_t>(items->size(), 16);
      for (size_t index = 0; index < limit; ++index) {
        instantiateEntity((*items)[index], position, random, depth + 1);
      }
    }
  }
}

void Simulation::rebuild() {
  entities_.clear();
  entityLive_.clear();
  fields_.clear();
  fieldWaves_.clear();
  effectQueue_.clear();
  emitters_.clear();
  liveFlareSettings_.clear();
  lastBeatIndex_ = -2147483648L;
  ++structureSerial_;
  if (!module_) return;

  const Module& module = *module_;
  const int maximum = std::min(module.resources().maxInteractiveFieldEntities, 16384);

  Random random;
  random.state = signal_.trackSeed ^ moduleSeed(module.id());
  random.multiplier = 6364136223846793005ULL;
  random.increment = 1;

  const Vec3 boundsMin = module.minimum();
  const Vec3 boundsMax = module.maximum();
  const bool is3D = module.is3D();

  for (const json::Value& sceneValue : module.scene()) {
    const json::Value resolvedScene = resolveTemplate(sceneValue);
    const json::Value* field = resolvedScene.find("field");
    if (field == nullptr || field->object() == nullptr) {
      instantiateEntity(resolvedScene, Vec3{}, random, 0);
      continue;
    }

    const int requested =
        static_cast<int>(field->find("count") != nullptr ? field->find("count")->numberOr(0) : 0);
    std::vector<double> resolution;
    if (const json::Value* value = field->find("resolution")) resolution = value->numberArray();

    const int baseColumns = std::max(
        1, !resolution.empty() ? static_cast<int>(resolution[0])
                               : static_cast<int>(std::sqrt(static_cast<double>(std::max(1, requested)))));
    const int baseRows = std::max(
        1, resolution.size() > 1 ? static_cast<int>(resolution[1])
                                 : std::max(1, requested / baseColumns));
    const int baseDepth = std::max(1, (is3D && resolution.size() > 2) ? static_cast<int>(resolution[2]) : 1);

    const std::string layout =
        field->find("layout") != nullptr ? field->find("layout")->stringOr("grid") : "grid";
    const ExpressionInputs inputs = expressionInputs(0.5);
    const float density = static_cast<float>(Expression::evaluate(field->find("density"), inputs, 1));
    const float normalizedDensity = clampValue(density, 0.05f, 1.0f);
    const float axisScale = is3D ? std::pow(normalizedDensity, 1.0f / 3.0f) : std::sqrt(normalizedDensity);

    // Counts at the *reference* extent -- the footprint the module author
    // declared as `resolution` x `spacing`. These set the gap; whether the grid
    // is then allocated at that size or at the full bounds is decided below.
    const int referenceColumns =
        layout == "grid" ? std::max(1, static_cast<int>(std::lround(baseColumns * axisScale))) : baseColumns;
    const int referenceRows =
        layout == "grid" ? std::max(1, static_cast<int>(std::lround(baseRows * axisScale))) : baseRows;
    const int depth =
        layout == "grid" ? std::max(1, static_cast<int>(std::lround(baseDepth * axisScale))) : baseDepth;

    std::vector<double> declaredSpacing;
    if (const json::Value* value = field->find("spacing")) declaredSpacing = value->numberArray();
    const bool usesDensity = field->find("density") != nullptr;

    auto resolvedSpacing = [&](size_t index, int baseCount,
                               int scaled) -> std::optional<float> {
      if (index >= declaredSpacing.size()) return std::nullopt;
      const float declared = static_cast<float>(declaredSpacing[index]);
      if (declared <= 0) return std::nullopt;
      if (!usesDensity || baseCount <= 1 || scaled <= 1) return declared;
      // Density changes entity count, not the visual footprint. Preserve the
      // module author's original grid extent instead of stretching the field to
      // the module's full bounds.
      return declared * static_cast<float>(baseCount - 1) / static_cast<float>(scaled - 1);
    };

    const std::optional<float> spacingX = resolvedSpacing(0, baseColumns, referenceColumns);
    const std::optional<float> spacingY = resolvedSpacing(1, baseRows, referenceRows);
    const std::optional<float> spacingZ = resolvedSpacing(2, baseDepth, depth);
    const Vec3 center = (boundsMin + boundsMax) * 0.5f;

    // A field that declares `extentX`/`extentY` sizes itself from the gap
    // rather than the other way round: the gap is whatever complexity made it,
    // and the grid is allocated to fill the module bounds at that gap. The live
    // extent then gates a centred sub-rectangle every tick (`applyFieldExtents`)
    // so a driver can sweep it without a structural rebuild.
    //
    // Fields that declare neither keep the original sizing exactly, which is
    // what holds every other module's conformance digest still.
    const json::Value* extentXValue = field->find("extentX");
    const json::Value* extentYValue = field->find("extentY");
    const bool gated = layout == "grid" && (extentXValue != nullptr || extentYValue != nullptr);

    auto spanCount = [](float span, std::optional<float> gap, int fallback) {
      if (!gap || *gap <= 0 || span <= 0) return fallback;
      return std::max(1, static_cast<int>(std::lround(span / *gap)) + 1);
    };

    const int columns = gated ? spanCount(boundsMax.x - boundsMin.x, spacingX, referenceColumns)
                              : referenceColumns;
    const int rows =
        gated ? spanCount(boundsMax.y - boundsMin.y, spacingY, referenceRows) : referenceRows;

    const int requestedCount = requested > 0 ? requested : baseColumns * baseRows * baseDepth;
    const int scaledCount = layout == "grid"
                                ? columns * rows * depth
                                : static_cast<int>(std::lround(requestedCount * normalizedDensity));
    const int count =
        std::min(std::max(1, scaledCount), maximum - static_cast<int>(entities_.size()));
    if (count <= 0) break;

    const size_t start = entities_.size();

    json::Value fieldTemplate = resolvedScene;
    if (const json::Value* templateId = field->find("template")) {
      if (const json::Value* templateValue = module.templateFor(templateId->stringOr(""))) {
        fieldTemplate = resolveTemplate(*templateValue);
      }
    }

    for (int localIndex = 0; localIndex < count; ++localIndex) {
      Vec3 position;
      if (layout == "radial") {
        const float angle = static_cast<float>(localIndex) / static_cast<float>(std::max(1, count)) * kPi * 2;
        const float radius = is3D ? 0.35f + random.next() * 0.55f : 0.72f;
        position = Vec3{std::cos(angle) * radius, std::sin(angle) * radius,
                        is3D ? (random.next() - 0.5f) * 1.2f : 0.0f};
      } else if (layout == "random" || layout == "volume") {
        position = Vec3{lerp(boundsMin.x, boundsMax.x, random.next()),
                        lerp(boundsMin.y, boundsMax.y, random.next()),
                        is3D ? lerp(boundsMin.z, boundsMax.z, random.next()) : 0.0f};
      } else if (layout == "line") {
        const float t = static_cast<float>(localIndex) / static_cast<float>(std::max(1, count - 1));
        position = Vec3{lerp(boundsMin.x, boundsMax.x, t), 0,
                        is3D ? lerp(boundsMin.z, boundsMax.z, t) : 0.0f};
      } else {
        const int x = localIndex % columns;
        const int y = (localIndex / columns) % rows;
        const int z = (localIndex / std::max(1, columns * rows)) % depth;
        const float px =
            spacingX ? center.x + (static_cast<float>(x) - (columns - 1) * 0.5f) * *spacingX
                     : lerp(boundsMin.x, boundsMax.x,
                            columns > 1 ? static_cast<float>(x) / (columns - 1) : 0.5f);
        const float py =
            spacingY ? center.y + (static_cast<float>(y) - (rows - 1) * 0.5f) * *spacingY
                     : lerp(boundsMin.y, boundsMax.y,
                            rows > 1 ? static_cast<float>(y) / (rows - 1) : 0.5f);
        float pz = 0;
        if (is3D) {
          pz = spacingZ ? center.z + (static_cast<float>(z) - (depth - 1) * 0.5f) * *spacingZ
                        : lerp(boundsMin.z, boundsMax.z,
                               depth > 1 ? static_cast<float>(z) / (depth - 1) : 0.5f);
        }
        position = Vec3{px, py, pz};
      }
      entities_.push_back(styledEntity(fieldTemplate, position, random));
    }

    const float fallbackSpacingX =
        columns > 1 ? (boundsMax.x - boundsMin.x) / static_cast<float>(columns - 1) : 1.0f;
    const float fallbackSpacingY =
        rows > 1 ? (boundsMax.y - boundsMin.y) / static_cast<float>(rows - 1) : fallbackSpacingX;

    auto slotSource = [field](std::string_view key) {
      const json::Value* value = field->find(key);
      return value != nullptr ? value->exprSource() : std::string();
    };
    const std::vector<std::string> legacyLine = paletteSlots(slotSource("wireframeColor"));
    const std::vector<std::string> lineStart = paletteSlots(slotSource("wireframeColorStart"));
    const std::vector<std::string> lineEnd = paletteSlots(slotSource("wireframeColorEnd"));

    FieldRange range;
    range.begin = start;
    range.end = entities_.size();
    range.columns = columns;
    range.rows = rows;
    range.depth = depth;
    range.topology =
        field->find("topology") != nullptr ? field->find("topology")->stringOr("grid") : "grid";
    range.spacing = std::max(0.0001f, std::min(spacingX.value_or(fallbackSpacingX),
                                               spacingY.value_or(fallbackSpacingY)));
    if (const json::Value* wireframe = field->find("wireframe")) range.wireframe = *wireframe;
    range.lineStartSlot = firstSlot({&lineStart, &legacyLine}, "linePrimary");
    range.lineEndSlot = lineEnd.empty()
                            ? (legacyLine.size() > 1 ? legacyLine[1] : range.lineStartSlot)
                            : lineEnd[0];
    range.radialBeatWave = radialBeatWave(resolvedScene);
    range.gated = gated;
    if (extentXValue != nullptr) range.extentX = *extentXValue;
    if (extentYValue != nullptr) range.extentY = *extentYValue;
    // Full extent until the first `applyFieldExtents`, so a field is never
    // momentarily empty between the rebuild and the tick that sizes it.
    range.liveColumns = columns;
    range.liveRows = rows;
    fields_.push_back(std::move(range));

    if (fields_.back().radialBeatWave.present) {
      for (size_t index = fields_.back().begin; index < fields_.back().end; ++index) {
        entities_[index].usesAnalyticWave = true;
      }
    }
  }

  if (entities_.empty()) {
    const int count = std::min(768, maximum);
    for (int index = 0; index < count; ++index) {
      const float angle = static_cast<float>(index) / static_cast<float>(count) * kPi * 2;
      const float radius = 0.22f + 0.7f * random.next();
      const float z = is3D ? (random.next() - 0.5f) * 1.4f : 0.0f;
      Entity entity;
      entity.position = Vec3{std::cos(angle) * radius, std::sin(angle) * radius, z};
      entity.origin = entity.position;
      entity.phase = random.next();
      entities_.push_back(entity);
    }
    FieldRange range;
    range.begin = 0;
    range.end = entities_.size();
    range.columns = static_cast<int>(entities_.size());
    range.rows = 1;
    range.depth = 1;
    range.topology = "nearest";
    range.spacing = 1;
    range.liveColumns = range.columns;
    range.liveRows = range.rows;
    range.lineStartSlot = "primary";
    range.lineEndSlot = "primary";
    fields_.push_back(std::move(range));
  }

  // Discover which module settings are wired directly to per-entity render
  // fields, so they can be applied live without a structural rebuild. The tvOS
  // engine hard-codes this for `particle-ripples`; driving it from `affects`
  // generalises the same behaviour to every module.
  //
  // One expression is excluded: a live binding flattens every entity to a single
  // value, while `styledEntity` evaluates the same expression per entity with a
  // per-entity `random`. An expression reading `random` therefore has to stay
  // baked, or applying it live would quietly erase the per-entity variation the
  // module authored.
  auto liveBindingIsSafe = [&module](const std::string& path) {
    // `templates.<name>.render.<key>` is the only shape these paths take.
    constexpr size_t kPrefix = sizeof("templates.") - 1;
    if (path.compare(0, kPrefix, "templates.") != 0) return true;
    const size_t nameEnd = path.find('.', kPrefix);
    if (nameEnd == std::string::npos) return true;
    const json::Value* entry = module.templateFor(path.substr(kPrefix, nameEnd - kPrefix));
    if (entry == nullptr) return true;
    const json::Value* render = entry->find("render");
    if (render == nullptr) return true;
    const json::Value* target = render->find(path.substr(path.rfind('.') + 1));
    if (target == nullptr) return true;
    return target->exprSource().find("random") == std::string::npos;
  };
  for (const ModuleSetting& setting : module.settings()) {
    for (const std::string& path : setting.affects) {
      if (!liveBindingIsSafe(path)) continue;
      if (path.find(".render.dotSizePixels") != std::string::npos) {
        liveFlareSettings_.emplace_back(LiveField::DotSizePixels, setting.id);
      } else if (path.find(".render.flareThreshold") != std::string::npos) {
        liveFlareSettings_.emplace_back(LiveField::FlareThreshold, setting.id);
      } else if (path.find(".render.flareGlow") != std::string::npos) {
        liveFlareSettings_.emplace_back(LiveField::FlareGlow, setting.id);
      } else if (path.find(".render.trailLength") != std::string::npos) {
        liveFlareSettings_.emplace_back(LiveField::TrailLength, setting.id);
      } else if (path.find(".render.flareSize") != std::string::npos) {
        liveFlareSettings_.emplace_back(LiveField::FlareSize, setting.id);
      } else if (path.find(".render.glow") != std::string::npos) {
        liveFlareSettings_.emplace_back(LiveField::Glow, setting.id);
      }
    }
  }

  visitedTokens_.assign(entities_.size(), 0);
  entityLive_.assign(entities_.size(), 1);
  effectQueue_.reserve(8192);
}

void Simulation::applyFieldExtents() {
  bool anyGated = false;
  for (const FieldRange& field : fields_) {
    if (field.gated) {
      anyGated = true;
      break;
    }
  }
  if (!anyGated) return;

  const ExpressionInputs inputs = expressionInputs(0.5);
  // An allocated grid of N cells spans N-1 gaps across the module bounds, so an
  // extent of `fraction` is that many gaps of it. Working in cells rather than
  // in world units keeps this exact at every complexity: the gap never has to
  // be divided back out.
  auto liveCount = [](const json::Value& extent, const ExpressionInputs& in, int allocated) {
    const double fraction = clampValue(Expression::evaluate(&extent, in, 1.0), 0.0, 1.0);
    const int desired = std::max(
        1, static_cast<int>(std::lround(fraction * static_cast<double>(allocated - 1))) + 1);
    // Snap to the allocated count's parity. The live rectangle is centred, so an
    // odd difference would sit it half a gap off centre -- and a driver sweeping
    // the extent would make the whole grid shimmer sideways as it grew.
    const int margin = std::max(0, (allocated - std::min(desired, allocated)) / 2);
    return std::max(1, allocated - margin * 2);
  };

  entityLive_.assign(entities_.size(), 1);
  for (FieldRange& field : fields_) {
    if (!field.gated) continue;
    field.liveColumns =
        field.extentX.isNull() ? field.columns : liveCount(field.extentX, inputs, field.columns);
    field.liveRows =
        field.extentY.isNull() ? field.rows : liveCount(field.extentY, inputs, field.rows);
    field.columnBegin = (field.columns - field.liveColumns) / 2;
    field.rowBegin = (field.rows - field.liveRows) / 2;

    for (size_t index = field.begin; index < field.end && index < entityLive_.size(); ++index) {
      const int local = static_cast<int>(index - field.begin);
      const int x = local % field.columns;
      const int y = (local / field.columns) % field.rows;
      if (field.cellIsLive(x, y)) continue;
      entityLive_[index] = 0;
      // Park the dead cell rather than leaving it wherever the last live tick
      // left it. It is not integrated while dead, so without this it would
      // reappear mid-ripple when the extent grows back over it.
      Entity& entity = entities_[index];
      entity.position = entity.origin;
      entity.velocity = Vec3{};
      entity.energy = 0;
      entity.waveEnergy = 0;
      entity.waveTarget = 0;
      entity.waveOffset = Vec3{};
    }
  }
}

void Simulation::emitBeatParticles() {
  if (emitters_.empty() || !module_) return;
  if (static_cast<int>(entities_.size()) >= module_->resources().maxParticles) return;

  Random random;
  random.state = signal_.trackSeed ^ static_cast<uint64_t>(static_cast<int64_t>(signal_.beatIndex)) ^
                 effectSequence_;
  random.multiplier = 2862933555777941757ULL;
  random.increment = 3037000493ULL;

  const bool is3D = module_->is3D();
  const size_t initialEmitterCount = emitters_.size();
  for (size_t index = 0; index < initialEmitterCount; ++index) {
    Emitter emitter = emitters_[index];
    const int available =
        std::min(emitter.maximum - emitter.emitted,
                 module_->resources().maxParticles - static_cast<int>(entities_.size()));
    const int count = std::min(emitter.burst, std::max(0, available));
    if (count <= 0) continue;
    const json::Value* templateValue = module_->templateFor(emitter.templateId);
    if (templateValue == nullptr) continue;

    for (int i = 0; i < count; ++i) {
      const size_t start = entities_.size();
      instantiateEntity(*templateValue, emitter.origin, random, 1);
      if (entities_.size() > start) {
        const float azimuth = random.next() * kPi * 2;
        const float elevation = is3D ? (random.next() - 0.5f) * kPi : 0.0f;
        const float speed = 0.004f + random.next() * 0.018f;
        entities_[start].velocity =
            Vec3{std::cos(azimuth) * std::cos(elevation) * speed,
                 std::sin(azimuth) * std::cos(elevation) * speed,
                 is3D ? std::sin(elevation) * speed : 0.0f};
      }
    }
    emitter.emitted += count;
    emitters_[index] = emitter;
  }

  if (visitedTokens_.size() < entities_.size()) visitedTokens_.resize(entities_.size(), 0);
}

void Simulation::enqueueRootEffects() {
  ++effectSequence_;
  const uint64_t token =
      (static_cast<uint64_t>(static_cast<int64_t>(signal_.beatIndex)) * 0x9e3779b97f4a7c15ULL) ^
      effectSequence_;

  double intensitySetting = 1;
  if (auto it = settings_.find("intensity"); it != settings_.end()) {
    intensitySetting = it->second;
  } else if (auto density = settings_.find("density"); density != settings_.end()) {
    intensitySetting = density->second;
  }
  const float intensity = static_cast<float>(intensitySetting);
  const float strength =
      static_cast<float>(0.18 + signal_.beatPulse * (0.45 + signal_.energy * 0.55)) * intensity;
  const ExpressionInputs inputs = expressionInputs(0.5);

  for (size_t fieldIndex = 0; fieldIndex < fields_.size(); ++fieldIndex) {
    const FieldRange& field = fields_[fieldIndex];
    const size_t center = field.begin + field.count() / 2;
    if (!field.radialBeatWave.present) {
      effectQueue_.push_back(EffectDelivery{center, center, token, 0, strength});
      continue;
    }

    const FieldWaveSource& source = field.radialBeatWave;
    auto value = [&](const json::Value& node, double fallback) {
      return static_cast<float>(
          Expression::evaluate(node.isNull() ? nullptr : &node, inputs, fallback));
    };
    const float waveStrength = value(source.strength, strength);
    const float speed = value(source.speed, 0.45);
    const float falloff = value(source.falloff, 0.94);
    const float wavelength = value(source.wavelength, field.spacing * 3);
    const float steepness = value(source.steepness, 0.85);
    const float anticipation = value(source.anticipation, 0.42);
    const float attack = value(source.attack, 0.04);
    const float release = value(source.release, 0.55);
    const float flashPower = value(source.flashPower, 4.5);

    // Both of these are measured over the live cells only. A gated field is
    // allocated at its full size, and taking the allocated corner here would
    // size every ripple to a lattice most of which is not on screen.
    Vec3 centerPosition;
    size_t live = 0;
    for (size_t index = field.begin; index < field.end; ++index) {
      if (!entityLive(index)) continue;
      centerPosition += entities_[index].origin;
      ++live;
    }
    centerPosition = centerPosition / static_cast<float>(std::max<size_t>(1, live));

    float maximumRadius = 0;
    for (size_t index = field.begin; index < field.end; ++index) {
      if (!entityLive(index)) continue;
      maximumRadius = std::max(maximumRadius, distance(entities_[index].origin, centerPosition));
    }

    FieldWave wave;
    wave.fieldIndex = fieldIndex;
    wave.center = centerPosition;
    wave.speed = std::max(0.01f, speed);
    wave.falloff = clampValue(falloff, 0.0f, 1.0f);
    wave.strength = std::max(0.0f, waveStrength);
    wave.wavelength = std::max(field.spacing, wavelength);
    wave.steepness = clampValue(steepness, 0.0f, 1.0f);
    wave.anticipation = std::max(0.02f, anticipation);
    wave.attack = std::max(0.01f, attack);
    wave.release = std::max(0.02f, release);
    wave.flashPower = clampValue(flashPower, 0.5f, 12.0f);
    wave.maximumRadius = maximumRadius;
    fieldWaves_.push_back(wave);

    if (fieldWaves_.size() > 16) {
      fieldWaves_.erase(fieldWaves_.begin(),
                        fieldWaves_.begin() + static_cast<long>(fieldWaves_.size() - 16));
    }
  }
}

void Simulation::advanceFieldWaves(float dt) {
  float offsetMagnifier = 1;
  if (auto it = settings_.find("offset_magnifier"); it != settings_.end()) {
    offsetMagnifier = clampValue(static_cast<float>(it->second), 0.0f, 50.0f);
  }

  for (Entity& entity : entities_) {
    if (!entity.usesAnalyticWave) continue;
    entity.waveOffset = Vec3{};
    entity.waveTarget = 0;
  }

  if (!fieldWaves_.empty()) {
    for (FieldWave& wave : fieldWaves_) {
      if (wave.fieldIndex >= fields_.size()) continue;
      const FieldRange& field = fields_[wave.fieldIndex];
      const float nextRadius = wave.radius + wave.speed * dt;

      for (size_t entityIndex = field.begin; entityIndex < field.end; ++entityIndex) {
        if (!entityLive(entityIndex)) continue;
        Entity& entity = entities_[entityIndex];
        const Vec3 radial = entity.origin - wave.center;
        const float dist = length(radial);
        const float distanceToFront = dist - nextRadius;
        const float anticipationWidth =
            std::max(wave.wavelength * 0.5f, wave.speed * wave.anticipation);
        if (distanceToFront < -(wave.speed * dt) || distanceToFront > anticipationWidth) continue;

        const float gridSteps = dist / field.spacing;
        const float deliveredStrength = wave.strength * std::pow(wave.falloff, gridSteps);
        if (deliveredStrength <= 0.005f) continue;

        const float leadingProgress = clampValue(distanceToFront / anticipationWidth, 0.0f, 1.0f);
        const float approach = 1 - leadingProgress;
        const float anticipation = 0.22f * std::pow(approach, 1.15f);
        const float flash = 0.78f * std::pow(approach, wave.flashPower);
        const float poweredDrive = std::pow(clampValue(deliveredStrength, 0.0f, 1.0f), 1.15f);
        const float glowSpike = poweredDrive * (anticipation + flash);

        entity.waveTarget = std::min(1.0f, entity.waveTarget + glowSpike);
        entity.waveAttack = wave.attack;
        entity.waveRelease = wave.release;

        if (dist > 0.000001f) {
          const Vec3 direction = radial / dist;
          const float phase = leadingProgress * (kPi / 2);
          const float crest = std::cos(phase) * std::pow(approach, 2.0f);
          const float horizontalAmplitude =
              std::min(field.spacing * 0.45f,
                       wave.steepness * deliveredStrength * field.spacing * 0.7f);
          const Vec3 gerstner = direction * (horizontalAmplitude * crest * offsetMagnifier);
          entity.waveOffset += Vec3{gerstner.x, gerstner.y, 0};
        }
      }
      wave.radius = nextRadius;
    }

    fieldWaves_.erase(
        std::remove_if(fieldWaves_.begin(), fieldWaves_.end(),
                       [](const FieldWave& wave) {
                         return wave.radius > wave.maximumRadius +
                                                  std::max(wave.wavelength * 0.5f,
                                                           wave.speed * wave.anticipation);
                       }),
        fieldWaves_.end());
  }

  for (Entity& entity : entities_) {
    if (!entity.usesAnalyticWave) continue;
    const float target = entity.waveTarget;
    const float responseRate =
        target > entity.waveEnergy
            ? (2.3f / entity.waveAttack) * (0.45f + 0.55f * std::pow(target, 2.0f))
            : 2.3f / entity.waveRelease;
    const float response = 1 - std::exp(-responseRate * dt);
    entity.waveEnergy += (target - entity.waveEnergy) * response;
  }
}

void Simulation::processEffects(Diagnostics& diagnostics) {
  const double started = now();
  size_t cursor = 0;
  while (cursor < effectQueue_.size() && diagnostics.propagationDeliveries < 8192) {
    // Spec section 7: a hard 4 ms CPU budget per tick. Unprocessed deliveries
    // are dropped at tick end, never queued into the next frame.
    if (now() - started >= 0.004) break;
    const EffectDelivery delivery = effectQueue_[cursor];
    ++cursor;
    if (delivery.receiver >= entities_.size()) continue;
    if (visitedTokens_[delivery.receiver] == delivery.token) {
      ++diagnostics.roundTrips;
      continue;
    }
    visitedTokens_[delivery.receiver] = delivery.token;
    ++diagnostics.propagationDeliveries;

    Entity& entity = entities_[delivery.receiver];
    entity.energy = std::min(2.0f, entity.energy + delivery.strength);
    const Vec3 direction = normalize(entity.position + Vec3{0.0001f, 0.0001f, 0.0001f});
    entity.velocity += direction * (delivery.strength * 0.045f * entity.inverseMass);

    if (delivery.hop >= 8 || delivery.strength <= 0.025f) continue;
    for (size_t neighbour : neighbours(delivery.receiver)) {
      effectQueue_.push_back(EffectDelivery{neighbour, delivery.receiver, delivery.token,
                                            delivery.hop + 1, delivery.strength * 0.78f});
    }
  }
  if (cursor < effectQueue_.size()) {
    diagnostics.droppedEffects = static_cast<int>(effectQueue_.size() - cursor);
  }
  effectQueue_.clear();
}

std::vector<size_t> Simulation::neighbours(size_t index) const {
  const FieldRange* field = nullptr;
  for (const FieldRange& candidate : fields_) {
    if (candidate.contains(index)) {
      field = &candidate;
      break;
    }
  }
  if (field == nullptr) return {};
  if (field->topology == "none") return {};

  const size_t local = index - field->begin;
  if (field->topology == "nearest" || field->topology == "radius" || field->rows == 1) {
    const size_t before = local > 0 ? index - 1 : field->end - 1;
    const size_t after = local + 1 < field->count() ? index + 1 : field->begin;
    return {before, after};
  }

  // A dead cell is not part of the lattice: it neither conducts an effect nor
  // receives one, so a ripple stops at the live boundary instead of crossing
  // the gap and re-emerging on the far side.
  if (!entityLive(index)) return {};

  const int columns = field->columns;
  const int rows = field->rows;
  const int depth = field->depth;
  const int x = static_cast<int>(local) % columns;
  const int y = (static_cast<int>(local) / columns) % rows;
  const int z = static_cast<int>(local) / std::max(1, columns * rows);

  std::vector<size_t> result;
  result.reserve(6);
  auto append = [&](int nx, int ny, int nz) {
    if (nx < 0 || nx >= columns || ny < 0 || ny >= rows || nz < 0 || nz >= depth) return;
    if (!field->cellIsLive(nx, ny)) return;
    const size_t neighbour =
        field->begin + static_cast<size_t>(nz * columns * rows + ny * columns + nx);
    if (field->contains(neighbour)) result.push_back(neighbour);
  };
  append(x - 1, y, z);
  append(x + 1, y, z);
  append(x, y - 1, z);
  append(x, y + 1, z);
  if (depth > 1) {
    append(x, y, z - 1);
    append(x, y, z + 1);
  }
  return result;
}

void Simulation::integrate(float dt) {
  const Vec3 minimum = module_->minimum();
  const Vec3 maximum = module_->maximum();
  const std::string& boundaryMode = module_->boundary().mode;
  const float restitution = static_cast<float>(module_->boundary().restitution);
  const bool is3D = module_->is3D();
  const float time = static_cast<float>(signal_.time);

  for (size_t index = 0; index < entities_.size(); ++index) {
    if (!entityLive(index)) continue;
    Entity& entity = entities_[index];
    entity.age += dt;
    if (entity.lifetime > 0 && entity.age >= entity.lifetime) {
      entity.age = 0;
      entity.position = entity.origin;
      entity.energy = std::max(entity.energy, static_cast<float>(signal_.beatPulse));
    }

    if (entity.usesAnalyticWave) {
      entity.energy = entity.waveEnergy;
      entity.position = entity.origin + entity.waveOffset;
      entity.velocity = Vec3{};
      continue;
    }

    entity.energy *= std::pow(0.28f, dt);
    const Vec3 spring = (entity.origin - entity.position) * 0.45f;
    const Vec3 wobble = Vec3{std::sin(time * 0.6f + entity.phase * 6.28f),
                             std::cos(time * 0.5f + entity.phase * 4.31f),
                             is3D ? std::sin(time * 0.4f + entity.phase * 5.17f) : 0.0f} *
                        (0.003f + entity.energy * 0.008f);
    entity.velocity += (spring + wobble) * (dt * entity.inverseMass);
    // Frame-rate independent momentum retention: the module declares a factor
    // per nominal 60 Hz frame, so it is raised to the actual elapsed frames.
    const float retainedMomentum =
        std::pow(entity.inertia, dt * 60.0f) * std::exp(-entity.drag * dt);
    entity.velocity *= retainedMomentum;
    entity.position += entity.velocity;
    applyBounds(index, minimum, maximum, boundaryMode, restitution, is3D);
  }
}

void Simulation::applyBounds(size_t index, const Vec3& minimum, const Vec3& maximum,
                             const std::string& mode, float restitution, bool is3D) {
  Entity& entity = entities_[index];
  const int axes = is3D ? 3 : 2;
  for (int axis = 0; axis < axes; ++axis) {
    if (entity.position[axis] >= minimum[axis] && entity.position[axis] <= maximum[axis]) continue;
    if (mode == "wrap") {
      entity.position[axis] = entity.position[axis] < minimum[axis] ? maximum[axis] : minimum[axis];
    } else if (mode == "slide" || mode == "clamp") {
      entity.position[axis] = clampValue(entity.position[axis], minimum[axis], maximum[axis]);
      entity.velocity[axis] = 0;
    } else if (mode == "respawn" || mode == "despawn") {
      entity.position = entity.origin;
      entity.velocity = Vec3{};
      entity.energy = 0;
    } else {
      entity.position[axis] = clampValue(entity.position[axis], minimum[axis], maximum[axis]);
      entity.velocity[axis] *= -restitution;
    }
  }
}

SnapshotPtr Simulation::publish(Diagnostics diagnostics, double elapsedMilliseconds) {
  diagnostics.simulationMilliseconds = elapsedMilliseconds;

  auto snapshot = std::make_shared<SceneSnapshot>();
  snapshot->particles.reserve(entities_.size() * 2);
  // Zeroed rather than merely sized: a cell outside the live extent is skipped
  // below and never writes an entry, and a wire must not pick up last frame's
  // size for it.
  publishedSize_.assign(entities_.size(), 0.0f);

  const bool moduleUsesPalette = module_ != nullptr;
  for (size_t entityIndex = 0; entityIndex < entities_.size(); ++entityIndex) {
    // Cells outside a gated field's live extent are not drawn at all. They are
    // still allocated, which is what lets the extent be driven without a
    // rebuild, but they contribute nothing to the picture or the draw count.
    if (!entityLive(entityIndex)) continue;
    const Entity& entity = entities_[entityIndex];
    const float energy = clampValue(entity.energy, 0.0f, 1.0f);
    const float linearFlare =
        entity.flareThreshold < 1
            ? clampValue((energy - entity.flareThreshold) / (1 - entity.flareThreshold), 0.0f, 1.0f)
            : 0.0f;
    const float flare = linearFlare * linearFlare * (3 - 2 * linearFlare);

    // Entities whose colours came from palette expressions follow the live
    // theme; literal-colour entities keep their declared colour.
    Vec4 colorStart = entity.color;
    Vec4 colorEnd = entity.color;
    if (entity.usesThemePalette && moduleUsesPalette) {
      colorStart = palette_.color(entity.paletteStartSlot);
      colorEnd = palette_.color(entity.paletteEndSlot);
    }
    const Vec4 glowStart = palette_.color(entity.paletteGlowStartSlot);
    const Vec4 glowEnd = palette_.color(entity.paletteGlowEndSlot);

    const float size = entity.size + energy * entity.energySize +
                       static_cast<float>(signal_.beatPulse) * entity.beatSize +
                       flare * entity.flareSize;
    // Kept for the grid-wire pass below, which needs both of a wire's endpoints
    // at the size they were actually drawn at.
    publishedSize_[entityIndex] = size;
    const float glow = entity.glow + energy + flare * entity.flareGlow;

    RenderParticle particle;
    particle.positionSize = Vec4{entity.position.x, entity.position.y, entity.position.z, size};
    particle.color = colorStart;
    particle.colorEnd = colorEnd;
    particle.glowColor = glowStart;
    particle.glowColorEnd = glowEnd;
    particle.meta = Vec4{glow, entity.primitive, entity.material, 0};
    particle.trail = Vec4{0, 0, 0, 0};
    snapshot->particles.push_back(particle);

    const Vec3 trailDirection = entity.position - entity.origin;
    if (entity.trailLength > 0 && linearFlare > 0 && lengthSquared(trailDirection) > 0.00000001f) {
      const Vec4 trailStart = palette_.color(entity.paletteTrailStartSlot);
      const Vec4 trailEnd = palette_.color(entity.paletteTrailEndSlot);
      RenderParticle trail;
      trail.positionSize = Vec4{entity.position.x, entity.position.y, entity.position.z, size};
      trail.color = trailStart;
      trail.colorEnd = trailEnd;
      trail.glowColor = trailStart;
      trail.glowColorEnd = trailEnd;
      trail.meta = Vec4{glow, 5, entity.material, 0};
      trail.trail = Vec4{trailDirection.x, trailDirection.y, trailDirection.z, entity.trailLength};
      snapshot->particles.push_back(trail);
    }
  }

  const ExpressionInputs inputs = expressionInputs(0.5);
  for (const FieldRange& field : fields_) {
    if (field.topology != "grid") continue;
    if (Expression::evaluate(field.wireframe.isNull() ? nullptr : &field.wireframe, inputs, 0) < 0.5) {
      continue;
    }
    if (field.columns <= 0 || field.rows <= 0) continue;

    const Vec4 lineStart = palette_.color(field.lineStartSlot);
    const Vec4 lineEnd = palette_.color(field.lineEndSlot);
    const size_t layerSize = static_cast<size_t>(field.columns) * static_cast<size_t>(field.rows);
    // Half-width at one end of a wire, from the size that end's dot was drawn
    // at -- the composed size, so a wire thickens where a ripple is passing
    // rather than sitting at the field's uniform base size. The floor is what
    // keeps a hairline lattice when the dots themselves are at zero.
    auto endWidth = [this](size_t index) {
      return std::max(0.0006f, publishedSize_[index] * 0.18f);
    };
    auto appendLine = [&](size_t from, size_t to) {
      if (!field.contains(from) || !field.contains(to)) return;
      const Entity& destination = entities_[to];
      RenderParticle line;
      // Two widths, because a wire tapers along its length between the two dots
      // it connects: `positionSize.w` is the destination end, `meta.w` the
      // source end, and the vertex shader interpolates between them on the same
      // `progress` it already walks the line with.
      line.positionSize = Vec4{destination.position.x, destination.position.y,
                               destination.position.z, endWidth(to)};
      line.color = lineStart;
      line.colorEnd = lineEnd;
      line.glowColor = lineStart;
      line.glowColorEnd = lineEnd;
      line.meta = Vec4{0, 6, 0, endWidth(from)};
      const Vec3 delta = destination.position - entities_[from].position;
      line.trail = Vec4{delta.x, delta.y, delta.z, 1};
      snapshot->particles.push_back(line);
    };

    // Wires span the live sub-rectangle only, so the lattice ends cleanly at the
    // boundary instead of trailing lines out into cells that are not drawn.
    const int columnEnd = field.columnBegin + field.liveColumns;
    const int rowEnd = field.rowBegin + field.liveRows;
    for (int z = 0; z < field.depth; ++z) {
      for (int y = field.rowBegin; y < rowEnd; ++y) {
        for (int x = field.columnBegin; x < columnEnd; ++x) {
          const size_t index = field.begin + static_cast<size_t>(z) * layerSize +
                               static_cast<size_t>(y) * static_cast<size_t>(field.columns) +
                               static_cast<size_t>(x);
          if (x + 1 < columnEnd) appendLine(index, index + 1);
          if (y + 1 < rowEnd) appendLine(index, index + static_cast<size_t>(field.columns));
        }
      }
    }
  }

  ++serial_;
  snapshot->serial = serial_;
  snapshot->structureSerial = structureSerial_;
  snapshot->simulationTime = simulationTime_;
  const Vec4 backgroundPrimary = palette_.background();
  const Vec4 backgroundSecondary = palette_.color("backgroundSecondary", backgroundPrimary);
  snapshot->background =
      mix(backgroundPrimary, backgroundSecondary,
          static_cast<float>(clampValue(signal_.energy * 0.35, 0.0, 0.35)));
  snapshot->fluidBackground = backgroundPrimary;
  snapshot->fluidAccent = palette_.color("backgroundSecondary", palette_.accent());
  // The original Phonoscope maps backgroundSecondary to BOTH fluid blob
  // channels. Dot/glow slots belong exclusively to the particle pass.
  snapshot->fluidHighlight = snapshot->fluidAccent;
  if (auto speed = settings_.find("fluid_speed"); speed != settings_.end()) {
    snapshot->fluidSpeed = static_cast<float>(speed->second);
  }
  snapshot->message = message_;
  snapshot->centreImage = centreImage_;
  snapshot->centreImageFrom = centreImagePrev_;
  snapshot->centreImageFade = static_cast<float>(centreImageFade_);
  snapshot->centreTransition = centreTransitionLatched_;
  snapshot->centreImageHeight = static_cast<float>(centreHeight_);
  snapshot->centreImageWidth = static_cast<float>(centreWidth_);
  snapshot->centreImageFit = centreFit_;
  snapshot->centreImageProportional = centreProportional_;
  snapshot->messageScale = static_cast<float>(messageScale_);
  snapshot->messageColor = palette_.color("primaryText", palette_.highlight());
  snapshot->glowBlurAmount = static_cast<float>(glowBlurAmount_);
  snapshot->glowOpacity = static_cast<float>(glowOpacity_);
  snapshot->glowOverdrive = static_cast<float>(glowOverdrive_);
  snapshot->glowClamped = glowClamped_;
  snapshot->glowBlendMode = glowBlendMode_;
  snapshot->backgroundHeight = static_cast<float>(backgroundHeight_);
  snapshot->backgroundWidth = static_cast<float>(backgroundWidth_);
  snapshot->backgroundScale = static_cast<float>(backgroundScale_);
  snapshot->backgroundFit = backgroundFit_;
  snapshot->backgroundProportional = backgroundProportional_;
  snapshot->backgroundImage = backgroundImage_;
  snapshot->backgroundImageFrom = backgroundImagePrev_;
  snapshot->backgroundImageFade = static_cast<float>(backgroundImageFade_);
  snapshot->backgroundTransition = backgroundTransitionLatched_;
  // Black when the theme declares no `vignette` slot, which is the colour the
  // edge gradients were authored with -- so a theme from before this slot
  // existed frames the band exactly as it always did.
  snapshot->vignetteColor = palette_.color("vignette", Vec4{0, 0, 0, 1});
  snapshot->vignetteOpacity = static_cast<float>(vignetteOpacity_);
  snapshot->vignetteSize = static_cast<float>(vignetteSize_);
  snapshot->sceneBlendMode = sceneBlendMode_;
  snapshot->boundsMinimum = module_ ? module_->minimum() : Vec3{-1.7778f, -1, 0};
  snapshot->boundsMaximum = module_ ? module_->maximum() : Vec3{1.7778f, 1, 0};
  snapshot->is3D = module_ ? module_->is3D() : false;
  snapshot->signal = signal_;
  snapshot->diagnostics = diagnostics;
  return snapshot;
}

}  // namespace nova
