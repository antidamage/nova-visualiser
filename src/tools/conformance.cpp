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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/background_band_reference.h"
#include "core/centre_image_reference.h"
#include "core/composite_reference.h"
#include "core/effect_scale.h"
#include "core/glow_overlay_reference.h"
#include "core/json.h"
#include "core/parameter_drivers.h"
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

// Backdrop-band-formula case. NOT a render test -- it evaluates
// `backgroundBandReference()` over a grid of sample points and band geometries
// and digests the result. `ParitySelfTests.testBackgroundBandParity()` evaluates
// the identical grid on tvOS.
//
// The band used to be a hardcoded 1/3 constant here and a SwiftUI frame there,
// with the vignette as five magic numbers in each shader. Now that all four of
// height, width, vignette opacity and vignette size are driven, the layout is
// the thing most likely to drift, so the layout is what gets locked. The sample
// points deliberately straddle every boundary: the band's clip edge, the corners
// where two edge gradients overlap, the centre where none reach, and the region
// outside the band that must come out as solid vignette colour.
CaseResult runBackgroundBandCase(const fs::path& directory, bool update, CaseResult result,
                                 const nova::json::Value& caseValue) {
  auto numbers = [&caseValue](const char* name, std::vector<double> fallback) {
    const nova::json::Value* value = caseValue.find(name);
    if (value == nullptr) return fallback;
    std::vector<double> parsed = value->numberArray();
    return parsed.empty() ? fallback : parsed;
  };

  const std::vector<double> heights = numbers("heightFractions", {0.0, 1.0 / 3.0, 0.5, 1.0});
  const std::vector<double> widths = numbers("widthFractions", {0.25, 0.6, 1.0});
  const std::vector<double> opacities = numbers("vignetteOpacities", {0.0, 0.5, 0.96, 1.0});
  const std::vector<double> sizes = numbers("vignetteSizes", {0.0, 1.0, 2.5});
  // Straddles the band edge at the default 1/3 height (0.3333 and 0.6667), both
  // corners, and the centre.
  const std::vector<double> us = numbers("samplesU", {0.0, 0.09, 0.2, 0.5, 0.91, 1.0});
  const std::vector<double> vs = numbers("samplesV", {0.0, 0.3333, 0.4, 0.5, 0.6667, 1.0});

  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&hash](float value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0f));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) * 1099511628211ULL;
    }
  };

  int samples = 0;
  for (double height : heights) {
    for (double width : widths) {
      for (double opacity : opacities) {
        for (double size : sizes) {
          for (double u : us) {
            for (double v : vs) {
              nova::BackgroundBandInput input;
              input.u = static_cast<float>(u);
              input.v = static_cast<float>(v);
              input.heightFraction = static_cast<float>(height);
              input.widthFraction = static_cast<float>(width);
              // The band's own pixel size at 4K, which is what the renderer
              // passes and what sets the softness of the clip edge.
              input.bandPixelWidth = static_cast<float>(3840.0 * width);
              input.bandPixelHeight = static_cast<float>(2160.0 * height);
              // Not black, so a mistake that collapses the vignette to a plain
              // darken is distinguishable from one that tints correctly.
              input.vignetteColor = {0.06f, 0.02f, 0.14f};
              input.vignetteOpacity = static_cast<float>(opacity);
              input.vignetteSize = static_cast<float>(size);
              input.fieldColor = {0.62f, 0.48f, 0.71f};
              const nova::BackgroundBandOutput out = nova::backgroundBandReference(input);
              mix(out.inBand);
              mix(out.vignetteShade);
              mix(out.color.x);
              mix(out.color.y);
              mix(out.color.z);
              mix(out.color.w);
              ++samples;
            }
          }
        }
      }
    }
  }

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "[\"background-band:%d:%016llx\"]", samples,
                static_cast<unsigned long long>(hash));
  return finishCase(std::move(result), directory, buffer, update);
}

// Centre-image-formula case. NOT a render test -- it evaluates
// `centreImageHalfExtent()` and `centreImageFade()` over a grid of frame and
// image aspects, scales and fade times, and digests the result.
// `ParitySelfTests.testCentreImageParity()` evaluates the identical grid on tvOS.
//
// The centre slot holds either a message or an image, at one place and on one
// scale axis. The image half is where the two engines can silently disagree:
// this one fits and scales in a shader uniform, tvOS in a SwiftUI modifier. The
// contain-fit is the thing that drifts, so the contain-fit is what gets locked.
CaseResult runCentreImageCase(const fs::path& directory, bool update, CaseResult result,
                              const nova::json::Value& caseValue) {
  auto numbers = [&caseValue](const char* key, std::vector<double> fallback) {
    const nova::json::Value* value = caseValue.find(key);
    std::vector<double> parsed = value != nullptr ? value->numberArray() : std::vector<double>{};
    return parsed.empty() ? fallback : parsed;
  };
  // 16:9 and 21:9 frames; images from a tall crest through square to a wide
  // banner, deliberately straddling the frame's own aspect in both directions
  // because that is which branch of the fit is taken.
  const std::vector<double> frameAspects = numbers("frameAspects", {16.0 / 9.0, 21.0 / 9.0});
  const std::vector<double> imageAspects =
      numbers("imageAspects", {0.5, 1.0, 16.0 / 9.0, 2.5, 4.0});
  // Both clamp ends, the identity, and values either side of them.
  const std::vector<double> scales = numbers("scales", {0.0, 0.1, 0.5, 1.0, 2.75, 5.0, 9.0});
  // The base height, as a fraction: nothing, the default third, full frame, and
  // an out-of-range value that must clamp rather than run away.
  const std::vector<double> heights = numbers("heightFractions", {0.0, 0.33, 1.0, 1.5});

  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&hash](float value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0f));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) * 1099511628211ULL;
    }
  };

  int samples = 0;
  for (double frameAspect : frameAspects) {
    for (double imageAspect : imageAspects) {
      for (double height : heights) {
        for (double scale : scales) {
          const nova::CentreImageExtent extent = nova::centreImageHalfExtent(
              static_cast<float>(frameAspect), static_cast<float>(imageAspect),
              static_cast<float>(height), static_cast<float>(scale));
          mix(extent.halfWidth);
          mix(extent.halfHeight);
          ++samples;
        }
      }
    }
  }

  // The cross-fade ramp, including both ends and a zero-length transition --
  // which must resolve to "already there" rather than dividing by zero.
  const std::vector<double> transitions = numbers("fadeTransitions", {0.0, 0.6, 2.5});
  const std::vector<double> elapsed = numbers("fadeElapsed", {0.0, 0.15, 0.6, 1.2, 5.0});
  for (double transition : transitions) {
    for (double seconds : elapsed) {
      mix(nova::centreImageFade(static_cast<float>(seconds), static_cast<float>(transition)));
      ++samples;
    }
  }

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "[\"centre-image:%d:%016llx\"]", samples,
                static_cast<unsigned long long>(hash));
  return finishCase(std::move(result), directory, buffer, update);
}

// Scene-blend-formula case. The four modes the scene layer can meet the backdrop
// with, plus the points where the driven `__sceneBlend` axis snaps from one to
// the next. Same grid as `composite`, run once per mode -- Linear must reproduce
// the composite case's own arithmetic exactly, which is what proves the default
// picture is unchanged.
CaseResult runSceneBlendCase(const fs::path& directory, bool update, CaseResult result,
                             const nova::json::Value& caseValue) {
  const float intensity = static_cast<float>(
      caseValue.find("intensity") != nullptr ? caseValue.find("intensity")->numberOr(1.45) : 1.45);
  std::vector<double> blendValues = caseValue.find("blendValues") != nullptr
                                        ? caseValue.find("blendValues")->numberArray()
                                        : std::vector<double>{};
  if (blendValues.empty()) {
    // Every mode, and every boundary either side of the snap.
    blendValues = {0, 0.4999, 0.5, 1, 1.4999, 1.5, 2, 2.4999, 2.5, 3, 4};
  }

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
  std::string invariant;
  for (double blendValue : blendValues) {
    const nova::SceneBlendMode mode = nova::sceneBlendModeFor(static_cast<float>(blendValue));
    // The resolved mode is digested too, so a change to where the axis snaps
    // fails here rather than silently shifting which look a stored range picks.
    mix(static_cast<float>(static_cast<int>(mode)));
    for (float coverage : coverages) {
      for (float glow : glows) {
        for (float backgroundAlpha : backgroundAlphas) {
          nova::CompositeInput input;
          input.scene = {0.9f * coverage, 0.35f * coverage, 0.15f * coverage, coverage};
          input.bloom = {glow, glow * 0.6f, glow * 0.25f, glow};
          input.background = {0.16f, 0.11f, 0.28f, backgroundAlpha};
          input.intensity = intensity;
          input.blendMode = mode;
          const nova::Vec4 out = nova::compositeReference(input);

          // The scene layer is an alpha mask, not a plate: where it covers
          // nothing there is nothing to blend, so every mode must reduce
          // exactly to Linear. Stated outright rather than left to the digest,
          // because the digest cannot say WHICH property broke -- and the
          // property that broke was this one. Multiply and overlay used to
          // return black here, blanking the backdrop everywhere the lattice
          // was not.
          if (coverage == 0.0f && invariant.empty()) {
            nova::CompositeInput linear = input;
            linear.blendMode = nova::SceneBlendMode::Linear;
            const nova::Vec4 reference = nova::compositeReference(linear);
            const float tolerance = 1e-6f;
            if (std::fabs(out.x - reference.x) > tolerance ||
                std::fabs(out.y - reference.y) > tolerance ||
                std::fabs(out.z - reference.z) > tolerance ||
                std::fabs(out.w - reference.w) > tolerance) {
              invariant = "uncovered scene pixel differs from Linear under blend mode " +
                          std::to_string(static_cast<int>(mode));
            }
          }

          mix(out.x);
          mix(out.y);
          mix(out.z);
          mix(out.w);
          ++samples;
        }
      }
    }
  }

  if (!invariant.empty()) {
    result.detail = invariant;
    return result;
  }

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "[\"scene-blend:%d:%016llx\"]", samples,
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
  // 1 is the identity. 2.5 saturates a mid glow, and 10 is the top of the axis,
  // where every non-black channel clamps -- the case the pre-clamp multiply
  // exists to produce.
  const float overdrives[] = {1.0f, 2.5f, 10.0f};
  // Clamped first, so the original ordering keeps contributing in its original
  // order and the unclamped run is appended.
  const bool clampings[] = {true, false};
  for (nova::GlowBlendMode mode : modes) {
    for (float base : bases) {
      for (float glow : glows) {
        for (float opacity : opacities) {
          for (float overdrive : overdrives) {
            for (bool clamped : clampings) {
              nova::GlowOverlayInput input;
              input.base = {base, base * 0.6f, base * 0.25f, 0.75f};
              input.glow = {glow, glow * 0.5f, glow * 0.9f, 0.4f};
              input.opacity = opacity;
              input.overdrive = overdrive;
              input.clamped = clamped;
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

// Parameter-driver case. Like the composite and glow-overlay cases this is a
// formula test rather than a render test: it steps the lane evaluator through a
// fixed script of frames and digests every resolved value.
//
// The script lives here rather than in case.json because all three engines have
// to walk the identical scenarios, and a grid in code is the same shape as the
// TypeScript reference's test file and the Swift self-test.
// `ParitySelfTests.testParameterDriverParity()` mirrors it exactly; changing
// either means changing both and re-recording expected.json.
CaseResult runParameterDriversCase(const fs::path& directory, bool update, CaseResult result,
                                   const nova::json::Value& caseValue) {
  const double bpm = caseValue.find("bpm") != nullptr ? caseValue.find("bpm")->numberOr(120) : 120;
  const int signature =
      caseValue.find("timeSignature") != nullptr
          ? static_cast<int>(caseValue.find("timeSignature")->numberOr(4))
          : 4;

  uint64_t hash = 1469598103934665603ULL;
  int samples = 0;
  auto mix = [&hash, &samples](double value) {
    const long long quantised = static_cast<long long>(std::llround(value * 10000.0));
    for (int byte = 0; byte < 8; ++byte) {
      hash = (hash ^ static_cast<uint64_t>((quantised >> (byte * 8)) & 0xff)) * 1099511628211ULL;
    }
    ++samples;
  };

  nova::EffectDeclaration glow;
  glow.id = "glow";
  glow.min = 0;
  glow.max = 10;
  glow.step = 0.1;
  glow.defaultValue = 0;
  const std::unordered_map<std::string, nova::EffectDeclaration> declarations{{"glow", glow}};

  auto driver = [](const std::string& type, int every = 1, int offset = 0, double interval = 4,
                   const std::string& cadence = "beat", double transition = 0.5) {
    nova::Driver value;
    value.type = type;
    value.every = every;
    value.offset = offset;
    value.intervalSeconds = interval;
    value.cadence = cadence;
    value.transitionSeconds = transition;
    return value;
  };
  auto binding = [](const std::string& id, double min, double max, double attack, double hold,
                    double release) {
    nova::EffectBinding value;
    value.id = id;
    value.effect = "glow";
    value.hasMin = true;
    value.min = min;
    value.hasMax = true;
    value.max = max;
    value.hasAttack = true;
    value.attackSeconds = attack;
    value.hasHold = true;
    value.holdSeconds = hold;
    value.hasRelease = true;
    value.releaseSeconds = release;
    return value;
  };
  auto frameAt = [bpm, signature](double time, double delta, int beatIndex, int barIndex,
                                  uint64_t trackSeed) {
    nova::SignalFrame frame;
    frame.time = time;
    frame.delta = delta;
    frame.beatIndex = beatIndex;
    frame.barIndex = barIndex;
    frame.bpm = bpm;
    frame.timeSignature = signature;
    frame.energy = 0;
    frame.trackSeed = trackSeed;
    frame.spectrum.fill(0.0f);
    return frame;
  };
  // Steps one lane set through `ticks` frames, mixing the resolved value each
  // tick. `advance` decides how the clock and indices move per tick.
  auto sweep = [&](const std::vector<nova::ScopedLane>& lanes,
                   const std::unordered_map<std::string, nova::CombineMode>& combine, int ticks,
                   const std::function<nova::SignalFrame(int)>& advance) {
    nova::DriverStates states;
    for (int tick = 0; tick < ticks; ++tick) {
      const nova::LaneEvaluation evaluation =
          nova::evaluateDriverLanes(lanes, combine, declarations, advance(tick), states);
      const auto found = evaluation.values.find("glow");
      mix(found == evaluation.values.end() ? -1.0 : found->second);
    }
  };
  auto laneOf = [](const std::string& id, const nova::Driver& primary,
                   const std::vector<nova::EffectBinding>& bindings,
                   const std::vector<nova::Driver>& modifiers = {}) {
    nova::DriverLane lane;
    lane.id = id;
    lane.driver = primary;
    lane.modifiers = modifiers;
    lane.bindings = bindings;
    return lane;
  };

  // 1. The triggered attack/hold/release envelope, sampled straight through
  //    attack, hold, release and back to idle.
  sweep({{"g", laneOf("l", driver("beat"), {binding("b1", 0, 10, 0.1, 0.1, 0.2)})}}, {}, 12,
        [&](int tick) { return frameAt(tick * 0.05, 0.05, 0, 0, 1); });

  // 2. Retrigger part way through a release: the level must resume upward from
  //    where it had fallen to rather than restarting at zero.
  sweep({{"g", laneOf("l", driver("beat"), {binding("b1", 0, 10, 0.1, 0, 1.0)})}}, {}, 8,
        [&](int tick) { return frameAt(tick * 0.05, 0.05, tick >= 3 ? 1 : 0, 0, 1); });

  // 3. `every` and `offset` gating on both beat and downbeat.
  for (const auto& [type, every, offset] :
       std::vector<std::tuple<std::string, int, int>>{{"beat", 2, 0},
                                                      {"beat", 3, 1},
                                                      {"downbeat", 4, 0},
                                                      {"downbeat", 4, 2},
                                                      {"downbeat", 16, 0}}) {
    sweep({{"g", laneOf("l", driver(type, every, offset), {binding("b1", 0, 10, 0, 0, 0)})}}, {}, 17,
          [&](int tick) { return frameAt(tick, 0.05, tick, tick, 1); });
  }

  // 4. Modifier summation, including the deliberate overshoot past the
  //    authored maximum.
  sweep({{"g", laneOf("l", driver("downbeat"), {binding("b1", 0, 10, 0, 1, 0)},
                      {driver("bass"), driver("treble")})}},
        {}, 4, [&](int tick) {
          nova::SignalFrame frame = frameAt(tick * 0.05, 0.05, 0, 0, 1);
          frame.spectrum[0] = 0.5f;
          frame.spectrum[25] = 0.25f;
          return frame;
        });

  // 5. Add versus strongest across a frequent and a rare lane, including the
  //    tick where only the frequent one fires.
  const std::vector<nova::ScopedLane> stacked{
      {"g", laneOf("beat", driver("beat"), {binding("b-beat", 0, 4, 0, 1, 0)})},
      {"g", laneOf("down", driver("downbeat", 4), {binding("b-down", 0, 10, 0, 1, 0)})},
      {"g", laneOf("song", driver("song"), {binding("b-song", 0, 6, 0, 1, 0)})}};
  for (nova::CombineMode mode : {nova::CombineMode::Add, nova::CombineMode::Strongest}) {
    sweep(stacked, {{"glow", mode}}, 10, [&](int tick) {
      return frameAt(tick, 0.05, tick, tick, tick < 5 ? 1 : 2);
    });
  }

  // 6. The overshoot guard: eight lanes that would otherwise reach eight times
  //    the authored range.
  {
    std::vector<nova::ScopedLane> many;
    for (int index = 0; index < 8; ++index) {
      many.push_back({"g", laneOf("l" + std::to_string(index), driver("beat"),
                                  {binding("b" + std::to_string(index), 0, 10, 0, 1, 0)})});
    }
    sweep(many, {{"glow", nova::CombineMode::Add}}, 3,
          [&](int tick) { return frameAt(tick * 0.05, 0.05, 0, 0, 1); });
  }

  // 7. Timer and song pulses, including `every` applied to counted song events.
  sweep({{"g", laneOf("l", driver("timer", 1, 0, 1.0), {binding("b1", 0, 10, 0, 0, 0)})}}, {}, 8,
        [&](int tick) { return frameAt(tick * 0.5, 0.5, 0, 0, 1); });
  sweep({{"g", laneOf("l", driver("song"), {binding("b1", 0, 10, 0, 0, 0)})}}, {}, 6,
        [&](int tick) { return frameAt(tick * 0.5, 0.5, 0, 0, 11 + (tick / 2) * 11); });
  sweep({{"g", laneOf("l", driver("song", 2), {binding("b1", 0, 10, 0, 0, 0)})}}, {}, 6,
        [&](int tick) { return frameAt(tick * 0.5, 0.5, 0, 0, 11 + tick * 11); });

  // 8. Continuous followers, each reading its own bands, with the envelope
  //    acting as a rate limit in both directions.
  for (const std::string& type : {"bass", "mid", "treble", "energy"}) {
    sweep({{"g", laneOf("l", driver(type), {binding("b1", 0, 10, 0.1, 0.05, 0.1)})}}, {}, 8,
          [&](int tick) {
            nova::SignalFrame frame = frameAt(tick * 0.05, 0.05, 0, 0, 1);
            const float level = tick < 4 ? 1.0f : 0.0f;
            frame.spectrum[0] = level;
            frame.spectrum[12] = level;
            frame.spectrum[25] = level;
            frame.energy = level;
            return frame;
          });
  }

  // 9. Seeded random: held between cadence events, glided when a transition is
  //    set, and identical on every engine.
  for (double transition : {0.0, 0.25}) {
    sweep({{"g", laneOf("l", driver("random", 1, 0, 4, "beat", transition),
                        {binding("b1", 0, 10, 0.05, 0, 0.6)})}},
          {}, 8, [&](int tick) { return frameAt(tick * 0.05, 0.05, tick / 3, 0, 1); });
  }

  // 10. Several settings groups on one entry: lanes stack and keep independent
  //     envelopes, while a colliding combine mode layers with the later group
  //     winning.
  {
    nova::SettingsGroup base;
    base.id = "base";
    base.lanes = {laneOf("a", driver("beat"), {binding("b-a", 0, 4, 0, 1, 0)})};
    base.combine["glow"] = nova::CombineMode::Add;
    base.staticSettings["complexity"] = 0.4;
    nova::SettingsGroup hard;
    hard.id = "hard";
    hard.lanes = {laneOf("b", driver("downbeat", 4), {binding("b-b", 0, 10, 0, 1, 0)})};
    hard.combine["glow"] = nova::CombineMode::Strongest;
    hard.staticSettings["complexity"] = 0.9;

    const nova::MergedSettingsGroups merged = nova::mergeSettingsGroups({&base, &hard});
    mix(static_cast<double>(merged.lanes.size()));
    mix(merged.combine.at("glow") == nova::CombineMode::Strongest ? 1.0 : 0.0);
    mix(merged.staticSettings.at("complexity"));
    sweep(merged.lanes, merged.combine, 8,
          [&](int tick) { return frameAt(tick, 0.05, tick, tick, 1); });

    // Reversing the order must flip the scalars and nothing else.
    const nova::MergedSettingsGroups reversed = nova::mergeSettingsGroups({&hard, &base});
    mix(reversed.combine.at("glow") == nova::CombineMode::Strongest ? 1.0 : 0.0);
    mix(reversed.staticSettings.at("complexity"));
  }

  // 11. Rarity ordering, which is what `strongest` resolves ties by.
  {
    const nova::SignalFrame frame = frameAt(0, 0.05, 0, 0, 1);
    for (const nova::Driver& value :
         {driver("beat"), driver("beat", 2), driver("downbeat"), driver("downbeat", 4),
          driver("timer", 1, 0, 30), driver("bass"), driver("random")}) {
      const double period = nova::driverPeriodSeconds(value, frame);
      mix(std::isinf(period) ? -2.0 : period);
    }
    mix(nova::driverPeriodSeconds(driver("song"), frame) ==
                std::numeric_limits<double>::infinity()
            ? 1.0
            : 0.0);
  }

  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "[\"parameter-drivers:%d:%016llx\"]", samples,
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
  if (caseValue->find("type") != nullptr &&
      caseValue->find("type")->stringOr("") == "parameter-drivers") {
    return runParameterDriversCase(directory, update, std::move(result), *caseValue);
  }
  if (caseValue->find("type") != nullptr &&
      caseValue->find("type")->stringOr("") == "background-band") {
    return runBackgroundBandCase(directory, update, std::move(result), *caseValue);
  }
  if (caseValue->find("type") != nullptr &&
      caseValue->find("type")->stringOr("") == "scene-blend") {
    return runSceneBlendCase(directory, update, std::move(result), *caseValue);
  }
  if (caseValue->find("type") != nullptr &&
      caseValue->find("type")->stringOr("") == "centre-image") {
    return runCentreImageCase(directory, update, std::move(result), *caseValue);
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
