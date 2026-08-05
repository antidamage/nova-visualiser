// Reads the shared dashboard configuration and turns it into simulation input.
//
// The renderer is not a second source of truth: module selection, per-module
// settings, colour groups and themes all still live in
// `/api/phonoscope/config`, exactly as the tvOS client read them. What changed
// is who reads them. Realtime pushes arrive on the dashboard's existing SSE bus
// so a slider moved in the browser lands in the render within a frame or two;
// ETag polling stays as the fallback for a dropped stream.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/module.h"
#include "core/palette.h"
#include "core/signal.h"
#include "core/simulation.h"
#include "net/sse_client.h"

namespace nova::net {

struct NowPlaying {
  TrackIdentity identity;
  double position = 0;
  double duration = 0;
  bool playing = false;
  double sampledAtMonotonic = 0;
  bool valid = false;
};

// The dashboard theme's animated-backdrop knobs, already divided by 100 and
// clamped exactly as `FluidBackgroundSettings(shared:)` does on tvOS. Colours
// are 0-1 linear-ish RGB, matching the palette convention used elsewhere here.
//
// This is the one thing the renderer reads that is NOT phonoscope config: the
// backdrop is themed with the dashboard's accent/highlight/background, so the
// visualiser matches the room's colours. Fetching it is best-effort -- see
// `themeAvailable`.
struct FluidThemeSettings {
  bool available = false;
  float peakIntensity = 1.0f;
  float falloffPower = 1.6f;
  float warpAmplitude = 1.0f;
  float hueSpread = 0.5f;
  float apexGlow = 1.0f;
  Vec4 background{0.10f, 0.10f, 0.10f, 1.0f};
  Vec4 accent{0.38f, 0.38f, 0.38f, 1.0f};
  Vec4 highlight{1.0f, 0.0f, 0.73f, 1.0f};
};

// A colour group's rotation, as the dashboard stores it and tvOS plays it.
//
// The renderer used to take `themes[0]` and stop there, which is not a
// simplification -- it is a different picture. A group set to rotate parks the
// visualiser permanently on its first theme, and if that theme happens to be a
// degenerate one (every foreground slot at intensity 0, say) the result is bare
// geometry on a flat background that looks nothing like the television.
struct ColorGroupRotation {
  struct ParameterSource {
    std::string type;
    std::optional<double> value;
    std::optional<double> min;
    std::optional<double> max;
    std::string cadence;
    double intervalSeconds = 4;
    double transitionSeconds = 0.5;
    double attackSeconds = 0.05;
    double holdSeconds = 0;
    double releaseSeconds = 0.6;
  };
  // "interval" advances on a timer, "downbeat" on each bar, anything else holds.
  std::string changeMode;
  // "shuffle" picks a random other theme; anything else advances in order.
  std::string order;
  double waitSeconds = 12;
  double transitionSeconds = 1.5;
  // Fully resolved palettes, in group order: module slot defaults with the
  // theme's own colours applied over them.
  std::vector<Palette> palettes;
  std::vector<std::string> themeIds;
  std::string groupId;
  // The configuration editor publishes its currently selected preview theme.
  // tvOS pins to it; the streamed renderer must not keep rotating underneath.
  std::string pinnedThemeId;
  // Runtime selection is owned by Nova. Iridium and every display consume the
  // same id/revision instead of running separate interval/downbeat clocks.
  std::string selectedThemeId;
  std::string selectedGroupId;
  std::optional<double> selectedTransitionSeconds;
  bool paused = false;
  uint64_t revision = 0;
  // Per-theme setting drivers. The local Metal engine applies these after the
  // module's baseline settings; Iridium must do the same or the selected theme
  // loses its flashes, trails and audio-reactive motion.
  std::vector<std::unordered_map<std::string, ParameterSource>> parameterOverrides;
};

using PhonoscopeParameterSource = ColorGroupRotation::ParameterSource;

struct ConfigSnapshot {
  std::shared_ptr<const Module> module;
  std::unordered_map<std::string, double> settings;
  std::unordered_set<std::string> driverInterpolatedSettings;
  std::unordered_map<std::string, PhonoscopeParameterSource> parameterSources;
  std::string message;
  Palette palette;
  ColorGroupRotation rotation;
  double transitionSeconds = 0.6;
  int reloadGeneration = 0;
  std::string activeModuleId;
  std::string activeModuleVersion;
  // True when the active module declares a setting affecting
  // `renderer.fluidBackground.speed`. Manifest-driven rather than keyed off the
  // module id, so a second module can opt in without touching this service.
  bool usesFluidBackground = false;
  FluidThemeSettings fluidTheme;
  bool valid = false;
};

class ConfigClient {
 public:
  ConfigClient() = default;
  ~ConfigClient();

  // `baseUrl` is the dashboard origin, e.g. http://127.0.0.1.
  void start(std::string baseUrl, double pollSeconds);
  void stop();

  ConfigSnapshot snapshot() const;
  NowPlaying nowPlaying() const;
  TrackAnalysis analysis() const;

  bool streamConnected() const { return sse_.connected(); }
  const std::string& lastError() const { return lastError_; }

  // Forces an immediate refresh, e.g. after a command that changes config.
  void poke();

 private:
  void run(double pollSeconds);
  bool refreshConfiguration();
  void refreshTheme();
  void refreshThemeState();
  void refreshNowPlaying();
  void resolveTrack(const TrackIdentity& identity);
  std::shared_ptr<const Module> loadModule(const std::string& id, const std::string& version,
                                           const std::string& hash);

  std::string baseUrl_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> pending_{true};
  SseClient sse_;

  mutable std::mutex mutex_;
  ConfigSnapshot snapshot_;
  NowPlaying nowPlaying_;
  TrackAnalysis analysis_;
  std::string configEtag_;
  std::string themeEtag_;
  FluidThemeSettings fluidTheme_;
  double lastThemeFetch_ = 0;
  double lastThemeStateFetch_ = 0;
  std::string lastError_;
  std::string loadedModuleKey_;
  std::shared_ptr<const Module> loadedModule_;
  std::string resolvedTrackKey_;
};

}  // namespace nova::net
