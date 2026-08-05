#include "net/config_client.h"

#include <chrono>
#include <cmath>

#include "core/json.h"
#include "net/http_client.h"

namespace nova::net {
namespace {

double monotonicSeconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

double numberField(const json::Value& value, std::string_view key, double fallback) {
  const json::Value* field = value.find(key);
  return field != nullptr ? field->numberOr(fallback) : fallback;
}

std::string stringField(const json::Value& value, std::string_view key,
                        std::string_view fallback = "") {
  const json::Value* field = value.find(key);
  return field != nullptr ? field->stringOr(fallback) : std::string(fallback);
}

// The dashboard stores colours as { rgb: [0-255], intensity: 0-100, opacity }.
// Port of `PhonoscopeColorValue.vector`.
Vec4 colourVector(const json::Value& value) {
  const json::Value* rgb = value.find("rgb");
  const std::vector<double> components = rgb != nullptr ? rgb->numberArray() : std::vector<double>{};
  const double intensity = clampValue(numberField(value, "intensity", 100.0), 0.0, 100.0) / 100.0;
  const double opacity = clampValue(numberField(value, "opacity", 100.0), 0.0, 100.0) / 100.0;
  auto channel = [&](size_t index) {
    const double raw = index < components.size() ? components[index] : 0.0;
    return static_cast<float>(clampValue(raw, 0.0, 255.0) * intensity / 255.0);
  };
  return Vec4{channel(0), channel(1), channel(2), static_cast<float>(opacity)};
}

PhonoscopeParameterSource parameterSource(const json::Value& value,
                                          std::string_view fallbackType = "manual") {
  PhonoscopeParameterSource source;
  source.type = stringField(value, "type", fallbackType);
  auto optionalNumber = [&](std::string_view key) -> std::optional<double> {
    const json::Value* field = value.find(key);
    return field != nullptr ? field->number() : std::nullopt;
  };
  source.value = optionalNumber("value");
  source.min = optionalNumber("min");
  source.max = optionalNumber("max");
  source.cadence = stringField(value, "cadence", "beat");
  source.intervalSeconds = numberField(value, "intervalSeconds", 4.0);
  source.transitionSeconds = numberField(value, "transitionSeconds", 0.5);
  source.attackSeconds = numberField(value, "attackSeconds", 0.05);
  source.holdSeconds = numberField(value, "holdSeconds", 0.0);
  source.releaseSeconds = numberField(value, "releaseSeconds", 0.6);
  return source;
}

}  // namespace

ConfigClient::~ConfigClient() { stop(); }

void ConfigClient::start(std::string baseUrl, double pollSeconds) {
  baseUrl_ = std::move(baseUrl);
  if (running_.exchange(true)) return;

  // Realtime push. The dashboard emits a `phonoscope` event whenever the config
  // is written; anything else on the bus is ignored here.
  sse_.start(baseUrl_ + "/api/events", [this](const std::string& event, const std::string& data) {
    if (event == "phonoscope" || data.find("\"phonoscope\"") != std::string::npos) {
      pending_.store(true, std::memory_order_relaxed);
    }
  });

  thread_ = std::thread(&ConfigClient::run, this, pollSeconds);
}

void ConfigClient::stop() {
  if (!running_.exchange(false)) return;
  sse_.stop();
  if (thread_.joinable()) thread_.join();
}

void ConfigClient::poke() { pending_.store(true, std::memory_order_relaxed); }

void ConfigClient::run(double pollSeconds) {
  double lastPoll = 0;
  double lastNowPlaying = 0;
  while (running_.load(std::memory_order_relaxed)) {
    const double now = monotonicSeconds();
    // SSE handles the fast path; the timed poll is only a safety net for a
    // dropped stream, so it can be lazy without costing responsiveness.
    if (pending_.exchange(false, std::memory_order_relaxed) || now - lastPoll >= pollSeconds) {
      lastPoll = now;
      refreshConfiguration();
    }
    // The now-playing uplink from the Apple TV lands in the dashboard's master
    // clock; sample it often enough to keep beat phase honest between updates.
    if (now - lastNowPlaying >= 0.5) {
      lastNowPlaying = now;
      refreshNowPlaying();
    }
    // The theme changes at human speed and only tints the backdrop, so it is
    // polled slowly and never blocks anything.
    if (now - lastThemeFetch_ >= 10.0) {
      lastThemeFetch_ = now;
      refreshTheme();
    }
    // Nova is the single runtime theme authority. This lightweight object is
    // intentionally sampled faster than the config document so skip/pause is
    // visible on the next few rendered frames without duplicating rotation.
    if (now - lastThemeStateFetch_ >= 0.25) {
      lastThemeStateFetch_ = now;
      refreshThemeState();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void ConfigClient::refreshThemeState() {
  const HttpResponse response = httpGet(baseUrl_ + "/api/phonoscope/theme", {}, 2);
  if (!response.ok()) return;
  auto value = json::Value::parse(response.body);
  if (!value) return;
  const std::string selected = stringField(*value, "themeId");
  const std::string selectedGroup = stringField(*value, "groupId");
  const bool paused = value->find("paused") != nullptr &&
                      value->find("paused")->boolean().value_or(false);
  const uint64_t revision = static_cast<uint64_t>(numberField(*value, "revision", 0));
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.rotation.selectedThemeId = selected;
  snapshot_.rotation.selectedGroupId = selectedGroup;
  if (const json::Value* transition = value->find("transitionSeconds")) {
    snapshot_.rotation.selectedTransitionSeconds = transition->number();
  }
  snapshot_.rotation.paused = paused;
  snapshot_.rotation.revision = revision;
}

std::shared_ptr<const Module> ConfigClient::loadModule(const std::string& id,
                                                       const std::string& version,
                                                       const std::string& hash) {
  // Package uploads replace an installed module without requiring a semantic
  // version bump. Include the dashboard's content hash or an updated package at
  // the same id/version remains cached forever in the streamed renderer.
  const std::string key = id + "@" + version + "#" + hash;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == loadedModuleKey_ && loadedModule_) return loadedModule_;
  }

  const HttpResponse response =
      httpGet(baseUrl_ + "/api/phonoscope/modules/" + id + "/" + version + "/compiled");
  if (!response.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = "module fetch failed: " + (response.error.empty()
                                                ? std::to_string(response.status)
                                                : response.error);
    return nullptr;
  }
  auto module = Module::parse(response.body);
  if (!module) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = "compiled module " + key + " did not decode";
    return nullptr;
  }

  auto shared = std::make_shared<const Module>(std::move(*module));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loadedModuleKey_ = key;
    loadedModule_ = shared;
  }
  return shared;
}

bool ConfigClient::refreshConfiguration() {
  std::map<std::string, std::string> headers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configEtag_.empty()) headers["If-None-Match"] = configEtag_;
  }

  const HttpResponse response = httpGet(baseUrl_ + "/api/phonoscope/config", headers);
  if (response.status == 304) return true;
  if (!response.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = "config fetch failed: " +
                 (response.error.empty() ? std::to_string(response.status) : response.error);
    return false;
  }

  auto envelope = json::Value::parse(response.body);
  if (!envelope) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = "config response did not parse";
    return false;
  }
  const json::Value* config = envelope->find("config");
  if (config == nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = "config response had no config object";
    return false;
  }

  ConfigSnapshot next;
  next.activeModuleId = stringField(*config, "activeModuleId");
  next.activeModuleVersion = stringField(*config, "activeModuleVersion");
  next.transitionSeconds = numberField(*config, "transitionMs", 600) / 1000.0;
  next.message = stringField(*config, "message");

  // Message scale is a fully driven Phonoscope parameter on tvOS. Resolve it
  // through the same path under a private setting id so moving the text into
  // the encoded frame does not lose beat/random/envelope animation.
  PhonoscopeParameterSource messageScale;
  messageScale.type = "manual";
  messageScale.value = 1.0;
  if (const json::Value* encoded = config->find("messageScaleSource")) {
    messageScale = parameterSource(*encoded);
  }
  next.settings["__messageScale"] = 1.0;
  next.parameterSources["__messageScale"] = messageScale;
  if (messageScale.type != "manual" && messageScale.type != "fixed" &&
      !messageScale.type.empty()) {
    next.driverInterpolatedSettings.insert("__messageScale");
  }

  // The final glow overlay. Blur amount, opacity and blend mode are all driven
  // through the same private-setting path as the message scale.
  {
    auto privateSource = [&](const char* settingId, const json::Value* encoded, double fallback) {
      PhonoscopeParameterSource source;
      source.type = "manual";
      source.value = fallback;
      if (encoded != nullptr) source = parameterSource(*encoded);
      next.settings[settingId] = fallback;
      next.parameterSources[settingId] = source;
      if (source.type != "manual" && source.type != "fixed" && !source.type.empty()) {
        next.driverInterpolatedSettings.insert(settingId);
      }
    };
    const json::Value* glow = config->find("glowOverlay");
    privateSource("__glowBlur", glow != nullptr ? glow->find("blurSource") : nullptr, 0.0);
    privateSource("__glowOpacity", glow != nullptr ? glow->find("opacitySource") : nullptr, 0.0);
    // 0 is screen, 1 multiply and 2 overlay, so an absent block resolves to
    // screen -- the same default the dashboard applies.
    privateSource("__glowBlend", glow != nullptr ? glow->find("blendModeSource") : nullptr, 0.0);
  }

  if (next.activeModuleId.empty() || next.activeModuleVersion.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = "config names no active module";
    return false;
  }
  std::string activeModuleHash;
  if (const json::Value* modules = envelope->find("modules")) {
    if (const json::Array* items = modules->array()) {
      for (const json::Value& summary : *items) {
        if (stringField(summary, "id") == next.activeModuleId &&
            stringField(summary, "version") == next.activeModuleVersion) {
          activeModuleHash = stringField(summary, "hash");
          break;
        }
      }
    }
  }
  next.module = loadModule(next.activeModuleId, next.activeModuleVersion, activeModuleHash);
  if (!next.module) return false;

  // Settings: module defaults first, then the saved overrides.
  for (const ModuleSetting& setting : next.module->settings()) {
    next.settings[setting.id] = setting.defaultValue;
  }
  if (const json::Value* moduleSettings = config->find("moduleSettings")) {
    if (const json::Value* mine = moduleSettings->find(next.activeModuleId)) {
      if (const json::Object* values = mine->object()) {
        for (const auto& [key, value] : *values) {
          // Unknown/retired keys are not settings. In particular this keeps the
          // removed fluid_frame_rate control truly unwired even if stale
          // persisted preferences still contain it until their next save.
          if (next.settings.count(key) == 0) continue;
          if (auto number = value.number()) next.settings[key] = *number;
        }
      }
    }
  }
  // Clamp to the declaration, as the spec requires when a new module version
  // narrows a range under a previously saved value.
  for (const ModuleSetting& setting : next.module->settings()) {
    auto it = next.settings.find(setting.id);
    if (it == next.settings.end()) continue;
    it->second = clampValue(it->second, std::min(setting.min, setting.max),
                            std::max(setting.min, setting.max));
  }

  if (const json::Value* reloads = config->find("moduleReloadGenerations")) {
    if (const json::Value* mine = reloads->find(next.activeModuleId)) {
      next.reloadGeneration = static_cast<int>(mine->numberOr(0));
    }
  }

  // Palette: module slot defaults, then the selected colour group's themes.
  Palette basePalette = Palette::defaults();
  for (const PaletteSlotDeclaration& slot : next.module->paletteSlots()) {
    basePalette.set(slot.id, slot.defaultRgb);
  }
  next.palette = basePalette;

  std::string selectedGroupId;
  next.rotation.pinnedThemeId = stringField(*config, "editorPreviewColorThemeId");
  selectedGroupId = stringField(*config, "editorPreviewColorGroupId");
  if (const json::Value* groups = config->find("moduleColorGroupIds")) {
    if (selectedGroupId.empty()) {
      if (const json::Value* mine = groups->find(next.activeModuleId)) {
        selectedGroupId = mine->stringOr("");
      }
    }
  }
  if (const json::Value* colorGroups = config->find("colorGroups")) {
    if (const json::Array* items = colorGroups->array()) {
      for (const json::Value& group : *items) {
        if (!selectedGroupId.empty() && stringField(group, "id") != selectedGroupId) continue;
        if (stringField(group, "moduleId") != next.activeModuleId) continue;
        const json::Value* themes = group.find("themes");
        const json::Array* themeItems = themes != nullptr ? themes->array() : nullptr;
        if (themeItems == nullptr || themeItems->empty()) continue;

        // Every theme in the group is resolved here; which one is showing is a
        // question for the engine thread, which has the clock and the beat.
        next.rotation.changeMode = stringField(group, "changeMode", "interval");
        next.rotation.groupId = stringField(group, "id");
        next.rotation.order = stringField(group, "order", "sequential");
        next.rotation.waitSeconds = numberField(group, "waitSeconds", 12.0);
        next.rotation.transitionSeconds = numberField(group, "transitionSeconds", 1.5);
        for (const json::Value& theme : *themeItems) {
          Palette palette = basePalette;
          const json::Value* colors = theme.find("colors");
          if (const json::Object* slots = colors != nullptr ? colors->object() : nullptr) {
            for (const auto& [slot, colour] : *slots) palette.set(slot, colourVector(colour));
          }
          next.rotation.palettes.push_back(palette);
          next.rotation.themeIds.push_back(stringField(theme, "id"));

          std::unordered_map<std::string, ColorGroupRotation::ParameterSource> overrides;
          const json::Value* allOverrides = theme.find("parameterOverrides");
          const json::Value* moduleOverrides =
              allOverrides != nullptr ? allOverrides->find(next.activeModuleId) : nullptr;
          if (const json::Object* settings =
                  moduleOverrides != nullptr ? moduleOverrides->object() : nullptr) {
            for (const auto& [settingId, encodedSource] : *settings) {
              ColorGroupRotation::ParameterSource source;
              source.type = stringField(encodedSource, "type", "manual");
              auto optionalNumber = [&](std::string_view key) -> std::optional<double> {
                const json::Value* field = encodedSource.find(key);
                return field != nullptr ? field->number() : std::nullopt;
              };
              source.value = optionalNumber("value");
              source.min = optionalNumber("min");
              source.max = optionalNumber("max");
              source.cadence = stringField(encodedSource, "cadence", "beat");
              source.intervalSeconds = numberField(encodedSource, "intervalSeconds", 4.0);
              source.transitionSeconds = numberField(encodedSource, "transitionSeconds", 0.5);
              source.attackSeconds = numberField(encodedSource, "attackSeconds", 0.05);
              source.holdSeconds = numberField(encodedSource, "holdSeconds", 0.0);
              source.releaseSeconds = numberField(encodedSource, "releaseSeconds", 0.6);
              overrides.emplace(settingId, std::move(source));
            }
          }
          next.rotation.parameterOverrides.push_back(std::move(overrides));
        }
        // Seed with the first theme so a cold start is never colourless.
        next.palette = next.rotation.palettes.front();
        break;
      }
    }
  }

  // Parameter sources drive a setting continuously from the audio, so they must
  // be applied immediately rather than chased -- exactly the tvOS rule.
  if (const json::Value* sources = config->find("moduleParameterSources")) {
    if (const json::Value* mine = sources->find(next.activeModuleId)) {
      if (const json::Object* values = mine->object()) {
        for (const auto& [key, value] : *values) {
          if (next.settings.count(key) == 0) continue;
          const std::string type = stringField(value, "type", "fixed");
          ColorGroupRotation::ParameterSource source;
          source.type = type;
          auto optionalNumber = [&](std::string_view fieldName) -> std::optional<double> {
            const json::Value* field = value.find(fieldName);
            return field != nullptr ? field->number() : std::nullopt;
          };
          source.value = optionalNumber("value");
          source.min = optionalNumber("min");
          source.max = optionalNumber("max");
          source.cadence = stringField(value, "cadence", "beat");
          source.intervalSeconds = numberField(value, "intervalSeconds", 4.0);
          source.transitionSeconds = numberField(value, "transitionSeconds", 0.5);
          source.attackSeconds = numberField(value, "attackSeconds", 0.05);
          source.holdSeconds = numberField(value, "holdSeconds", 0.0);
          source.releaseSeconds = numberField(value, "releaseSeconds", 0.6);
          next.parameterSources.emplace(key, std::move(source));
          if (type != "manual" && type != "fixed" && !type.empty()) {
            next.driverInterpolatedSettings.insert(key);
          }
        }
      }
    }
  }

  // Does this module want the animated backdrop? Driven by the manifest, not by
  // the module id: `particle-ripples` declares `fluid_speed` with
  // `affects: [renderer.fluidBackground.speed]`, and any future module can opt
  // in the same way without this service learning its name.
  for (const ModuleSetting& setting : next.module->settings()) {
    for (const std::string& target : setting.affects) {
      if (target == "renderer.fluidBackground.speed") {
        next.usesFluidBackground = true;
        break;
      }
    }
    if (next.usesFluidBackground) break;
  }

  next.valid = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next.fluidTheme = fluidTheme_;
    // A config refresh and the faster runtime-theme poll are independent HTTP
    // requests. Preserve the last authoritative selection while swapping the
    // larger immutable config snapshot.
    next.rotation.selectedThemeId = snapshot_.rotation.selectedThemeId;
    next.rotation.selectedGroupId = snapshot_.rotation.selectedGroupId;
    next.rotation.selectedTransitionSeconds = snapshot_.rotation.selectedTransitionSeconds;
    next.rotation.paused = snapshot_.rotation.paused;
    next.rotation.revision = snapshot_.rotation.revision;
    snapshot_ = std::move(next);
    auto etag = response.headers.find("etag");
    configEtag_ = etag != response.headers.end() ? etag->second : std::string();
    lastError_.clear();
  }
  return true;
}

// The dashboard theme, for the animated backdrop's colours.
//
// Best-effort by design: this service depends on nothing, and an unreachable or
// malformed dashboard must leave it rendering with the previous (or default)
// theme rather than failing. `?variant=resolved` makes the server flatten the
// dark/light envelope, so the sun-driven "auto" rule stays implemented once.
void ConfigClient::refreshTheme() {
  std::map<std::string, std::string> headers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!themeEtag_.empty()) headers["If-None-Match"] = themeEtag_;
  }

  const HttpResponse response = httpGet(baseUrl_ + "/api/theme?variant=resolved", headers, 5);
  if (response.status == 304 || !response.ok()) return;

  auto envelope = json::Value::parse(response.body);
  if (!envelope) return;
  const json::Value* theme = envelope->find("theme");
  if (theme == nullptr || theme->object() == nullptr) return;

  // A dashboard that predates `?variant=resolved` ignores the parameter and
  // returns the un-flattened {selection, themes:{dark,light}} envelope, which
  // has none of these keys. Treat that as "no theme" rather than quietly
  // reporting success and rendering the backdrop in fallback colours.
  if (theme->find("accent") == nullptr && theme->find("background") == nullptr) return;

  FluidThemeSettings next;
  next.available = true;
  if (const json::Value* accent = theme->find("accent")) next.accent = colourVector(*accent);
  if (const json::Value* highlight = theme->find("highlight")) {
    next.highlight = colourVector(*highlight);
  }
  if (const json::Value* background = theme->find("background")) {
    next.background = colourVector(*background);
  }
  // Same /100 scaling and the same clamps as FluidBackgroundSettings(shared:)
  // on tvOS, so both engines land on identical uniforms.
  if (const json::Value* effect = theme->find("backgroundEffect")) {
    auto scaled = [&](std::string_view key, double fallbackPercent, double lowPercent,
                      double highPercent) {
      const double percent =
          clampValue(numberField(*effect, key, fallbackPercent), lowPercent, highPercent);
      return static_cast<float>(percent / 100.0);
    };
    next.peakIntensity = scaled("peakIntensity", 100, 40, 260);
    next.falloffPower = scaled("falloffPower", 160, 80, 320);
    next.warpAmplitude = scaled("warpAmplitude", 100, 40, 220);
    next.hueSpread = scaled("hueSpread", 50, 0, 100);
    next.apexGlow = scaled("apexGlow", 100, 0, 240);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  fluidTheme_ = next;
  snapshot_.fluidTheme = next;
  auto etag = response.headers.find("etag");
  themeEtag_ = etag != response.headers.end() ? etag->second : std::string();
}

void ConfigClient::refreshNowPlaying() {
  const double requestedAt = monotonicSeconds();
  const HttpResponse response = httpGet(baseUrl_ + "/api/phonoscope/now-playing", {}, 5);
  const double receivedAt = monotonicSeconds();
  if (!response.ok()) return;
  auto value = json::Value::parse(response.body);
  if (!value) return;

  const json::Value* track = value->find("track");
  NowPlaying next;
  next.playing = value->find("playing") != nullptr &&
                 (value->find("playing")->boolean().value_or(false));
  next.position = numberField(*value, "position", 0);
  next.duration = numberField(*value, "duration", 0);
  // The dashboard extrapolates position at response construction. Its exact
  // instant lies within this HTTP round trip; the midpoint is the unbiased
  // local monotonic estimate and lets the 120 Hz loop account for the return
  // leg rather than silently losing half an RTT.
  next.sampledAtMonotonic = (requestedAt + receivedAt) * 0.5;

  if (track != nullptr && track->object() != nullptr) {
    next.identity.appleMusicId = stringField(*track, "appleMusicId");
    next.identity.isrc = stringField(*track, "isrc");
    next.identity.title = stringField(*track, "title");
    next.identity.artist = stringField(*track, "artist");
    next.identity.album = stringField(*track, "album");
    next.identity.duration = numberField(*track, "duration", next.duration);
    next.identity.artworkUrl = stringField(*track, "artworkUrl");
    if (const json::Value* genres = track->find("genreNames")) {
      next.identity.genreNames = genres->stringArray();
    }
    next.valid = !next.identity.empty();
  }

  std::string trackKey;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    nowPlaying_ = next;
    trackKey = next.valid ? next.identity.key() : std::string();
    if (trackKey == resolvedTrackKey_) return;
  }
  if (!trackKey.empty()) resolveTrack(next.identity);
}

void ConfigClient::resolveTrack(const TrackIdentity& identity) {
  std::string body = "{";
  body += "\"title\":\"" + json::escape(identity.title) + "\",";
  body += "\"artist\":\"" + json::escape(identity.artist) + "\",";
  body += "\"album\":\"" + json::escape(identity.album) + "\",";
  body += "\"duration\":" + std::to_string(identity.duration);
  if (!identity.appleMusicId.empty()) {
    body += ",\"appleMusicId\":\"" + json::escape(identity.appleMusicId) + "\"";
  }
  if (!identity.isrc.empty()) body += ",\"isrc\":\"" + json::escape(identity.isrc) + "\"";
  body += "}";

  const HttpResponse response =
      httpPost(baseUrl_ + "/api/phonoscope/tracks/resolve", body, "application/json", {}, 20);
  if (!response.ok()) return;
  auto value = json::Value::parse(response.body);
  if (!value) return;
  const json::Value* node = value->find("analysis");
  if (node == nullptr) return;

  TrackAnalysis analysis;
  analysis.trackKey = stringField(*node, "trackKey");
  analysis.matched = node->find("matched") != nullptr &&
                     node->find("matched")->boolean().value_or(false);
  if (const json::Value* bpm = node->find("bpm")) {
    if (auto number = bpm->number()) analysis.bpm = *number;
  }
  analysis.beatOffset = numberField(*node, "beatOffset", 0);
  if (const json::Value* beats = node->find("beatTimes")) analysis.beatTimes = beats->numberArray();
  analysis.timeSignature = static_cast<int>(numberField(*node, "timeSignature", 4));
  if (const json::Value* energy = node->find("energy")) {
    if (auto number = energy->number()) analysis.energy = *number;
  }
  if (const json::Value* valence = node->find("valence")) {
    if (auto number = valence->number()) analysis.valence = *number;
  }
  if (const json::Value* lyrics = node->find("lyrics")) {
    if (const json::Array* items = lyrics->array()) {
      for (const json::Value& item : *items) {
        TimedLyric lyric;
        lyric.time = numberField(item, "time", 0);
        lyric.text = stringField(item, "text");
        analysis.lyrics.push_back(std::move(lyric));
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  analysis_ = std::move(analysis);
  resolvedTrackKey_ = identity.key();
}

ConfigSnapshot ConfigClient::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

NowPlaying ConfigClient::nowPlaying() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return nowPlaying_;
}

TrackAnalysis ConfigClient::analysis() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return analysis_;
}

}  // namespace nova::net
