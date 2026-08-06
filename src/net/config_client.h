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

#include "core/image.h"
#include "core/module.h"
#include "core/palette.h"
#include "core/parameter_drivers.h"
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
  // One stop on the playlist. A colour theme may appear in several entries with
  // different settings groups, which is why an entry carries its own id: the
  // theme id is no longer unique within a group and cannot key the rotation.
  struct Entry {
    std::string id;
    std::string themeId;
    // Applied in order: their lanes stack, their scalars layer.
    std::vector<std::string> settingsGroupIds;
  };
  // Fully resolved palettes, parallel to `entries`: module slot defaults with
  // the entry's theme colours applied over them.
  std::vector<Palette> palettes;
  // The entry's theme's centre image, parallel to `entries` and null where the
  // theme supplies none. Decoded once and shared, so moving between two entries
  // that name the same image is a pointer comparison and no re-upload.
  std::vector<std::shared_ptr<const DecodedImage>> images;
  std::vector<Entry> entries;
  std::string groupId;
  // The configuration editor publishes the entry it is previewing. tvOS pins to
  // it; the streamed renderer must not keep rotating underneath.
  std::string pinnedEntryId;
  // Runtime selection is owned by Nova. Iridium and every display consume the
  // same ids/revision instead of running separate rotation clocks. The theme
  // and settings ids arrive together so colour and behaviour never tear.
  std::string selectedEntryId;
  std::string selectedThemeId;
  std::string selectedGroupId;
  std::vector<std::string> selectedSettingsGroupIds;
  std::optional<double> selectedTransitionSeconds;
  bool paused = false;
  uint64_t revision = 0;
};

struct ConfigSnapshot {
  std::shared_ptr<const Module> module;
  std::unordered_map<std::string, double> settings;
  std::unordered_set<std::string> driverInterpolatedSettings;
  // Named sets of driver lanes. Which of them apply is decided by the selected
  // entry, so the whole library travels in the snapshot.
  std::vector<SettingsGroup> settingsGroups;
  // The centre of the picture when it is text. A non-blank message overrides
  // whatever image the live colour theme supplies; the image half never comes
  // from here, only from a theme.
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
  std::string lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
  }
  // How many times the configuration has actually been re-read and committed.
  // Exposed because a config that silently stops refreshing is otherwise
  // indistinguishable from one nobody has changed -- the same reason
  // `fluidPhase` is published.
  uint64_t revision() const { return revision_.load(std::memory_order_relaxed); }

  // Forces an immediate refresh, e.g. after a command that changes config.
  void poke();

 private:
  void run(double pollSeconds);
  bool refreshConfiguration();
  // Fetches and decodes a centre image, or returns the cached one. Runs on the
  // config thread; the render thread only ever receives a finished shared_ptr.
  std::shared_ptr<const DecodedImage> centreImage(const std::string& url);
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
  std::atomic<uint64_t> revision_{0};
  SseClient sse_;

  mutable std::mutex mutex_;
  ConfigSnapshot snapshot_;
  NowPlaying nowPlaying_;
  TrackAnalysis analysis_;
  std::string configEtag_;
  // Decoded centre images, keyed by the URL they came from. Every URL carries
  // `?v=<updatedAt>`, so a re-upload is a new key rather than a stale hit, and
  // an entry survives only while something still references it.
  std::unordered_map<std::string, std::shared_ptr<const DecodedImage>> imageCache_;
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
