#include "service/engine.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

#include "core/centre_image_reference.h"
#include "core/effect_scale.h"
#include "core/json.h"
#include "core/picture_effects.h"
#include "net/http_client.h"

namespace nova::service {
namespace {

double monotonicSeconds() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

// Default browser-rung divisor: encode every other frame. See the comment at
// its use site. A debug lease can drop this to 1 for a bounded window.
// Keep MediaMTX's path alive for the web debug view without spending half of
// the single Turing NVENC engine on an unwatched convenience rung. Ten fps is
// enough for discovery/diagnostics; the Apple TV's 4K60 rung always wins.
constexpr int kDefaultPublishFrameDivisor = 6;

// Keyframe requests are coalesced to at most one per this interval. A client on
// bad WiFi can drop to "awaiting IDR" repeatedly, and honouring every request
// would turn the stream into a sequence of IDRs -- which is both enormous and
// exactly the burst pattern that causes the hitching in the first place.
constexpr double kKeyframeRequestInterval = 0.5;

// Rolling window for the frame-interval tail, in seconds.
constexpr double kStatsWindowSeconds = 5.0;

// Number of clients currently reading the visualiser path from MediaMTX.
// Returns 0 on any failure, which is the safe answer: an unreachable sidecar
// must not be able to pin the GPU open.
int mediamtxReaderCount(const std::string& apiUrl) {
  if (apiUrl.empty()) return 0;
  const net::HttpResponse response = net::httpGet(apiUrl + "/v3/paths/get/visualiser", {}, 2);
  if (!response.ok()) return 0;
  auto value = json::Value::parse(response.body);
  if (!value) return 0;
  const json::Value* readers = value->find("readers");
  if (readers == nullptr) return 0;
  const json::Array* items = readers->array();
  return items != nullptr ? static_cast<int>(items->size()) : 0;
}

// Absolute-deadline sleep. Sleeping for a duration accumulates drift; sleeping
// until a deadline keeps the frame cadence honest even when a frame runs long.
void sleepUntil(double deadlineSeconds) {
  timespec deadline{};
  deadline.tv_sec = static_cast<time_t>(deadlineSeconds);
  deadline.tv_nsec = static_cast<long>((deadlineSeconds - std::floor(deadlineSeconds)) * 1e9);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
  }
}

}  // namespace

Engine::Engine(Options options) : options_(std::move(options)) {}

Engine::~Engine() {
  requestStop();
  if (simulationThread_.joinable()) simulationThread_.join();
  if (renderThread_.joinable()) renderThread_.join();
  if (watcherThread_.joinable()) watcherThread_.join();
  stopGpu();
}

void Engine::requestStop() { running_.store(false, std::memory_order_relaxed); }

bool Engine::startGpu(std::string& error) {
  if (gpuReady_.load()) return true;

  if (!device_.initialise(-1, error)) return false;
  if (!device_.makeCurrent()) {
    error = "could not make the EGL context current on the render thread";
    return false;
  }
  if (!renderer_.initialise(options_.width, options_.height, error)) return false;
  if (!interop_.initialise(error)) return false;

  primaryEncoder_ = std::make_unique<encode::Encoder>();
  encode::EncoderSettings primary;
  primary.codec = encode::Codec::Hevc;
  primary.width = options_.width;
  primary.height = options_.height;
  primary.frameRate = options_.frameRate;
  primary.bitRate = options_.bitRate;
  primary.tenBit = options_.tenBit;
  // In-band parameter sets: the Apple TV rung is a raw Annex-B feed, so it has
  // to be self-describing for a client to join a stream already in progress.
  primary.globalHeaderParameterSets = false;
  if (!primaryEncoder_->initialise(interop_.context(), primary, error)) return false;

  primaryOutput_ = renderer_.addOutput(
      options_.width, options_.height,
      options_.tenBit ? gfx::PixelLayout::P010 : gfx::PixelLayout::NV12,
      primaryEncoder_->lumaStrideBytes(), primaryEncoder_->chromaStrideBytes(), error);
  if (primaryOutput_ < 0) return false;
  const gfx::OutputPlane& primaryPlane = renderer_.planeFor(primaryOutput_);
  if (!interop_.registerBuffer(primaryPlane.lumaBuffer, error)) return false;
  if (!interop_.registerBuffer(primaryPlane.chromaBuffer, error)) return false;

  net::StreamHeader header;
  header.codec = "hevc";
  header.profile = options_.tenBit ? "main10" : "main";
  header.width = options_.width;
  header.height = options_.height;
  header.frameRate = options_.frameRate;
  header.parameterSets = primaryEncoder_->parameterSets();
  stream_.updateHeader(header);
#if NOVA_VISUALISER_HAVE_SRT
  srt_.updateHeader(header);
#endif

  // One-shot modes never serve a browser, so they must not open the browser
  // rung. Turing has a single NVENC engine and a handful of sessions; a stray
  // `--dump-frame` left running has already been observed holding two of them
  // plus 600 MB of VRAM on a card the voice stack shares.
  const bool oneShot = !options_.dumpFramePath.empty() || options_.selfTestFrames > 0;
  if (!options_.publishUrl.empty() && !oneShot) {
    publishEncoder_ = std::make_unique<encode::Encoder>();
    encode::EncoderSettings browser;
    browser.codec = encode::Codec::H264;
    browser.width = options_.publishWidth;
    browser.height = options_.publishHeight;
    browser.frameRate = options_.frameRate;
    browser.bitRate = options_.publishBitRate;
    browser.tenBit = false;
    // RTSP carries the parameter sets in the SDP, so this rung needs them in
    // extradata rather than in-band.
    browser.globalHeaderParameterSets = true;
    std::string publishError;
    if (publishEncoder_->initialise(interop_.context(), browser, publishError)) {
      publishOutput_ = renderer_.addOutput(browser.width, browser.height, gfx::PixelLayout::NV12,
                                           publishEncoder_->lumaStrideBytes(),
                                           publishEncoder_->chromaStrideBytes(), publishError);
      if (publishOutput_ >= 0) {
        const gfx::OutputPlane& plane = renderer_.planeFor(publishOutput_);
        interop_.registerBuffer(plane.lumaBuffer, publishError);
        interop_.registerBuffer(plane.chromaBuffer, publishError);
      }
    }
    if (publishOutput_ < 0) {
      // The browser rung is a convenience. Losing it must never take the Apple
      // TV path down with it.
      std::lock_guard<std::mutex> lock(statusMutex_);
      statusError_ = "browser rung unavailable: " + publishError;
      publishEncoder_.reset();
    }
  }

  gpuReady_.store(true);
  return true;
}

void Engine::stopGpu() {
  if (!gpuReady_.exchange(false)) return;
  publisher_.close();
  publishEncoder_.reset();
  primaryEncoder_.reset();
  interop_.shutdown();
  renderer_.shutdown();
  device_.shutdown();
  primaryOutput_ = -1;
  publishOutput_ = -1;
}

SignalFrame projectedSignal(SignalFrame frame, double seconds) {
  frame.delta = kSimulationStep;
  if (!frame.playing || seconds <= 0) return frame;

  frame.time += seconds;
  frame.progress = frame.duration > 0
                       ? std::min(1.0, std::max(0.0, frame.time / frame.duration))
                       : 0;
  const double beatLength = 60 / std::max(20.0, frame.bpm);
  const double beatPosition = frame.beatPhase + seconds / beatLength;
  const int advancedBeats = static_cast<int>(std::floor(beatPosition));
  frame.beatIndex += advancedBeats;
  frame.beatPhase = beatPosition - std::floor(beatPosition);
  frame.beatPulse = std::pow(std::max(0.0, 1 - frame.beatPhase), 5);
  const int signature = std::max(1, frame.timeSignature);
  frame.barIndex = frame.beatIndex / signature;
  frame.barPhase = (static_cast<double>(frame.beatIndex % signature) + frame.beatPhase) /
                   static_cast<double>(signature);
  frame.downbeatPulse = frame.beatIndex % signature == 0 ? frame.beatPulse : 0;
  return frame;
}

double Engine::renderAheadSeconds() const {
  // The master clock is sampled before render/encode. Account for that work,
  // then the selected transport's jitter budget, half an RTT to Iridium, and
  // one display interval for the client-side PTS queue. SRT's TSBPD clock
  // holds packets for the transport budget; AVSampleBufferDisplayLayer then
  // keeps exactly this final frame decoded and presents it on its authored PTS.
  double seconds = std::max(0.0, renderMs_.load(std::memory_order_relaxed) +
                                    encodeMs_.load(std::memory_order_relaxed)) /
                   1000.0;
#if NOVA_VISUALISER_HAVE_SRT
  if (srt_.clientCount() > 0) {
    const net::SrtTransportStats stats = srt_.worstStats();
    seconds += static_cast<double>(srt_.latencyMs()) / 1000.0;
    if (stats.valid) seconds += std::max(0.0, stats.rttMs) / 2000.0;
  } else
#endif
  {
    seconds += stream_.presentationDelaySeconds();
  }
  seconds += 1.0 / std::max(1, options_.frameRate);
  return seconds;
}

void Engine::applyControlLanes(
    const net::ConfigSnapshot& snapshot, const SignalFrame& frame,
    std::unordered_map<std::string, double>& settings,
    std::unordered_set<std::string>& driven) {
  if (!snapshot.module) return;

  // Picture-level effects: household configuration that belongs to the frame
  // rather than to any one module. Their ranges come from
  // `core/picture_effects.h`, which the config client also seeds its settings
  // map from -- one table, because a range declared here for an effect the
  // parse does not know about is a control that resolves to nothing. Mirrors
  // PHONOSCOPE_PICTURE_EFFECTS in the dashboard and the private settings in
  // PhonoscopeStore.swift; the three must agree on every range.
  std::unordered_map<std::string, EffectDeclaration> declarations;
  for (const PictureEffect& effect : kPictureEffects) {
    EffectDeclaration declaration;
    declaration.id = effect.id;
    declaration.min = effect.min;
    declaration.max = effect.max;
    declaration.step = effect.step;
    declaration.defaultValue = effect.defaultValue;
    declarations[effect.id] = declaration;
  }

  for (const ModuleSetting& setting : snapshot.module->settings()) {
    if (setting.updateMode == "structural") continue;
    EffectDeclaration declaration;
    declaration.id = setting.id;
    declaration.min = setting.min;
    declaration.max = setting.max;
    declaration.step = setting.step;
    declaration.defaultValue = setting.defaultValue;
    declarations[setting.id] = declaration;
  }

  // Which settings groups apply is Nova's answer, arriving with the selected
  // entry. Their lanes stack and their scalars layer, last one winning.
  std::vector<const SettingsGroup*> chosen;
  const std::vector<std::string>* ids = &snapshot.rotation.selectedSettingsGroupIds;
  std::vector<std::string> fromEntry;
  if (ids->empty() && entryIndex_ < snapshot.rotation.entries.size()) {
    fromEntry = snapshot.rotation.entries[entryIndex_].settingsGroupIds;
    ids = &fromEntry;
  }
  for (const std::string& id : *ids) {
    for (const SettingsGroup& group : snapshot.settingsGroups) {
      if (group.id == id) {
        chosen.push_back(&group);
        break;
      }
    }
  }
  if (chosen.empty()) {
    // Nothing named anything usable, so fall back to the group everything falls
    // back to rather than dropping every driver on the floor.
    for (const SettingsGroup& group : snapshot.settingsGroups) {
      if (group.isDefault) {
        chosen.push_back(&group);
        break;
      }
    }
  }

  const MergedSettingsGroups merged = mergeSettingsGroups(chosen);
  for (const auto& [id, value] : merged.staticSettings) {
    // Structural values cannot be driven, so they are simply applied.
    settings[id] = value;
  }
  const LaneEvaluation evaluation =
      evaluateDriverLanes(merged.lanes, merged.combine, declarations, frame,
                          parameterDriverStates_);
  for (const auto& [id, value] : evaluation.values) settings[id] = value;
  for (const std::string& id : evaluation.driven) driven.insert(id);
}

void Engine::publishSimulationInput() {
  const net::NowPlaying nowPlaying = config_.nowPlaying();
  const TrackAnalysis analysis = config_.analysis();
  const net::ConfigSnapshot snapshot = config_.snapshot();
  if (!snapshot.valid) return;

  // Render-ahead. Audio plays on the Apple TV while video goes through encode,
  // network and decode, so the frame drawn now must depict the state the room
  // will hear when it is actually presented. Without this every beat lands late.
  const double presentationDelay = renderAheadSeconds();

  static int lyricIndex = -1;
  static double lyricPulse = 0;

  double position = 0;
  bool playing = false;
  const TrackIdentity* identity = nullptr;
  if (nowPlaying.valid) {
    identity = &nowPlaying.identity;
    playing = nowPlaying.playing;
    // Extrapolate between master-clock samples so beat phase stays smooth at
    // 120 Hz even though the uplink only arrives a few times a second.
    position = nowPlaying.position;
    if (playing) position += monotonicSeconds() - nowPlaying.sampledAtMonotonic;
  } else {
    // Idle. The clock keeps running so anything time-based still advances, but
    // this deliberately does NOT claim to be playing. It used to, to stop the
    // visualiser "freezing" -- the effect was that an idle screen synthesised
    // beats and a full spectrum from a free-running clock and animated exactly
    // as hard as a playing one, driving every audio-reactive parameter and a
    // full-rate encode with no audio in the room. Idle motion now comes only
    // from a driver's resting floor.
    position = monotonicSeconds();
    playing = false;
  }
  idlePlayback_.store(!playing, std::memory_order_relaxed);
  position += presentationDelay;

  SignalFrame frame;
  bool usedUpstreamSignal = false;
  {
    std::lock_guard<std::mutex> lock(signalMutex_);
    const double age = monotonicSeconds() - upstreamSignalReceivedAt_;
    if (hasUpstreamSignal_ && age >= 0 && age < 1.0) {
      frame = projectedSignal(upstreamSignal_, age + presentationDelay);
      usedUpstreamSignal = true;
    }
  }
  if (!usedUpstreamSignal) {
    // Do not discard lyrics/energy/valence merely because a provider did not
    // also return BPM or beat timestamps. The Swift engine consumes every
    // partial analysis result, and the streamed engine must do the same.
    const bool hasAnalysis = !analysis.trackKey.empty();
    frame = nova::buildSignal(position, playing, identity, hasAnalysis ? &analysis : nullptr,
                              kSimulationStep, lyricIndex, lyricPulse);
  }
  lyricPulse = frame.lyricPulse;

  SimulationInput input;
  input.module = snapshot.module;
  input.signal = frame;
  input.settings = snapshot.settings;
  input.driverInterpolatedSettings = snapshot.driverInterpolatedSettings;
  input.palette = snapshot.palette;
  input.transitionDuration = snapshot.transitionSeconds;
  // What a module's `dotSizePixels` is counted in. The encoded output's height,
  // not any per-pass target: the glow blur runs at a quarter of this and the
  // bloom at a fraction again, and neither may change how wide a dot is.
  input.outputHeight = static_cast<double>(options_.height);
  input.reloadGeneration = snapshot.reloadGeneration;

  // --- Nova-owned colour-theme selection -----------------------------------
  // Rotation and remote commands are coordinated by the dashboard. Iridium
  // only resolves Nova's selected id to a palette; the Apple TV consumes that
  // same state, so reconnects and independent client clocks cannot diverge.
  const net::ColorGroupRotation& rotation = snapshot.rotation;
  if (!rotation.palettes.empty()) {
    if (entryIndex_ >= rotation.palettes.size()) entryIndex_ = 0;

    const bool selectedGroupMatches = rotation.selectedGroupId.empty() ||
                                      rotation.selectedGroupId == rotation.groupId;
    // The advanced colour editor explicitly pins the entry it is modifying.
    // That preview is authoritative over the normal rotation state; otherwise
    // the renderer keeps showing a different entry throughout the edit and the
    // user has no live view of what they are changing. Clearing the pin on
    // editor close immediately returns to Nova's selected/rotating entry.
    const std::string authoritativeEntryId =
        !rotation.pinnedEntryId.empty()
            ? rotation.pinnedEntryId
            : (selectedGroupMatches ? rotation.selectedEntryId : std::string{});
    if (authoritativeEntryId != currentEntryId_) {
      for (size_t index = 0; index < rotation.entries.size(); ++index) {
        if (rotation.entries[index].id == authoritativeEntryId) {
          entryIndex_ = index;
          currentEntryId_ = authoritativeEntryId;
          break;
        }
      }
    }

    if (entryIndex_ < rotation.entries.size()) {
      currentEntryId_ = rotation.entries[entryIndex_].id;
      // Reported as the theme actually on screen, alt included: this id is what
      // the diagnostics endpoint and House Party telemetry name.
      const net::ColorGroupRotation::Entry& showing = rotation.entries[entryIndex_];
      currentThemeId_ = rotation.altActive && !showing.altThemeId.empty()
                            ? showing.altThemeId
                            : showing.themeId;
    }
    // Nova's household alt state selects the column; the entry selects the row.
    // Both columns are fully resolved at config parse and an entry with no alt
    // has the same palette in both, so this cannot show a hole.
    const bool useAlt = rotation.altActive &&
                        entryIndex_ < rotation.altPalettes.size();
    input.palette = useAlt ? rotation.altPalettes[entryIndex_]
                           : rotation.palettes[entryIndex_];
    // The entry's theme may also supply the picture's centrepiece. Taken from
    // the same index and the same column as the palette, so colour and centre
    // image can never come from different entries or different sides of a flip.
    if (useAlt && entryIndex_ < rotation.altImages.size()) {
      input.themeImage = rotation.altImages[entryIndex_];
    } else if (entryIndex_ < rotation.images.size()) {
      input.themeImage = rotation.images[entryIndex_];
    }
    // And the backdrop, from the same index and the same column, for the same
    // reason. Null here is not a hole: it is the theme saying "use the field".
    if (useAlt && entryIndex_ < rotation.altBackgrounds.size()) {
      input.backgroundImage = rotation.altBackgrounds[entryIndex_];
    } else if (entryIndex_ < rotation.backgrounds.size()) {
      input.backgroundImage = rotation.backgrounds[entryIndex_];
    }
    // The simulation chases the palette rather than snapping to it, so the
    // authored transition time is what governs the cross-fade. Two consecutive
    // entries can share a theme -- same palette, different settings groups --
    // and that transition is still real, so there is deliberately no
    // "same colours, skip the fade" shortcut here.
    const bool editorPreviewActive = !rotation.pinnedEntryId.empty();
    // Mirrors PhonoscopeStore.settingTransitionSeconds and its colour-theme
    // update loop: an editor preview must always chase promptly, even when the
    // normal colour rotation is paused. Otherwise its ID changes on Iridium
    // but the simulation palette remains frozen on the old entry.
    input.transitionDuration = editorPreviewActive
        ? 0.05
        : std::max(0.0, selectedGroupMatches && rotation.selectedTransitionSeconds
                             ? *rotation.selectedTransitionSeconds
                             : 0.0);
    if (editorPreviewActive || !selectedGroupMatches) {
      // A preview is a cut: it is "show me this one", not a change being made,
      // so it neither flips nor slides. All of its (very short) duration is the
      // ease-out, which is what a bare release means.
      input.transitionAttack = 0.0;
      input.transitionHold = 0.0;
      input.transitionRelease = input.transitionDuration;
      input.centreTransition = CentreTransitionParams{};
      // The backdrop cuts with it: a preview shows one entry, and half of it
      // arriving by slide while the other half cuts is not a preview of
      // anything.
      input.backgroundTransitionAttack = 0.0;
      input.backgroundTransitionHold = 0.0;
      input.backgroundTransitionRelease = input.transitionDuration;
      input.backgroundTransition = CentreTransitionParams{};
    } else {
      input.transitionAttack = std::max(0.0, rotation.selectedTransitionAttack);
      input.transitionHold = std::max(0.0, rotation.selectedTransitionHold);
      input.transitionRelease = std::max(0.0, rotation.selectedTransitionRelease);
      input.centreTransition = rotation.selectedTransition;
      input.backgroundTransitionAttack =
          std::max(0.0, rotation.selectedBackgroundTransitionAttack);
      input.backgroundTransitionHold = std::max(0.0, rotation.selectedBackgroundTransitionHold);
      input.backgroundTransitionRelease =
          std::max(0.0, rotation.selectedBackgroundTransitionRelease);
      input.backgroundTransition = rotation.selectedBackgroundTransition;
    }
    input.transitionPaused = rotation.paused && !editorPreviewActive;
  }
  applyControlLanes(snapshot, frame, input.settings, input.driverInterpolatedSettings);
  input.message = snapshot.message;
  if (const auto scale = input.settings.find("__messageScale"); scale != input.settings.end()) {
    input.messageScale = scale->second;
  }
  if (const auto height = input.settings.find("__centreHeight"); height != input.settings.end()) {
    input.centreHeight = height->second;
  }
  if (const auto width = input.settings.find("__centreWidth"); width != input.settings.end()) {
    input.centreWidth = width->second;
  }
  if (const auto fit = input.settings.find("__centreFit"); fit != input.settings.end()) {
    input.centreFit = imageFitFor(fit->second);
  }
  if (const auto proportional = input.settings.find("__centreProportional");
      proportional != input.settings.end()) {
    input.centreProportional = proportional->second >= 0.5;
  }
  if (const auto blur = input.settings.find("__glowBlur"); blur != input.settings.end()) {
    input.glowBlurAmount = blur->second;
  }
  if (const auto opacity = input.settings.find("__glowOpacity");
      opacity != input.settings.end()) {
    input.glowOpacity = opacity->second;
  }
  if (const auto overdrive = input.settings.find("__glowOverdrive");
      overdrive != input.settings.end()) {
    input.glowOverdrive = overdrive->second;
  }
  if (const auto clamped = input.settings.find("__glowClamp");
      clamped != input.settings.end()) {
    input.glowClamped = clamped->second >= 0.5;
  }
  if (const auto blend = input.settings.find("__glowBlend"); blend != input.settings.end()) {
    input.glowBlendMode = glowBlendModeFor(static_cast<float>(blend->second));
  }
  if (const auto height = input.settings.find("__bgHeight"); height != input.settings.end()) {
    input.backgroundHeight = height->second;
  }
  if (const auto width = input.settings.find("__bgWidth"); width != input.settings.end()) {
    input.backgroundWidth = width->second;
  }
  if (const auto scale = input.settings.find("__bgScale"); scale != input.settings.end()) {
    input.backgroundScale = scale->second;
  }
  if (const auto fit = input.settings.find("__bgFit"); fit != input.settings.end()) {
    input.backgroundFit = imageFitFor(fit->second);
  }
  if (const auto proportional = input.settings.find("__bgProportional");
      proportional != input.settings.end()) {
    input.backgroundProportional = proportional->second >= 0.5;
  }
  if (const auto opacity = input.settings.find("__vignetteOpacity");
      opacity != input.settings.end()) {
    input.vignetteOpacity = opacity->second;
  }
  if (const auto size = input.settings.find("__vignetteSize"); size != input.settings.end()) {
    input.vignetteSize = size->second;
  }
  if (const auto blend = input.settings.find("__sceneBlend"); blend != input.settings.end()) {
    input.sceneBlendMode = sceneBlendModeFor(static_cast<float>(blend->second));
  }
  const Palette activePalette = input.palette;
  simulation_.submit(std::move(input));

  // The rotated palette, not the seed: house-party lighting must match the
  // colours actually on screen.
  // The hue offset is resolved with everything else, then handed to the House
  // Party producer so the lighting jitter can be driven like any other effect.
  const auto hueOffset = input.settings.find("__hueOffset");
  houseParty_.update(frame, activePalette,
                     hueOffset != input.settings.end() ? hueOffset->second : 0.0);
}

void Engine::requestKeyframe() {
  if (!primaryEncoder_) return;
  const double now = monotonicSeconds();
  double last = lastKeyframeRequest_.load(std::memory_order_relaxed);
  if (now - last < kKeyframeRequestInterval) return;
  // Only the thread that wins the exchange actually asks, so simultaneous
  // requests from several clients still cost one IDR.
  if (!lastKeyframeRequest_.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  primaryEncoder_->requestKeyframe();
  // Lets the idle cadence know it must not skip the next frame.
  keyframeWanted_.store(true, std::memory_order_relaxed);
}

int Engine::publishDivisor() const {
  return std::max(1, publishFrameDivisor_.load(std::memory_order_relaxed));
}

// Viewer demand and the debug full-rate lease. Both involve either a blocking
// HTTP call or a policy decision that wants a wall clock, and neither belongs
// in the frame-pacing loop.
void Engine::watcherLoop() {
  double lastReaderPoll = 0;
  while (running_.load(std::memory_order_relaxed)) {
    const double now = monotonicSeconds();

    if (publisher_.connected()) {
      if (now - lastReaderPoll > 2.0) {
        lastReaderPoll = now;
        // An established RTSP publish is NOT viewer demand: MediaMTX accepts the
        // feed whether or not anyone is watching. Ask it how many readers the
        // path actually has, or the service encodes 4K60 into an empty room
        // forever and never gives the voice stack its VRAM back.
        browserWatching_.store(mediamtxReaderCount(options_.mediamtxApiUrl) > 0,
                               std::memory_order_relaxed);
      }
    } else {
      browserWatching_.store(false, std::memory_order_relaxed);
    }

    // Full-rate lease expiry. Time is the backstop; pressure is the real guard,
    // because a diagnostic view must never be the reason the television stutters.
    const double until = fullRateUntil_.load(std::memory_order_relaxed);
    if (until > 0) {
      int reason = 0;
      if (now >= until) {
        reason = 1;
      } else if (gpuReady_.load()) {
        const double frameInterval = 1.0 / std::max(1, options_.frameRate);
        const bool fpsLow = measuredFps_.load(std::memory_order_relaxed) <
                            static_cast<double>(options_.frameRate) * 0.95;
        const bool encodeHigh =
            encodeMs_.load(std::memory_order_relaxed) > frameInterval * 1000.0 * 0.6;
        if (fpsLow || encodeHigh) {
          // Require the pressure to persist, so one slow frame during a config
          // reload does not cancel the lease the operator just took.
          double since = fullRatePressureSince_.load(std::memory_order_relaxed);
          if (since <= 0) {
            fullRatePressureSince_.store(now, std::memory_order_relaxed);
          } else if (now - since >= 2.0) {
            reason = fpsLow ? 2 : 3;
          }
        } else {
          fullRatePressureSince_.store(0, std::memory_order_relaxed);
        }
      }
      if (reason != 0) {
        publishFrameDivisor_.store(kDefaultPublishFrameDivisor, std::memory_order_relaxed);
        fullRateUntil_.store(0, std::memory_order_relaxed);
        fullRatePressureSince_.store(0, std::memory_order_relaxed);
        fullRateExitReason_.store(reason, std::memory_order_relaxed);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

void Engine::simulationLoop() {
  double next = monotonicSeconds();
  while (running_.load(std::memory_order_relaxed)) {
    publishSimulationInput();
    const double started = monotonicSeconds();
    SnapshotPtr snapshot = simulation_.step();
    simulationMs_.store((monotonicSeconds() - started) * 1000.0, std::memory_order_relaxed);
    if (snapshot) {
      particleCount_.store(static_cast<int>(snapshot->particles.size()),
                           std::memory_order_relaxed);
      snapshots_.publish(std::move(snapshot));
    }

    next += kSimulationStep;
    const double now = monotonicSeconds();
    if (next < now) {
      // Fell behind (usually the GPU stealing CPU during a voice turn). Resync
      // rather than spinning to catch up, which would only make it worse.
      next = now;
    }
    sleepUntil(next);
  }
}

void Engine::renderLoop() {
  std::string error;
  if (!startGpu(error)) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    statusError_ = error;
    std::fprintf(stderr, "nova-visualiser: GPU start failed: %s\n", error.c_str());
    running_.store(false);
    return;
  }

  const double frameInterval = 1.0 / std::max(1, options_.frameRate);
  double next = monotonicSeconds();
  double lastActive = monotonicSeconds();
  int64_t frameIndex = 0;
  int fpsFrames = 0;
  double fpsWindow = monotonicSeconds();
  int slowFrames = 0;
  int fastFrames = 0;
  double lastPublishAttempt = 0;
  bool advertisedParameterSets = false;

  // Rolling frame-interval samples for the p99/max readout. 5 s at 60 Hz is 300
  // doubles, so the once-a-second sort costs nothing worth measuring.
  std::vector<double> intervalSamples;
  intervalSamples.reserve(static_cast<size_t>(kStatsWindowSeconds * options_.frameRate) + 16);
  double lastFrameAt = 0;
  // Backdrop shape (theme-authored, human-speed) is refreshed twice a second;
  // the chased colours and speed on top of it are applied every frame. Holding
  // the whole struct at 2 Hz made the chase land in twelve visible steps
  // instead of a ramp, which is the backdrop half of the "colours snap"
  // report.
  gfx::Renderer::FluidBackground fluidSettings;
  double statsWindow = monotonicSeconds();
  int64_t auBytesTotal = 0;
  int64_t auCount = 0;
  int64_t auBytesMax = 0;
  int64_t keyframeBytesTotal = 0;
  int64_t keyframeCount = 0;

  while (running_.load(std::memory_order_relaxed)) {
    // Viewer demand is resolved on the watcher thread. It used to be a blocking
    // HTTP GET right here, inside the pacing loop.
    int streamClients = stream_.clientCount();
#if NOVA_VISUALISER_HAVE_SRT
    streamClients += srt_.clientCount();
#endif
    const bool watching = streamClients > 0 || browserWatching_.load(std::memory_order_relaxed);
    if (watching) lastActive = monotonicSeconds();

    // Give the voice stack its VRAM back when nothing is watching.
    if (!watching && gpuReady_.load() && options_.idleReleaseSeconds > 0 &&
        monotonicSeconds() - lastActive > options_.idleReleaseSeconds) {
      stopGpu();
    }
    if (watching && !gpuReady_.load()) {
      std::string restartError;
      if (!startGpu(restartError)) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusError_ = restartError;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
      device_.makeCurrent();
    }
    if (!gpuReady_.load()) {
      next = monotonicSeconds() + 0.25;
      sleepUntil(next);
      continue;
    }

    SnapshotPtr latest;
    SnapshotPtr previous;
    snapshots_.latestPair(latest, previous);
    if (!latest) {
      next += frameInterval;
      sleepUntil(next);
      continue;
    }

    // Nothing to draw, so send nothing. A configuration that has never been read
    // means no module and no palette to draw it in, and the frame this loop would
    // encode is flat: a client that takes one drops its own working renderer and
    // shows a black screen instead. Staying silent leaves the Apple TV's own
    // Metal engine on screen, which is what it is there for, and `configError` in
    // /status says why. A configuration that was read once and then lost is left
    // alone -- the last known scene is a picture, and a picture beats nothing.
    if (config_.revision() == 0) {
      next += frameInterval;
      sleepUntil(next);
      continue;
    }

    // The simulation runs at 120 Hz and the renderer at 60 Hz, so a rendered
    // frame usually sits exactly on a sim state. Interpolating anyway keeps the
    // motion smooth if either rate slips.
    const float alpha = 1.0f;
    const double frameStarted = monotonicSeconds();
    if (lastFrameAt > 0) intervalSamples.push_back((frameStarted - lastFrameAt) * 1000.0);
    lastFrameAt = frameStarted;

    // Backdrop shape. Refreshed twice a second rather than per frame: a theme
    // changes at human speed, and this is the only place the render thread
    // reads the config snapshot at all.
    if (frameIndex % 30 == 0) {
      const net::ConfigSnapshot configSnapshot = config_.snapshot();
      // The MODULE's declaration. `enabled` is resolved from this plus the
      // image state further down, because a theme's picture is not the module's
      // business -- but which occupant the backdrop's non-image side actually
      // has IS the module's business, and the two questions came apart when a
      // change to or from "no background" became a real transition.
      fluidSettings.field = configSnapshot.valid && configSnapshot.usesFluidBackground;
      fluidSettings.enabled = fluidSettings.field;
      const net::FluidThemeSettings& theme = configSnapshot.fluidTheme;
      fluidSettings.peakIntensity = theme.peakIntensity;
      fluidSettings.falloffPower = theme.falloffPower;
      fluidSettings.warpAmplitude = theme.warpAmplitude;
      fluidSettings.hueSpread = theme.hueSpread;
      fluidSettings.apexGlow = theme.apexGlow;
    }
    if (fluidSettings.enabled) {
      // Chased values, applied every frame. Speed comes from the simulation's
      // resolved/chased settings so baseline drivers and per-theme overrides
      // affect the backdrop too. Shape/intensity above comes from the
      // dashboard appearance theme, while the colours come from the same
      // chased visualiser palette as this scene -- that keeps the band, dots,
      // glow and trails on one selected theme.
      fluidSettings.speed = latest->fluidSpeed;
      fluidSpeedForStatus_.store(latest->fluidSpeed);
    }
    // What the centre slot actually resolved to this frame, for /status.
    centreMessageForStatus_.store(!latest->message.empty());
    centreImageWidthForStatus_.store(latest->centreImage ? latest->centreImage->width : 0);
    centreImageHeightForStatus_.store(latest->centreImage ? latest->centreImage->height : 0);
    centreImageFadeForStatus_.store(latest->centreImageFade);

    // The backdrop pass runs when the module declares the blob field OR when the
    // live colour theme names a background image. `enabled` was only ever about
    // the FIELD -- a module opts into that by declaring a setting affecting
    // `renderer.fluidBackground.speed` -- and a theme's picture is not the
    // module's business. Without this, a background image set under a module
    // with no fluid field simply never drew.
    fluidSettings.image = latest->backgroundImage;
    fluidSettings.imageFrom = latest->backgroundImageFrom;
    fluidSettings.enabled = fluidSettings.field || latest->backgroundImage != nullptr
                            || latest->backgroundImageFrom != nullptr;
    if (fluidSettings.enabled) {
      fluidPhaseForStatus_.store(renderer_.fluidPhase());
      fluidSettings.background = latest->fluidBackground;
      fluidSettings.accent = latest->fluidAccent;
      fluidSettings.highlight = latest->fluidHighlight;
      // Frame geometry and vignette. Every-frame, not on the 2 Hz theme path
      // above: these are driven parameters, and sampling a driver at 2 Hz would
      // turn a smooth sweep into six visible steps a second.
      fluidSettings.heightFraction = latest->backgroundHeight;
      fluidSettings.widthFraction = latest->backgroundWidth;
      fluidSettings.scale = latest->backgroundScale;
      fluidSettings.fit = latest->backgroundFit;
      fluidSettings.proportional = latest->backgroundProportional;
      fluidSettings.imageFade = latest->backgroundImageFade;
      fluidSettings.imageTransition = latest->backgroundTransition;
      fluidSettings.vignette = latest->vignetteColor;
      fluidSettings.vignetteOpacity = latest->vignetteOpacity;
      fluidSettings.vignetteSize = latest->vignetteSize;
      // The composite's own backdrop term, so that under a module with no blob
      // field an image can dissolve to and from the flat colour that really was
      // there. `background`, not `fluidBackground`: the composite draws the
      // energy-mixed one, and landing on anything else would show as a step at
      // the end of the transition.
      fluidSettings.fallback = latest->background;
    }
    renderer_.setFluidBackground(fluidSettings);

    std::string renderError;
    if (!renderer_.render(latest, previous, alpha, options_.exposure, renderError)) {
      std::lock_guard<std::mutex> lock(statusMutex_);
      statusError_ = renderError;
      break;
    }
    renderer_.convertOutputs(options_.exposure);
    // The CUDA copy reads what the compute pass wrote, so the GL work has to be
    // complete first. glFinish is heavy-handed but correct, and at 60 Hz the
    // pipeline depth it costs is preferable to a race.
    renderer_.finish();
    const double renderedAt = monotonicSeconds();
    renderMs_.store((renderedAt - frameStarted) * 1000.0, std::memory_order_relaxed);

    // --- encode -------------------------------------------------------------
    // While nothing is playing the scene is nearly static, but the Apple TV rung
    // is CBR: it would spend the full 16 Mbit/s padding a still picture. Encode
    // a fraction of the frames instead. PTS is absolute (frameIndex *
    // frameInterval), so a skipped frame is simply a longer gap and the client's
    // timebase presents it correctly -- no renegotiation, no header change.
    const int idleDivisor =
        idlePlayback_.load(std::memory_order_relaxed) && options_.idleFrameRate > 0
            ? std::max(1, options_.frameRate / std::max(1, options_.idleFrameRate))
            : 1;
    // A pending keyframe request must not wait out the idle cadence: a client
    // connecting to an idle stream would otherwise stare at nothing.
    const bool encodeThisFrame =
        idleDivisor <= 1 || (frameIndex % idleDivisor) == 0 ||
        keyframeWanted_.load(std::memory_order_relaxed);

    std::vector<encode::EncodedFrame> packets;
    if (encodeThisFrame && primaryEncoder_ && primaryOutput_ >= 0) {
      const gfx::OutputPlane& plane = renderer_.planeFor(primaryOutput_);
      const size_t sampleBytes = options_.tenBit ? 2 : 1;
      std::string copyError;
      interop_.copyToDevice(plane.lumaBuffer, primaryEncoder_->lumaPlane(),
                            static_cast<size_t>(primaryEncoder_->lumaStrideBytes()),
                            static_cast<size_t>(plane.lumaStrideBytes),
                            static_cast<size_t>(plane.width) * sampleBytes,
                            static_cast<size_t>(plane.height), copyError);
      interop_.copyToDevice(plane.chromaBuffer, primaryEncoder_->chromaPlane(),
                            static_cast<size_t>(primaryEncoder_->chromaStrideBytes()),
                            static_cast<size_t>(plane.chromaStrideBytes),
                            static_cast<size_t>(plane.width) * sampleBytes,
                            static_cast<size_t>(plane.height / 2), copyError);
      interop_.synchronise(copyError);
      if (!copyError.empty()) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusError_ = copyError;
      }

      std::string encodeError;
      packets.clear();
      keyframeWanted_.store(false, std::memory_order_relaxed);
      if (primaryEncoder_->encode(frameIndex, packets, encodeError)) {
        const int64_t ptsMicroseconds =
            static_cast<int64_t>(frameIndex * frameInterval * 1e6);
        for (const encode::EncodedFrame& packet : packets) {
          // With in-band parameter sets the encoder only learns them from its
          // first IDR, which is after the handshake was first published. Refresh
          // the advertised header once, so later clients get them immediately
          // instead of waiting for a keyframe.
          if (!advertisedParameterSets && !primaryEncoder_->parameterSets().empty()) {
            advertisedParameterSets = true;
            net::StreamHeader header;
            header.codec = "hevc";
            header.profile = options_.tenBit ? "main10" : "main";
            header.width = options_.width;
            header.height = options_.height;
            header.frameRate = options_.frameRate;
            header.parameterSets = primaryEncoder_->parameterSets();
            stream_.updateHeader(header);
#if NOVA_VISUALISER_HAVE_SRT
            srt_.updateHeader(header);
#endif
          }
          stream_.broadcast(packet, ptsMicroseconds);
#if NOVA_VISUALISER_HAVE_SRT
          srt_.broadcast(packet, ptsMicroseconds);
#endif
          encodedFrames_.fetch_add(1, std::memory_order_relaxed);

          const int64_t bytes = static_cast<int64_t>(packet.data.size());
          auBytesTotal += bytes;
          ++auCount;
          auBytesMax = std::max(auBytesMax, bytes);
          if (packet.keyframe) {
            keyframeBytesTotal += bytes;
            ++keyframeCount;
          }
        }
      } else {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusError_ = encodeError;
      }
    }

    // Turing has a single NVENC engine, and 4K60 HEVC already uses most of it.
    // The always-available browser path is a low-cadence diagnostic rung; its
    // bounded full-rate lease below is explicitly subordinate to Apple TV.
    const bool publishThisFrame =
        publishEncoder_ && publishOutput_ >= 0 && (frameIndex % publishDivisor()) == 0;
    if (publishThisFrame) {
      const gfx::OutputPlane& plane = renderer_.planeFor(publishOutput_);
      std::string copyError;
      interop_.copyToDevice(plane.lumaBuffer, publishEncoder_->lumaPlane(),
                            static_cast<size_t>(publishEncoder_->lumaStrideBytes()),
                            static_cast<size_t>(plane.lumaStrideBytes),
                            static_cast<size_t>(plane.width), static_cast<size_t>(plane.height),
                            copyError);
      interop_.copyToDevice(plane.chromaBuffer, publishEncoder_->chromaPlane(),
                            static_cast<size_t>(publishEncoder_->chromaStrideBytes()),
                            static_cast<size_t>(plane.chromaStrideBytes),
                            static_cast<size_t>(plane.width),
                            static_cast<size_t>(plane.height / 2), copyError);
      interop_.synchronise(copyError);

      std::vector<encode::EncodedFrame> browserPackets;
      std::string encodeError;
      if (publishEncoder_->encode(frameIndex, browserPackets, encodeError)) {
        // Retried lazily and slowly: MediaMTX may start after this service does,
        // but a missing sidecar must not turn into 60 connection attempts a
        // second against a dead port.
        if (!publisher_.connected() && !browserPackets.empty() &&
            monotonicSeconds() - lastPublishAttempt > 5.0) {
          lastPublishAttempt = monotonicSeconds();
          std::string openError;
          if (!publisher_.open(options_.publishUrl, publishEncoder_->settings(),
                               publishEncoder_->parameterSets(), openError)) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            statusError_ = "browser rung: " + openError;
          }
        }
        if (publisher_.connected()) {
          for (const encode::EncodedFrame& packet : browserPackets) {
            std::string writeError;
            if (!publisher_.write(packet, writeError)) {
              publisher_.close();
              break;
            }
          }
        }
      }
    }
    encodeMs_.store((monotonicSeconds() - renderedAt) * 1000.0, std::memory_order_relaxed);

    // --- pacing and dynamic resolution ---------------------------------------
    // Keyed on render cost alone, not total frame cost. The encode surface is
    // always full size, so shrinking the scene does nothing whatsoever to
    // relieve an encoder bottleneck -- reacting to total cost would throw away
    // scene resolution for no gain, which is exactly what it did at first.
    const double renderCost = renderedAt - frameStarted;
    const double renderBudget = frameInterval * 0.5;
    if (renderCost > renderBudget) {
      ++slowFrames;
      fastFrames = 0;
    } else if (renderCost < renderBudget * 0.6) {
      ++fastFrames;
      slowFrames = 0;
    }
    // Scale the scene, never the encode surface: the stream resolution must not
    // change under the client, but the scene can quietly render smaller while
    // the GPU is busy with voice inference and be upscaled into the same frame.
    if (slowFrames >= 30 && renderer_.resolutionScale() > 0.5f) {
      renderer_.setResolutionScale(renderer_.resolutionScale() - 0.1f);
      slowFrames = 0;
    } else if (fastFrames >= 300 && renderer_.resolutionScale() < 1.0f) {
      renderer_.setResolutionScale(renderer_.resolutionScale() + 0.05f);
      fastFrames = 0;
    }

    ++frameIndex;
    ++fpsFrames;
    const double now = monotonicSeconds();
    if (now - fpsWindow >= 1.0) {
      measuredFps_.store(fpsFrames / (now - fpsWindow), std::memory_order_relaxed);
      fpsFrames = 0;
      fpsWindow = now;
    }
    gpuMs_.store(renderer_.stats().gpuMilliseconds, std::memory_order_relaxed);

    // Publish the rolling window once a second, then start a fresh one. Sorting
    // a copy keeps the samples in arrival order for the drop below.
    if (now - statsWindow >= 1.0) {
      statsWindow = now;
      if (!intervalSamples.empty()) {
        std::vector<double> sorted = intervalSamples;
        std::sort(sorted.begin(), sorted.end());
        const size_t index = std::min(sorted.size() - 1,
                                      static_cast<size_t>(std::floor(sorted.size() * 0.99)));
        frameIntervalP99Ms_.store(sorted[index], std::memory_order_relaxed);
        frameIntervalMaxMs_.store(sorted.back(), std::memory_order_relaxed);
      }
      if (auCount > 0) {
        auBytesMean_.store(static_cast<double>(auBytesTotal) / static_cast<double>(auCount),
                           std::memory_order_relaxed);
        auBytesMax_.store(auBytesMax, std::memory_order_relaxed);
      }
      if (keyframeCount > 0) {
        keyframeBytesMean_.store(
            static_cast<double>(keyframeBytesTotal) / static_cast<double>(keyframeCount),
            std::memory_order_relaxed);
      }

      // Drop the oldest second so the window slides rather than resetting; a
      // window that resets hides a hitch that lands just after a flush.
      const size_t keep = static_cast<size_t>(kStatsWindowSeconds * options_.frameRate);
      if (intervalSamples.size() > keep) {
        intervalSamples.erase(intervalSamples.begin(),
                              intervalSamples.begin() +
                                  static_cast<std::ptrdiff_t>(intervalSamples.size() - keep));
      }
      auBytesTotal = 0;
      auCount = 0;
      auBytesMax = 0;
      keyframeBytesTotal = 0;
      keyframeCount = 0;
    }

    next += frameInterval;
    if (next < now) next = now;
    sleepUntil(next);
  }
}

std::string Engine::statusJson() const {
  const net::ConfigSnapshot snapshot = config_.snapshot();
  const net::NowPlaying nowPlaying = config_.nowPlaying();
  std::string error;
  std::string clientConnectionState;
  std::string clientDisconnectReason;
  int64_t clientReconnectCount = 0;
  SignalFrame upstreamSignal;
  double upstreamSignalAge = -1;
  {
    std::lock_guard<std::mutex> lock(signalMutex_);
    if (hasUpstreamSignal_) {
      upstreamSignal = upstreamSignal_;
      upstreamSignalAge = std::max(0.0, monotonicSeconds() - upstreamSignalReceivedAt_);
    }
  }
  {
    std::lock_guard<std::mutex> lock(statusMutex_);
    error = statusError_;
    clientConnectionState = clientConnectionState_;
    clientDisconnectReason = clientDisconnectReason_;
    clientReconnectCount = clientReconnectCount_;
  }

  std::ostringstream out;
  out.precision(3);
  out << std::fixed << "{";
  out << "\"ok\":" << (gpuReady_.load() ? "true" : "false") << ",";
  out << "\"renderer\":\"" << json::escape(device_.renderer()) << "\",";
  out << "\"fps\":" << measuredFps_.load() << ",";
  out << "\"renderMs\":" << renderMs_.load() << ",";
  out << "\"encodeMs\":" << encodeMs_.load() << ",";
  out << "\"simulationMs\":" << simulationMs_.load() << ",";
  out << "\"particles\":" << particleCount_.load() << ",";
  out << "\"encodedFrames\":" << encodedFrames_.load() << ",";
  out << "\"clients\":" << stream_.clientCount() << ",";
  out << "\"presentationDelayMs\":" << renderAheadSeconds() * 1000.0 << ",";
  out << "\"clockSource\":\"appletv-musickit\",";
  out << "\"signalSource\":\"" << (upstreamSignalAge >= 0 && upstreamSignalAge < 1.0
                                            ? "appletv-resolved"
                                            : "renderer-derived") << "\",";
  out << "\"signalAgeMs\":" << (upstreamSignalAge < 0 ? -1 : upstreamSignalAge * 1000.0)
      << ",";
  out << "\"signalBeat\":" << upstreamSignal.beatPulse << ",";
  out << "\"signalDownbeat\":" << upstreamSignal.downbeatPulse << ",";
  out << "\"signalBass\":"
      << *std::max_element(upstreamSignal.spectrum.begin(), upstreamSignal.spectrum.begin() + 8)
      << ",";
  out << "\"signalMid\":"
      << *std::max_element(upstreamSignal.spectrum.begin() + 8, upstreamSignal.spectrum.begin() + 20)
      << ",";
  out << "\"signalTreble\":"
      << *std::max_element(upstreamSignal.spectrum.begin() + 20, upstreamSignal.spectrum.end())
      << ",";
  // The backdrop's resolved animation rate, after the module's parameter driver
  // has been applied. Exposed because a driven `fluid_speed` is otherwise
  // invisible from outside: nothing else reports what the envelope settled on,
  // which made a bass-acceleration driver impossible to verify without
  // eyeballing the picture.
  out << "\"fluidSpeed\":" << fluidSpeedForStatus_.load() << ",";
  out << "\"fluidPhase\":" << fluidPhaseForStatus_.load() << ",";
  // The centre slot as resolved, not as configured. A message that lost to an
  // image, an image whose fetch or decode failed, and a theme that supplies
  // none all look identical from the configuration alone.
  // The configuration pipeline itself. A renderer that has silently stopped
  // re-reading looks exactly like one nobody has reconfigured, which cost real
  // time to tell apart once.
  out << "\"configRevision\":" << config_.revision() << ",";
  out << "\"configError\":\"" << json::escape(config_.lastError()) << "\",";
  out << "\"centreMessage\":" << (centreMessageForStatus_.load() ? "true" : "false") << ",";
  out << "\"centreImageWidth\":" << centreImageWidthForStatus_.load() << ",";
  out << "\"centreImageHeight\":" << centreImageHeightForStatus_.load() << ",";
  out << "\"centreImageFade\":" << centreImageFadeForStatus_.load() << ",";
#if NOVA_VISUALISER_HAVE_SRT
  {
    // The SRT rung's own view of the link. These are measurements, unlike the
    // TCP rung's client-reported delay, so they are what adaptive bitrate and
    // any latency question should be answered from.
    const net::SrtTransportStats stats = srt_.worstStats();
    out << "\"srtAvailable\":" << (srt_.available() ? "true" : "false") << ",";
    out << "\"srtPort\":" << options_.srtPort << ",";
    out << "\"srtLatencyMs\":" << srt_.latencyMs() << ",";
    out << "\"srtClients\":" << srt_.clientCount() << ",";
    out << "\"srtFramesSent\":" << srt_.framesSent() << ",";
    out << "\"srtFramesDropped\":" << srt_.framesDropped() << ",";
    out << "\"srtRttMs\":" << stats.rttMs << ",";
    out << "\"srtBandwidthMbps\":" << stats.bandwidthMbps << ",";
    out << "\"srtPacketsLost\":" << stats.packetsLost << ",";
    out << "\"srtPacketsRetransmitted\":" << stats.packetsRetransmitted << ",";
    out << "\"srtPacketsDropped\":" << stats.packetsDropped << ",";
    out << "\"srtSendBufferMs\":" << stats.sendBufferMs << ",";
  }
#else
  out << "\"srtAvailable\":false,";
  out << "\"srtPort\":0,";
#endif
  out << "\"resolutionScale\":" << renderer_.stats().resolutionScale << ",";
  out << "\"renderWidth\":" << renderer_.stats().renderWidth << ",";
  out << "\"renderHeight\":" << renderer_.stats().renderHeight << ",";
  // Instrumentation. The interval tail is the honest hitch signal; fps is not.
  out << "\"gpuMs\":" << gpuMs_.load() << ",";
  out << "\"frameIntervalP99Ms\":" << frameIntervalP99Ms_.load() << ",";
  out << "\"frameIntervalMaxMs\":" << frameIntervalMaxMs_.load() << ",";
  out << "\"auBytesMean\":" << auBytesMean_.load() << ",";
  out << "\"auBytesMax\":" << auBytesMax_.load() << ",";
  out << "\"keyframeBytesMean\":" << keyframeBytesMean_.load() << ",";
  out << "\"clientDecodedFps\":" << clientDecodedFps_.load() << ",";
  out << "\"clientDecodedFrames\":" << clientDecodedFrames_.load() << ",";
  out << "\"clientDroppedFrames\":" << clientDroppedFrames_.load() << ",";
  out << "\"clientConnectionState\":\"" << json::escape(clientConnectionState) << "\",";
  out << "\"clientDisconnectReason\":\"" << json::escape(clientDisconnectReason) << "\",";
  out << "\"clientReconnectCount\":" << clientReconnectCount << ",";
  out << "\"publishFrameDivisor\":" << publishDivisor() << ",";
  {
    const double until = fullRateUntil_.load();
    const double remaining = until > 0 ? std::max(0.0, until - monotonicSeconds()) : 0.0;
    out << "\"fullRateSecondsRemaining\":" << remaining << ",";
    const int reason = fullRateExitReason_.load();
    const char* reasonText = reason == 1   ? "expired"
                             : reason == 2 ? "frame-rate-pressure"
                             : reason == 3 ? "encode-pressure"
                                           : "";
    out << "\"fullRateLastExitReason\":\"" << reasonText << "\",";
  }
  out << "\"streamWidth\":" << options_.width << ",";
  out << "\"streamHeight\":" << options_.height << ",";
  out << "\"tenBit\":" << (options_.tenBit ? "true" : "false") << ",";
  out << "\"browserRung\":" << (publisher_.connected() ? "true" : "false") << ",";
  out << "\"configStream\":" << (config_.streamConnected() ? "true" : "false") << ",";
  out << "\"module\":\"" << json::escape(snapshot.activeModuleId) << "\",";
  out << "\"moduleVersion\":\"" << json::escape(snapshot.activeModuleVersion) << "\",";
  out << "\"colorEntries\":" << snapshot.rotation.palettes.size() << ",";
  out << "\"colorEntryIndex\":" << entryIndex_ << ",";
  out << "\"colorEntryId\":\"" << json::escape(currentEntryId_) << "\",";
  out << "\"colorThemeId\":\"" << json::escape(currentThemeId_) << "\",";
  out << "\"colorThemePaused\":" << (snapshot.rotation.paused ? "true" : "false") << ",";
  out << "\"colorThemeRevision\":" << snapshot.rotation.revision << ",";
  // Which settings groups the live entry is running, so the status readout can
  // say what behaviour is on screen and not just what colours are.
  out << "\"settingsGroups\":\"";
  for (size_t index = 0; index < snapshot.rotation.selectedSettingsGroupIds.size(); ++index) {
    if (index != 0) out << ",";
    out << json::escape(snapshot.rotation.selectedSettingsGroupIds[index]);
  }
  out << "\",";
  out << "\"housePartyActive\":" << (houseParty_.active() ? "true" : "false") << ",";
  out << "\"track\":\"" << json::escape(nowPlaying.valid ? nowPlaying.identity.title : "") << "\",";
  out << "\"error\":\"" << json::escape(error) << "\"";
  out << "}";
  return out.str();
}

net::ControlResponse Engine::handleControl(const net::ControlRequest& request) {
  net::ControlResponse response;
  const std::string path = request.path.substr(0, request.path.find('?'));

  if (path == "/status" || path == "/") {
    response.body = statusJson();
    return response;
  }
  if (path == "/healthz") {
    // `ok` keeps its old meaning -- is the GPU pipeline resident -- so this is an
    // addition rather than a redefinition. Note what that meaning costs: it is
    // false, with a 503, for as long as the idle policy has released the card,
    // which is the ordinary state of a renderer nobody is watching. `config` is
    // the field that separates "nothing to draw" from "nothing to watch"; a
    // configuration never read at all used to look identical to a healthy service
    // from here for weeks.
    const bool ready = gpuReady_.load();
    response.body = std::string("{\"ok\":") + (ready ? "true" : "false") +
                    ",\"config\":" + (config_.revision() > 0 ? "true" : "false") + "}";
    response.status = ready ? 200 : 503;
    return response;
  }
  if (request.method != "POST") {
    response.status = 404;
    response.body = R"({"error":"not found"})";
    return response;
  }
  if (path == "/keyframe") {
    requestKeyframe();
    response.body = R"({"ok":true})";
    return response;
  }
  if (path == "/full-rate") {
    // Takes the browser rung to full frame rate for a bounded window, so a
    // debug viewer shows what the Apple TV rung actually receives rather than a
    // half-rate approximation. Bounded because the two rungs share one NVENC
    // engine: this is borrowed capacity, never granted capacity.
    auto value = json::Value::parse(request.body);
    double seconds = 0;
    if (value) {
      const json::Value* field = value->find("seconds");
      if (field != nullptr) seconds = field->number().value_or(0.0);
    }
    seconds = std::min(seconds, static_cast<double>(options_.fullRateMaxSeconds));
    seconds = std::max(0.0, seconds);
    if (seconds <= 0) {
      publishFrameDivisor_.store(kDefaultPublishFrameDivisor, std::memory_order_relaxed);
      fullRateUntil_.store(0, std::memory_order_relaxed);
    } else {
      publishFrameDivisor_.store(1, std::memory_order_relaxed);
      fullRateUntil_.store(monotonicSeconds() + seconds, std::memory_order_relaxed);
    }
    fullRatePressureSince_.store(0, std::memory_order_relaxed);
    fullRateExitReason_.store(0, std::memory_order_relaxed);
    std::ostringstream body;
    body.precision(3);
    body << std::fixed << R"({"ok":true,"seconds":)" << seconds
         << R"(,"publishFrameDivisor":)" << publishDivisor() << "}";
    response.body = body.str();
    return response;
  }
  if (path == "/refresh") {
    config_.poke();
    response.body = R"({"ok":true})";
    return response;
  }
  if (path == "/signal") {
    auto value = json::Value::parse(request.body);
    if (!value || value->object() == nullptr) {
      response.status = 400;
      response.body = R"({"error":"invalid signal"})";
      return response;
    }
    auto number = [&](std::string_view key, double fallback) {
      const json::Value* field = value->find(key);
      return field != nullptr ? field->numberOr(fallback) : fallback;
    };
    SignalFrame signal;
    signal.time = std::max(0.0, number("time", 0));
    signal.delta = std::max(0.0, std::min(0.25, number("delta", kSimulationStep)));
    signal.duration = std::max(0.0, number("duration", 0));
    signal.progress = std::max(0.0, std::min(1.0, number("progress", 0)));
    signal.playing = value->find("playing") != nullptr &&
                     value->find("playing")->boolean().value_or(false);
    signal.bpm = std::max(20.0, number("bpm", 72));
    signal.beatPhase = std::max(0.0, std::min(1.0, number("beatPhase", 0)));
    signal.beatPulse = std::max(0.0, std::min(1.0, number("beatPulse", 0)));
    signal.beatIndex = static_cast<int>(number("beatIndex", 0));
    signal.barPhase = std::max(0.0, std::min(1.0, number("barPhase", 0)));
    signal.barIndex = static_cast<int>(number("barIndex", 0));
    signal.timeSignature = std::max(1, static_cast<int>(number("timeSignature", 4)));
    signal.downbeatPulse = std::max(0.0, std::min(1.0, number("downbeatPulse", 0)));
    signal.energy = std::max(0.0, std::min(1.0, number("energy", 0.28)));
    signal.valence = std::max(0.0, std::min(1.0, number("valence", 0.5)));
    signal.lyricProgress = std::max(0.0, std::min(1.0, number("lyricProgress", 0)));
    signal.lyricPulse = std::max(0.0, std::min(1.0, number("lyricPulse", 0)));
    signal.lyricIndex = static_cast<int>(number("lyricIndex", -1));
    signal.lyricCurrent = value->find("lyricCurrent") != nullptr
                              ? value->find("lyricCurrent")->stringOr("").substr(0, 512)
                              : "";
    signal.lyricNext = value->find("lyricNext") != nullptr
                           ? value->find("lyricNext")->stringOr("").substr(0, 512)
                           : "";
    signal.quality = static_cast<SignalQuality>(std::max(
        0, std::min(4, static_cast<int>(number("quality", 0)))));
    if (const json::Value* seed = value->find("trackSeed")) {
      try {
        signal.trackSeed = std::stoull(seed->stringOr("0"));
      } catch (...) {
        signal.trackSeed = 0x4e4f5641;
      }
    }
    if (const json::Value* spectrum = value->find("spectrum")) {
      const std::vector<double> bands = spectrum->numberArray();
      for (size_t index = 0; index < signal.spectrum.size() && index < bands.size(); ++index) {
        signal.spectrum[index] = static_cast<float>(
            std::max(0.0, std::min(1.0, bands[index])));
      }
    }
    {
      std::lock_guard<std::mutex> lock(signalMutex_);
      upstreamSignal_ = std::move(signal);
      upstreamSignalReceivedAt_ = monotonicSeconds();
      hasUpstreamSignal_ = true;
    }
    response.body = R"({"ok":true})";
    return response;
  }
  if (path == "/telemetry") {
    auto value = json::Value::parse(request.body);
    if (!value || value->object() == nullptr) {
      response.status = 400;
      response.body = R"({"error":"invalid telemetry"})";
      return response;
    }
    if (const json::Value* fps = value->find("decodedFps")) {
      clientDecodedFps_.store(std::max(0.0, std::min(240.0, fps->numberOr(0))),
                              std::memory_order_relaxed);
    }
    if (const json::Value* decoded = value->find("decodedFrames")) {
      clientDecodedFrames_.store(std::max<int64_t>(0, static_cast<int64_t>(decoded->numberOr(0))),
                                 std::memory_order_relaxed);
    }
    if (const json::Value* dropped = value->find("droppedFrames")) {
      clientDroppedFrames_.store(std::max<int64_t>(0, static_cast<int64_t>(dropped->numberOr(0))),
                                 std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      if (const json::Value* state = value->find("connectionState")) {
        clientConnectionState_ = state->stringOr("").substr(0, 64);
        if (clientConnectionState_ == "streaming") clientDisconnectReason_.clear();
      }
      if (const json::Value* reason = value->find("disconnectReason")) {
        clientDisconnectReason_ = reason->stringOr("").substr(0, 256);
      }
      if (const json::Value* reconnects = value->find("reconnectCount")) {
        clientReconnectCount_ = std::max<int64_t>(
            0, static_cast<int64_t>(reconnects->numberOr(clientReconnectCount_)));
      }
    }
    response.body = R"({"ok":true})";
    return response;
  }
  if (path == "/house-party") {
    auto value = json::Value::parse(request.body);
    const bool enabled =
        value && value->find("enabled") != nullptr &&
        value->find("enabled")->boolean().value_or(false);
    houseParty_.setEnabled(enabled);
    response.body = std::string(R"({"ok":true,"enabled":)") + (enabled ? "true" : "false") + "}";
    return response;
  }

  response.status = 404;
  response.body = R"({"error":"not found"})";
  return response;
}

bool Engine::run(std::string& error) {
  running_.store(true);

  config_.start(options_.dashboardUrl, options_.configPollSeconds);
  houseParty_.start(options_.dashboardUrl);
  houseParty_.setEnabled(options_.housePartyEnabled);

  net::StreamHeader header;
  header.width = options_.width;
  header.height = options_.height;
  header.frameRate = options_.frameRate;
  header.profile = options_.tenBit ? "main10" : "main";
  if (!stream_.start(options_.streamPort, header,
                     [this]() {
                       // A client needs an IDR immediately when it joins, and
                       // again whenever the transport has dropped it to the next
                       // one. Without this second case a WiFi hiccup leaves the
                       // television blank until the next scheduled GOP boundary.
                       requestKeyframe();
                     },
                     error)) {
    return false;
  }
#if NOVA_VISUALISER_HAVE_SRT
  if (options_.srtPort > 0) {
    std::string srtError;
    if (!srt_.start(options_.srtPort, options_.srtLatencyMs, header,
                    [this]() { requestKeyframe(); }, srtError)) {
      // Non-fatal: the TCP rung still serves the Apple TV, so a transport
      // problem must not take the visualiser down with it.
      std::lock_guard<std::mutex> lock(statusMutex_);
      statusError_ = "srt rung unavailable: " + srtError;
    }
  }
#endif

  if (!control_.start(options_.controlPort,
                      [this](const net::ControlRequest& request) { return handleControl(request); },
                      error)) {
    return false;
  }

  simulationThread_ = std::thread(&Engine::simulationLoop, this);
  renderThread_ = std::thread(&Engine::renderLoop, this);
  watcherThread_ = std::thread(&Engine::watcherLoop, this);

  while (running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // Arm the shutdown watchdog *before* joining, not after. CUDA teardown can
  // wedge inside the driver, and a watchdog started after the joins would never
  // run — which is exactly how a stop turned into systemd's 90 s SIGKILL.
  std::thread([]() {
    std::this_thread::sleep_for(std::chrono::seconds(8));
    std::fprintf(stderr, "nova-visualiser: shutdown wedged, exiting hard\n");
    std::fflush(nullptr);
    _exit(0);
  }).detach();

  if (simulationThread_.joinable()) simulationThread_.join();
  if (renderThread_.joinable()) renderThread_.join();
  if (watcherThread_.joinable()) watcherThread_.join();
  stream_.stop();
#if NOVA_VISUALISER_HAVE_SRT
  srt_.stop();
#endif
  control_.stop();
  houseParty_.stop();
  config_.stop();

  std::lock_guard<std::mutex> lock(statusMutex_);
  if (!statusError_.empty()) {
    error = statusError_;
    return false;
  }
  return true;
}

bool Engine::selfTest(int frames, std::string& error) {
  if (!startGpu(error)) return false;

  config_.start(options_.dashboardUrl, options_.configPollSeconds);
  // Give the config client a moment to fetch the active module before timing.
  for (int attempt = 0; attempt < 50 && !config_.snapshot().valid; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!config_.snapshot().valid) {
    error = "could not load the active module: " + config_.lastError();
    config_.stop();
    return false;
  }

  running_.store(true);
  simulationThread_ = std::thread(&Engine::simulationLoop, this);
  // Let the simulation build a scene.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const double started = monotonicSeconds();
  double renderTotal = 0;
  double encodeTotal = 0;
  int64_t bytes = 0;
  int rendered = 0;

  for (int index = 0; index < frames && running_.load(); ++index) {
    SnapshotPtr latest;
    SnapshotPtr previous;
    snapshots_.latestPair(latest, previous);
    if (!latest) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    const double frameStarted = monotonicSeconds();
    if (!renderer_.render(latest, previous, 1.0f, options_.exposure, error)) break;
    renderer_.convertOutputs(options_.exposure);
    renderer_.finish();
    const double renderedAt = monotonicSeconds();
    renderTotal += renderedAt - frameStarted;

    const gfx::OutputPlane& plane = renderer_.planeFor(primaryOutput_);
    const size_t sampleBytes = options_.tenBit ? 2 : 1;
    std::string copyError;
    interop_.copyToDevice(plane.lumaBuffer, primaryEncoder_->lumaPlane(),
                          static_cast<size_t>(primaryEncoder_->lumaStrideBytes()),
                          static_cast<size_t>(plane.lumaStrideBytes),
                          static_cast<size_t>(plane.width) * sampleBytes,
                          static_cast<size_t>(plane.height), copyError);
    interop_.copyToDevice(plane.chromaBuffer, primaryEncoder_->chromaPlane(),
                          static_cast<size_t>(primaryEncoder_->chromaStrideBytes()),
                          static_cast<size_t>(plane.chromaStrideBytes),
                          static_cast<size_t>(plane.width) * sampleBytes,
                          static_cast<size_t>(plane.height / 2), copyError);
    interop_.synchronise(copyError);
    if (!copyError.empty()) {
      error = copyError;
      break;
    }

    std::vector<encode::EncodedFrame> packets;
    if (!primaryEncoder_->encode(index, packets, error)) break;
    for (const encode::EncodedFrame& packet : packets) bytes += packet.data.size();
    encodeTotal += monotonicSeconds() - renderedAt;
    ++rendered;
    if (rendered % 30 == 0) {
      std::printf("  %d/%d frames, %.1f fps so far\n", rendered, frames,
                  rendered / std::max(0.001, monotonicSeconds() - started));
    }
  }

  const double elapsed = monotonicSeconds() - started;
  running_.store(false);
  if (simulationThread_.joinable()) simulationThread_.join();
  config_.stop();

  std::printf("frames        : %d\n", rendered);
  std::printf("elapsed       : %.2f s (%.1f fps)\n", elapsed, rendered / std::max(0.001, elapsed));
  std::printf("render        : %.2f ms/frame\n", rendered > 0 ? renderTotal / rendered * 1000 : 0);
  std::printf("encode+copy   : %.2f ms/frame\n", rendered > 0 ? encodeTotal / rendered * 1000 : 0);
  std::printf("bitrate       : %.1f Mbps\n", elapsed > 0 ? bytes * 8.0 / elapsed / 1e6 : 0);
  std::printf("particles     : %d\n", particleCount_.load());
  std::printf("scene size    : %dx%d (scale %.2f)\n", renderer_.stats().renderWidth,
              renderer_.stats().renderHeight, renderer_.stats().resolutionScale);
  return rendered > 0 && error.empty();
}

bool Engine::dumpFrame(const std::string& path, std::string& error) {
  if (!startGpu(error)) return false;
  config_.start(options_.dashboardUrl, options_.configPollSeconds);
  for (int attempt = 0; attempt < 50 && !config_.snapshot().valid; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  const net::ConfigSnapshot configSnapshot = config_.snapshot();
  if (!configSnapshot.valid) {
    error = "could not load the active module: " + config_.lastError();
    config_.stop();
    return false;
  }
  std::printf("module        : %s@%s\n", configSnapshot.activeModuleId.c_str(),
              configSnapshot.activeModuleVersion.c_str());

  // Run a short warm-up so the scene has settled into something worth looking
  // at rather than the first tick of a cold field.
  for (int index = 0; index < 240; ++index) {
    publishSimulationInput();
    snapshots_.publish(simulation_.step());
  }

  SnapshotPtr latest;
  SnapshotPtr previous;
  snapshots_.latestPair(latest, previous);
  if (!latest) {
    error = "the simulation published no snapshot";
    config_.stop();
    return false;
  }
  std::printf("particles     : %zu\n", latest->particles.size());
  if (!latest->particles.empty()) {
    const RenderParticle& first = latest->particles.front();
    std::printf("first particle: pos %.3f,%.3f size %.4f colour %.2f,%.2f,%.2f,%.2f glow %.2f\n",
                first.positionSize.x, first.positionSize.y, first.positionSize.w, first.color.x,
                first.color.y, first.color.z, first.color.w, first.meta.x);
  }
  std::printf("bounds        : %.2f,%.2f .. %.2f,%.2f\n", latest->boundsMinimum.x,
              latest->boundsMinimum.y, latest->boundsMaximum.x, latest->boundsMaximum.y);
  std::printf("background    : %.3f,%.3f,%.3f,%.3f\n", latest->background.x, latest->background.y,
              latest->background.z, latest->background.w);

  // The dump exists to be compared against what the television draws, so it has
  // to include the animated backdrop. Without this it would render the module
  // over a flat colour and "look wrong" in exactly the way being investigated.
  gfx::Renderer::FluidBackground fluid;
  fluid.field = configSnapshot.usesFluidBackground;
  fluid.enabled = fluid.field;
  fluid.fallback = latest->background;
  if (fluid.enabled) {
    auto setting = configSnapshot.settings.find("fluid_speed");
    if (setting != configSnapshot.settings.end()) {
      fluid.speed = static_cast<float>(setting->second);
    }
    const net::FluidThemeSettings& theme = configSnapshot.fluidTheme;
    fluid.peakIntensity = theme.peakIntensity;
    fluid.falloffPower = theme.falloffPower;
    fluid.warpAmplitude = theme.warpAmplitude;
    fluid.hueSpread = theme.hueSpread;
    fluid.apexGlow = theme.apexGlow;
    fluid.background = latest->fluidBackground;
    fluid.accent = latest->fluidAccent;
    fluid.highlight = latest->fluidHighlight;
  }
  renderer_.setFluidBackground(fluid);
  std::printf("fluid backdrop: %s (theme %s)\n", fluid.enabled ? "on" : "off",
              configSnapshot.fluidTheme.available ? "loaded" : "defaults");

  if (!renderer_.render(latest, previous, 1.0f, options_.exposure, error)) {
    config_.stop();
    return false;
  }
  renderer_.finish();
  std::printf("stages        : %s\n", renderer_.stageSummary().c_str());

  std::vector<uint8_t> pixels;
  int width = 0;
  int height = 0;
  if (!renderer_.readbackRgba(pixels, width, height, options_.exposure)) {
    error = "framebuffer readback failed";
    config_.stop();
    return false;
  }

  // Netpbm: no encoder dependency, and `ffmpeg -i` or any viewer reads it.
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << width << " " << height << "\n255\n";
  for (int y = height - 1; y >= 0; --y) {
    for (int x = 0; x < width; ++x) {
      const size_t index = (static_cast<size_t>(y) * width + x) * 4;
      out.put(static_cast<char>(pixels[index]));
      out.put(static_cast<char>(pixels[index + 1]));
      out.put(static_cast<char>(pixels[index + 2]));
    }
  }
  config_.stop();
  std::printf("wrote %s (%dx%d)\n", path.c_str(), width, height);
  return true;
}

}  // namespace nova::service
