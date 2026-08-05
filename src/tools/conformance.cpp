// Cross-engine conformance runner.
//
// The Apple TV keeps its own Metal implementation of the same module spec as a
// permanent fallback, so two independent engines now interpret
// `PHONOSCOPE_MODULE_SPEC.md`. Two implementations drift unless drift is
// detectable. This replays a fixed input trace through the C++ engine and emits
// per-tick state digests; `ParitySelfTests.swift` replays the identical trace on
// tvOS and must produce the same digests.
//
//   nova-visualiser-conformance --corpus tests/conformance [--update] [--case name]
//
// `--update` rewrites each case's `expected.json`. Only do that when a spec
// change is intended, and update the tvOS side in the same commit.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/composite_reference.h"
#include "core/effect_scale.h"
#include "core/glow_overlay_reference.h"
#include "core/json.h"
#include "core/simulation.h"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// FNV-1a over the raw particle bytes, quantised first so that a
// last-significant-bit difference between compilers does not fail a run while a
// genuine behavioural change still does. 1e-4 is far below anything visible at
// 4K yet far above float noise.
uint64_t digest(const std::vector<nova::RenderParticle>& particles) {
  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&hash](float value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0f));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) * 1099511628211ULL;
    }
  };
  for (const nova::RenderParticle& particle : particles) {
    const float* fields = reinterpret_cast<const float*>(&particle);
    for (size_t index = 0; index < sizeof(nova::RenderParticle) / sizeof(float); ++index) {
      mix(fields[index]);
    }
  }
  return hash;
}

struct CaseResult {
  std::string name;
  bool ok = false;
  std::string detail;
};

nova::Palette paletteFrom(const nova::json::Value* value) {
  nova::Palette palette = nova::Palette::defaults();
  if (value == nullptr) return palette;
  const nova::json::Object* object = value->object();
  if (object == nullptr) return palette;
  for (const auto& [slot, colour] : *object) {
    const std::vector<double> rgb = colour.numberArray();
    if (rgb.size() < 3) continue;
    palette.set(slot, nova::Vec4{static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                                 static_cast<float>(rgb[2]),
                                 static_cast<float>(rgb.size() > 3 ? rgb[3] : 1.0)});
  }
  return palette;
}

// Writes `produced` to expected.json under --update, otherwise compares.
CaseResult finishCase(CaseResult result, const fs::path& directory, const std::string& produced,
                      bool update) {
  const fs::path expectedPath = directory / "expected.json";
  if (update) {
    std::ofstream out(expectedPath, std::ios::binary);
    out << produced << "\n";
    result.ok = true;
    result.detail = "updated";
    return result;
  }

  const std::string expected = readFile(expectedPath);
  std::string trimmed;
  for (char c : expected) {
    if (c != '\n' && c != '\r' && c != ' ') trimmed.push_back(c);
  }
  if (trimmed.empty()) {
    result.detail = "expected.json missing -- run with --update to record a baseline";
    return result;
  }
  result.ok = trimmed == produced;
  if (!result.ok) result.detail = "expected " + trimmed + "\n           actual   " + produced;
  return result;
}

// Composite-formula case. NOT a render test -- it evaluates
// `compositeReference()` over a fixed grid of scene/bloom/background tuples and
// digests the result. `ParitySelfTests.testCompositeParity()` evaluates the
// identical grid on tvOS.
//
// This exists because the rest of the corpus digests particle state only, which
// left every shared shader formula untested. A grid is enough: the composite is
// a pure per-pixel function, so agreeing on a spread of representative inputs
// is agreeing everywhere.
CaseResult runCompositeCase(const fs::path& directory, bool update, CaseResult result,
                            const nova::json::Value& caseValue) {
  const float intensity = static_cast<float>(
      caseValue.find("intensity") != nullptr ? caseValue.find("intensity")->numberOr(1.45) : 1.45);

  // Deliberately spans the cases that used to disagree: zero coverage with a
  // bright glow (the halo that erased the background), full coverage, partial
  // coverage, an opaque background, a fully transparent one (tvOS letterbox),
  // and HDR bloom values well past 1.0.
  const float coverages[] = {0.0f, 0.25f, 0.5f, 1.0f};
  const float glows[] = {0.0f, 0.4f, 1.6f, 6.0f};
  const float backgroundAlphas[] = {0.0f, 0.5f, 1.0f};

  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&hash](float value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0f));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) * 1099511628211ULL;
    }
  };

  int samples = 0;
  for (float coverage : coverages) {
    for (float glow : glows) {
      for (float backgroundAlpha : backgroundAlphas) {
        nova::CompositeInput input;
        // Premultiplied, as the scene pass emits it.
        input.scene = {0.9f * coverage, 0.35f * coverage, 0.15f * coverage, coverage};
        input.bloom = {glow, glow * 0.6f, glow * 0.25f, glow};
        input.background = {0.16f, 0.11f, 0.28f, backgroundAlpha};
        input.intensity = intensity;
        const nova::Vec4 out = nova::compositeReference(input);
        mix(out.x);
        mix(out.y);
        mix(out.z);
        mix(out.w);
        ++samples;
      }
    }
  }

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "[\"composite:%d:%016llx\"]", samples,
                static_cast<unsigned long long>(hash));
  return finishCase(std::move(result), directory, buffer, update);
}

// Resolution adaptation is deliberately asymmetric: soft effects grow at 4K,
// while the dot core and wire width do not. Both shader engines consume this
// contract, and the tvOS self-test computes the identical digest.
CaseResult runEffectScaleCase(const fs::path& directory, bool update, CaseResult result,
                              const nova::json::Value& caseValue) {
  const auto number = [&caseValue](const char* name, double fallback) {
    const nova::json::Value* value = caseValue.find(name);
    return static_cast<float>(value != nullptr ? value->numberOr(fallback) : fallback);
  };
  std::vector<double> heights = caseValue.find("heights") != nullptr
                                    ? caseValue.find("heights")->numberArray()
                                    : std::vector<double>{1080, 2160};

  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&hash](float value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0f));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) *
             1099511628211ULL;
    }
  };

  int samples = 0;
  for (double height : heights) {
    const nova::EffectDimensions dimensions = nova::resolveEffectDimensions(
        static_cast<float>(height), number("dotCore", 0.012), number("wireWidth", 0.002),
        number("haloRadius", 0.03), number("trailLength", 25.5),
        number("trailWidth", 0.012), number("bloomRadius", 1),
        number("backgroundFeatureRadius", 0.48));
    mix(dimensions.scale);
    mix(dimensions.dotCore);
    mix(dimensions.wireWidth);
    mix(dimensions.haloRadius);
    mix(dimensions.trailLength);
    mix(dimensions.trailWidth);
    mix(dimensions.bloomRadius);
    mix(dimensions.backgroundFeatureRadius);
    ++samples;
  }

  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "[\"effect-scale:%d:%016llx\"]", samples,
                static_cast<unsigned long long>(hash));
  return finishCase(std::move(result), directory, buffer, update);
}

// Glow-overlay case. Like the composite case this is a formula test, covering
// the two halves of the pass that can drift independently: the blur's tap
// geometry (sigma mapping, quarter-resolution divisor, weights) and the blend
// modes.
CaseResult runGlowOverlayCase(const fs::path& directory, bool update, CaseResult result,
                              const nova::json::Value& caseValue) {
  std::vector<double> heights = caseValue.find("heights") != nullptr
                                    ? caseValue.find("heights")->numberArray()
                                    : std::vector<double>{1080, 2160};
  std::vector<double> blurAmounts = caseValue.find("blurAmounts") != nullptr
                                        ? caseValue.find("blurAmounts")->numberArray()
                                        : std::vector<double>{0, 1, 7.5, 20};
  std::vector<double> blendValues = caseValue.find("blendValues") != nullptr
                                        ? caseValue.find("blendValues")->numberArray()
                                        : std::vector<double>{0,   0.25, 0.4999, 0.5,
                                                              0.75, 1,   1.4999, 1.5, 2};

  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&hash](float value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0f));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) * 1099511628211ULL;
    }
  };

  int samples = 0;
  for (double height : heights) {
    for (double blur : blurAmounts) {
      mix(nova::glowBlurSigmaTexels(static_cast<float>(blur), static_cast<float>(height)));
      ++samples;
    }
  }
  for (int tap = 0; tap <= nova::kGlowBlurTapRadius; ++tap) {
    mix(nova::glowBlurTapWeight(tap));
    ++samples;
  }

  // Spans an unlit background, a mid tone, a fully lit pixel, and an HDR
  // highlight past 1.0 -- the case the clamp exists for. Opacity 0 must be the
  // identity in both modes.
  const float bases[] = {0.0f, 0.35f, 1.0f, 3.0f};
  const float glows[] = {0.0f, 0.4f, 1.0f, 2.5f};
  const float opacities[] = {0.0f, 0.5f, 1.0f};
  // Ordered so the two original modes keep contributing in their original
  // order; overlay is appended, exactly as it is on the axis.
  const nova::GlowBlendMode modes[] = {nova::GlowBlendMode::Multiply,
                                       nova::GlowBlendMode::Screen,
                                       nova::GlowBlendMode::Overlay};
  for (nova::GlowBlendMode mode : modes) {
    for (float base : bases) {
      for (float glow : glows) {
        for (float opacity : opacities) {
          nova::GlowOverlayInput input;
          input.base = {base, base * 0.6f, base * 0.25f, 0.75f};
          input.glow = {glow, glow * 0.5f, glow * 0.9f, 0.4f};
          input.opacity = opacity;
          input.mode = mode;
          const nova::Vec4 out = nova::glowOverlayReference(input);
          mix(out.x);
          mix(out.y);
          mix(out.z);
          mix(out.w);
          ++samples;
        }
      }
    }
  }

  // The driven blend mode. A driver hands the overlay a continuous number, so
  // what has to match across engines is which mode each number snaps to --
  // including the values either side of every cut.
  for (double blend : blendValues) {
    mix(static_cast<float>(static_cast<int>(nova::glowBlendModeFor(static_cast<float>(blend)))));
    ++samples;
  }

  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "[\"glow-overlay:%d:%016llx\"]", samples,
                static_cast<unsigned long long>(hash));
  return finishCase(std::move(result), directory, buffer, update);
}

CaseResult runCase(const fs::path& directory, bool update) {
  CaseResult result;
  result.name = directory.filename().string();

  const std::string caseText = readFile(directory / "case.json");
  auto caseValue = nova::json::Value::parse(caseText);
  if (!caseValue) {
    result.detail = "case.json missing or unparseable";
    return result;
  }

  if (caseValue->find("type") != nullptr && caseValue->find("type")->stringOr("") == "composite") {
    return runCompositeCase(directory, update, std::move(result), *caseValue);
  }
  if (caseValue->find("type") != nullptr && caseValue->find("type")->stringOr("") == "effect-scale") {
    return runEffectScaleCase(directory, update, std::move(result), *caseValue);
  }
  if (caseValue->find("type") != nullptr &&
      caseValue->find("type")->stringOr("") == "glow-overlay") {
    return runGlowOverlayCase(directory, update, std::move(result), *caseValue);
  }

  const std::string moduleName =
      caseValue->find("module") != nullptr ? caseValue->find("module")->stringOr("module.json")
                                           : "module.json";
  auto module = nova::Module::parse(readFile(directory / moduleName));
  if (!module) {
    result.detail = "compiled module " + moduleName + " missing or unparseable";
    return result;
  }

  nova::SimulationInput input;
  input.module = std::make_shared<const nova::Module>(*module);
  input.palette = paletteFrom(caseValue->find("palette"));
  input.transitionDuration =
      caseValue->find("transitionMs") != nullptr
          ? caseValue->find("transitionMs")->numberOr(600) / 1000.0
          : 0.0;
  // Conformance runs with transitions settled so a case measures simulation
  // behaviour, not the theme chase.
  input.transitionPaused = true;

  if (const nova::json::Value* settings = caseValue->find("settings")) {
    if (const nova::json::Object* object = settings->object()) {
      for (const auto& [key, value] : *object) input.settings[key] = value.numberOr(0);
    }
  } else {
    for (const nova::ModuleSetting& setting : module->settings()) {
      input.settings[setting.id] = setting.defaultValue;
    }
  }

  // A case can trace the NOT-playing path. The idle branch of `buildSignal` is
  // mirrored by hand in tvOS `PhonoscopeStore.makeSignal`, and without a case
  // driving it the two engines could drift on idle behaviour while every
  // playing case still passed.
  const bool casePlaying = caseValue->find("playing") == nullptr ||
                           caseValue->find("playing")->boolean().value_or(true);

  nova::SignalFrame signal = nova::SignalFrame::idle();
  signal.playing = casePlaying;
  signal.bpm = caseValue->find("bpm") != nullptr ? caseValue->find("bpm")->numberOr(120) : 120;
  signal.duration = 240;
  signal.energy = caseValue->find("energy") != nullptr ? caseValue->find("energy")->numberOr(0.6) : 0.6;
  signal.trackSeed = static_cast<uint64_t>(
      caseValue->find("trackSeed") != nullptr ? caseValue->find("trackSeed")->numberOr(20260803)
                                              : 20260803);
  signal.spectrum = nova::syntheticSpectrum(0, 0, 0, signal.energy, signal.trackSeed);
  input.signal = signal;

  std::vector<int> sampleTicks;
  if (const nova::json::Value* samples = caseValue->find("sampleTicks")) {
    for (double tick : samples->numberArray()) sampleTicks.push_back(static_cast<int>(tick));
  }
  if (sampleTicks.empty()) sampleTicks = {1, 30, 120, 360};
  const int totalTicks = sampleTicks.back();

  nova::Simulation simulation;
  simulation.submit(input);

  std::vector<std::string> digests;
  int lyricIndex = -1;
  for (int tick = 1; tick <= totalTicks; ++tick) {
    // The trace drives the clock explicitly rather than using wall time, which
    // is what makes the run reproducible on both engines.
    const double position = tick * nova::kSimulationStep;
    nova::SignalFrame frame = nova::buildSignal(position, casePlaying, nullptr, nullptr,
                                                nova::kSimulationStep, lyricIndex, 0);
    if (!casePlaying) {
      // Take the inert frame exactly as built. The overrides below exist to pin
      // beat state to the case BPM, which is meaningless with no playback and
      // would smuggle a synthetic beat back into the trace.
      input.signal = frame;
      simulation.submit(input);
      nova::SnapshotPtr idleSnapshot = simulation.step();
      if (std::find(sampleTicks.begin(), sampleTicks.end(), tick) != sampleTicks.end()) {
        char idleBuffer[64];
        std::snprintf(idleBuffer, sizeof(idleBuffer), "%d:%zu:%016llx", tick,
                      idleSnapshot->particles.size(),
                      static_cast<unsigned long long>(digest(idleSnapshot->particles)));
        digests.emplace_back(idleBuffer);
      }
      continue;
    }
    frame.bpm = signal.bpm;
    frame.trackSeed = signal.trackSeed;
    frame.energy = signal.energy;
    // Recompute beat state against the case BPM so a case is not at the mercy
    // of the ambient-seed fallback tempo.
    const double beatLength = 60 / std::max(20.0, frame.bpm);
    const double beatValue = position / beatLength;
    frame.beatIndex = static_cast<int>(std::floor(beatValue));
    frame.beatPhase = beatValue - std::floor(beatValue);
    frame.beatPulse = std::pow(std::max(0.0, 1 - frame.beatPhase), 5);
    frame.time = position;
    frame.spectrum = nova::syntheticSpectrum(position, frame.beatPhase, frame.beatPulse,
                                             frame.energy, frame.trackSeed);
    input.signal = frame;
    simulation.submit(input);

    nova::SnapshotPtr snapshot = simulation.step();
    if (std::find(sampleTicks.begin(), sampleTicks.end(), tick) == sampleTicks.end()) continue;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%d:%zu:%016llx", tick, snapshot->particles.size(),
                  static_cast<unsigned long long>(digest(snapshot->particles)));
    digests.emplace_back(buffer);
  }

  std::string produced = "[";
  for (size_t index = 0; index < digests.size(); ++index) {
    if (index > 0) produced += ",";
    produced += "\"" + digests[index] + "\"";
  }
  produced += "]";

  return finishCase(std::move(result), directory, produced, update);
}

}  // namespace

int main(int argc, char** argv) {
  fs::path corpus = "tests/conformance";
  bool update = false;
  std::string only;
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    if (flag == "--corpus" && index + 1 < argc) corpus = argv[++index];
    if (flag == "--case" && index + 1 < argc) only = argv[++index];
    if (flag == "--update") update = true;
  }

  if (!fs::exists(corpus)) {
    std::cerr << "conformance corpus not found: " << corpus << "\n";
    return 2;
  }

  std::vector<CaseResult> results;
  for (const fs::directory_entry& entry : fs::directory_iterator(corpus)) {
    if (!entry.is_directory()) continue;
    if (!only.empty() && entry.path().filename().string() != only) continue;
    results.push_back(runCase(entry.path(), update));
  }

  int failed = 0;
  for (const CaseResult& result : results) {
    if (result.ok) {
      std::printf("  ok    %s%s\n", result.name.c_str(),
                  result.detail.empty() ? "" : (" (" + result.detail + ")").c_str());
    } else {
      std::printf("  FAIL  %s\n           %s\n", result.name.c_str(), result.detail.c_str());
      ++failed;
    }
  }
  if (results.empty()) {
    std::printf("  no conformance cases found in %s\n", corpus.string().c_str());
    return 2;
  }
  std::printf("\n%zu case(s), %d failed\n", results.size(), failed);
  return failed == 0 ? 0 : 1;
}
