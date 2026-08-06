#include "net/config_client.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <unordered_set>

#include "core/centre_image_reference.h"
#include "core/json.h"
#include "gfx/image_decode.h"
#include "net/http_client.h"

namespace nova::net {
namespace {

// The synthetic rotation entry a soloed colour theme is pinned to. Prefixed so
// it can never collide with an id the dashboard generates.
constexpr const char* kSoloEntryId = "__solo";

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

// Every driver field is always present on the wire, so an absent block is a
// plain beat driver rather than an error. `every` is clamped the same way the
// dashboard clamps it, and `offset` only means anything inside its own cycle.
Driver driverFrom(const json::Value* value) {
  Driver driver;
  if (value == nullptr) return driver;
  driver.type = stringField(*value, "type", "beat");
  driver.every = std::max(1, std::min(16, static_cast<int>(numberField(*value, "every", 1))));
  driver.offset = std::max(
      0, std::min(driver.every - 1, static_cast<int>(numberField(*value, "offset", 0))));
  driver.intervalSeconds = numberField(*value, "intervalSeconds", 4.0);
  driver.cadence = stringField(*value, "cadence", "beat");
  driver.transitionSeconds = numberField(*value, "transitionSeconds", 0.5);
  return driver;
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
  const std::string selectedEntry = stringField(*value, "entryId");
  const std::string selected = stringField(*value, "themeId");
  const std::string selectedGroup = stringField(*value, "groupId");
  std::vector<std::string> selectedSettingsGroupIds;
  if (const json::Value* ids = value->find("settingsGroupIds")) {
    if (const json::Array* items = ids->array()) {
      for (const json::Value& id : *items) selectedSettingsGroupIds.push_back(id.stringOr(""));
    }
  }
  const bool paused = value->find("paused") != nullptr &&
                      value->find("paused")->boolean().value_or(false);
  const uint64_t revision = static_cast<uint64_t>(numberField(*value, "revision", 0));
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.rotation.selectedEntryId = selectedEntry;
  snapshot_.rotation.selectedThemeId = selected;
  snapshot_.rotation.selectedGroupId = selectedGroup;
  snapshot_.rotation.selectedSettingsGroupIds = std::move(selectedSettingsGroupIds);
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

std::shared_ptr<const DecodedImage> ConfigClient::centreImage(const std::string& url) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cached = imageCache_.find(url);
    if (cached != imageCache_.end()) return cached->second;
  }

  // Absolute URLs are left alone so an image could be served from elsewhere;
  // the dashboard's own are relative and get the base prepended.
  const std::string absolute = url.rfind("http", 0) == 0 ? url : baseUrl_ + url;
  const HttpResponse response = httpGet(absolute);
  std::shared_ptr<const DecodedImage> image;
  if (!response.ok()) {
    std::fprintf(stderr, "nova-visualiser: centre image fetch failed (%s): %s\n", url.c_str(),
                 response.error.empty() ? std::to_string(response.status).c_str()
                                        : response.error.c_str());
  } else {
    std::string error;
    image = gfx::decodePng(response.body, url, error);
    if (!image) {
      std::fprintf(stderr, "nova-visualiser: centre image decode failed (%s): %s\n", url.c_str(),
                   error.c_str());
    }
  }

  // A failure is cached as a null too. Retrying a broken image on every config
  // read would hammer the dashboard for a picture that is not going to appear;
  // the `?v=` in the URL means a re-upload gets a fresh attempt anyway.
  std::lock_guard<std::mutex> lock(mutex_);
  imageCache_[url] = image;
  return image;
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

  // Centre images. The configuration stores library ids; the envelope resolves
  // them to fetchable URLs, so this service never has to know how the dashboard
  // lays its data directory out.
  std::unordered_map<std::string, std::string> imageUrlsById;
  if (const json::Value* urls = envelope->find("centreImageUrls")) {
    if (const json::Object* entries = urls->object()) {
      for (const auto& [id, url] : *entries) imageUrlsById[id] = url.stringOr("");
    }
  }
  auto imageForId = [&](const std::string& id) -> std::shared_ptr<const DecodedImage> {
    if (id.empty()) return nullptr;
    const auto url = imageUrlsById.find(id);
    if (url == imageUrlsById.end() || url->second.empty()) return nullptr;
    return centreImage(url->second);
  };

  // Picture-level effects. Household configuration, declared by no module
  // manifest, so their declarations are synthesised here and resolved through
  // exactly the same lanes as every module setting. Seeded at their defaults so
  // an unbound effect still has a value.
  next.settings["__messageScale"] = 1.0;
  // The centre image's base height, as a percentage of the frame. A separate
  // axis from the scale above: this is how big the image is, that is a
  // multiplier a driver lane can sweep on top of it.
  next.settings["__centreHeight"] = kCentreImageDefaultHeightPercent;
  next.settings["__glowBlur"] = 0.0;
  // Opacity 0 is the identity, and it is what both engines check to skip the
  // glow pass entirely.
  next.settings["__glowOpacity"] = 0.0;
  // 0 screen, 1 multiply, 2 overlay.
  next.settings["__glowBlend"] = 0.0;
  // Degrees of random hue jitter per House Party light. Resolved here rather
  // than by the dashboard because only this engine holds the spectrum a bass or
  // energy driver reads; the value rides out on the lighting frame.
  next.settings["__hueOffset"] = 5.0;
  // Frame geometry, as a percentage of the render view: the fixed letterbox
  // these replaced, so an unbound picture is the one that was always drawn.
  next.settings["__bgHeight"] = 33.0;
  next.settings["__bgWidth"] = 100.0;
  next.settings["__vignetteOpacity"] = 96.0;
  next.settings["__vignetteSize"] = 1.0;
  // 0 linear, 1 screen, 2 overlay, 3 multiply. Linear is the original term.
  next.settings["__sceneBlend"] = 0.0;

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
  next.rotation.pinnedEntryId = stringField(*config, "editorPreviewColorEntryId");
  selectedGroupId = stringField(*config, "editorPreviewColorGroupId");
  if (const json::Value* groups = config->find("moduleColorGroupIds")) {
    if (selectedGroupId.empty()) {
      if (const json::Value* mine = groups->find(next.activeModuleId)) {
        selectedGroupId = mine->stringOr("");
      }
    }
  }

  // The flat colour theme library, keyed for the entry lookup below. Themes are
  // colour only now: behaviour comes from whichever settings groups the entry
  // names, which is what lets one palette run under several sets of drivers.
  std::unordered_map<std::string, Palette> palettesByThemeId;
  // A theme may also supply the picture's centrepiece. Resolved alongside the
  // palette so a rotation entry carries both, and the two can never tear.
  std::unordered_map<std::string, std::shared_ptr<const DecodedImage>> imagesByThemeId;
  if (const json::Value* themes = config->find("colorThemes")) {
    if (const json::Array* items = themes->array()) {
      for (const json::Value& theme : *items) {
        if (stringField(theme, "moduleId") != next.activeModuleId) continue;
        Palette palette = basePalette;
        const json::Value* colors = theme.find("colors");
        if (const json::Object* slots = colors != nullptr ? colors->object() : nullptr) {
          for (const auto& [slot, colour] : *slots) palette.set(slot, colourVector(colour));
        }
        const std::string themeId = stringField(theme, "id");
        if (auto image = imageForId(stringField(theme, "imageId"))) {
          imagesByThemeId.emplace(themeId, std::move(image));
        }
        palettesByThemeId.emplace(themeId, std::move(palette));
      }
    }
  }
  auto imageForTheme = [&](const std::string& themeId) -> std::shared_ptr<const DecodedImage> {
    const auto found = imagesByThemeId.find(themeId);
    return found != imagesByThemeId.end() ? found->second : nullptr;
  };

  if (const json::Value* colorGroups = config->find("colorGroups")) {
    if (const json::Array* items = colorGroups->array()) {
      for (const json::Value& group : *items) {
        if (!selectedGroupId.empty() && stringField(group, "id") != selectedGroupId) continue;
        if (stringField(group, "moduleId") != next.activeModuleId) continue;
        const json::Value* entries = group.find("entries");
        const json::Array* entryItems = entries != nullptr ? entries->array() : nullptr;
        if (entryItems == nullptr || entryItems->empty()) continue;

        // Every entry is resolved here; which one is showing is Nova's answer,
        // arriving on the faster /api/phonoscope/theme poll.
        next.rotation.groupId = stringField(group, "id");
        for (const json::Value& entry : *entryItems) {
          ColorGroupRotation::Entry resolved;
          resolved.id = stringField(entry, "id");
          resolved.themeId = stringField(entry, "themeId");
          if (const json::Value* ids = entry.find("settingsGroupIds")) {
            if (const json::Array* idItems = ids->array()) {
              for (const json::Value& id : *idItems) {
                resolved.settingsGroupIds.push_back(id.stringOr(""));
              }
            }
          }
          auto palette = palettesByThemeId.find(resolved.themeId);
          next.rotation.palettes.push_back(
              palette != palettesByThemeId.end() ? palette->second : basePalette);
          next.rotation.images.push_back(imageForTheme(resolved.themeId));
          next.rotation.entries.push_back(std::move(resolved));
        }
        // Seed with the first entry so a cold start is never colourless.
        next.palette = next.rotation.palettes.front();
        break;
      }
    }
  }

  // Solo: hold the picture on one colour theme, whether or not the rotation
  // playlist contains it. The theme library above is keyed by id independently
  // of the playlist, so a synthetic entry can be appended and pinned -- which
  // reuses the pin the engine already treats as authoritative over the
  // rotation, rather than adding a second way to override it.
  const std::string soloThemeId = stringField(*config, "soloColorThemeId");
  if (!soloThemeId.empty()) {
    const auto solo = palettesByThemeId.find(soloThemeId);
    if (solo != palettesByThemeId.end()) {
      ColorGroupRotation::Entry entry;
      entry.id = kSoloEntryId;
      entry.themeId = soloThemeId;
      // Deliberately no settings groups: which settings run is Nova's answer,
      // arriving as `selectedSettingsGroupIds` on the faster theme poll, and it
      // already carries any settings-group solo.
      next.rotation.entries.push_back(std::move(entry));
      next.rotation.palettes.push_back(solo->second);
      next.rotation.images.push_back(imageForTheme(soloThemeId));
      next.rotation.pinnedEntryId = kSoloEntryId;
      next.palette = solo->second;
    }
  }

  // The settings group library. Lanes drive settings continuously from the
  // audio, so they are applied immediately rather than chased -- exactly the
  // tvOS rule the old parameter sources followed.
  if (const json::Value* groups = config->find("settingsGroups")) {
    if (const json::Array* items = groups->array()) {
      for (const json::Value& raw : *items) {
        if (stringField(raw, "moduleId") != next.activeModuleId) continue;
        SettingsGroup group;
        group.id = stringField(raw, "id");
        group.name = stringField(raw, "name");
        group.moduleId = stringField(raw, "moduleId");
        group.isDefault = raw.find("isDefault") != nullptr &&
                          raw.find("isDefault")->boolean().value_or(false);
        if (const json::Value* combine = raw.find("combine")) {
          if (const json::Object* modes = combine->object()) {
            for (const auto& [effect, mode] : *modes) {
              group.combine[effect] = mode.stringOr("add") == "strongest"
                                          ? CombineMode::Strongest
                                          : CombineMode::Add;
            }
          }
        }
        if (const json::Value* statics = raw.find("staticSettings")) {
          if (const json::Object* values = statics->object()) {
            for (const auto& [id, value] : *values) {
              group.staticSettings[id] = value.numberOr(0);
            }
          }
        }
        if (const json::Value* lanes = raw.find("lanes")) {
          if (const json::Array* laneItems = lanes->array()) {
            for (const json::Value& rawLane : *laneItems) {
              DriverLane lane;
              lane.id = stringField(rawLane, "id");
              lane.driver = driverFrom(rawLane.find("driver"));
              if (const json::Value* modifiers = rawLane.find("modifiers")) {
                if (const json::Array* modifierItems = modifiers->array()) {
                  for (const json::Value& modifier : *modifierItems) {
                    lane.modifiers.push_back(driverFrom(&modifier));
                  }
                }
              }
              if (const json::Value* bindings = rawLane.find("bindings")) {
                if (const json::Array* bindingItems = bindings->array()) {
                  for (const json::Value& rawBinding : *bindingItems) {
                    EffectBinding binding;
                    binding.id = stringField(rawBinding, "id");
                    binding.effect = stringField(rawBinding, "effect");
                    // An effect the active module does not declare cannot be
                    // resolved, so it is dropped rather than carried as a
                    // binding that never fires.
                    if (next.settings.count(binding.effect) == 0) continue;
                    auto optional = [&](std::string_view key, double& target, bool& present) {
                      const json::Value* field = rawBinding.find(key);
                      if (field == nullptr) return;
                      if (auto number = field->number()) {
                        target = *number;
                        present = true;
                      }
                    };
                    optional("min", binding.min, binding.hasMin);
                    optional("max", binding.max, binding.hasMax);
                    optional("attackSeconds", binding.attackSeconds, binding.hasAttack);
                    optional("holdSeconds", binding.holdSeconds, binding.hasHold);
                    optional("releaseSeconds", binding.releaseSeconds, binding.hasRelease);
                    if (const json::Value* params = rawBinding.find("params")) {
                      if (const json::Object* values = params->object()) {
                        for (const auto& [key, value] : *values) {
                          binding.params[key] = value.numberOr(0);
                        }
                      }
                    }
                    lane.bindings.push_back(std::move(binding));
                  }
                }
              }
              if (!lane.bindings.empty()) group.lanes.push_back(std::move(lane));
            }
          }
        }
        next.settingsGroups.push_back(std::move(group));
      }
    }
  }
  // Anything a lane can move is interpolated between config snapshots; the
  // engine decides frame to frame which of them a lane is actually driving.
  for (const SettingsGroup& group : next.settingsGroups) {
    for (const DriverLane& lane : group.lanes) {
      for (const EffectBinding& binding : lane.bindings) {
        next.driverInterpolatedSettings.insert(binding.effect);
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
    next.rotation.selectedEntryId = snapshot_.rotation.selectedEntryId;
    next.rotation.selectedThemeId = snapshot_.rotation.selectedThemeId;
    next.rotation.selectedGroupId = snapshot_.rotation.selectedGroupId;
    next.rotation.selectedSettingsGroupIds = snapshot_.rotation.selectedSettingsGroupIds;
    next.rotation.selectedTransitionSeconds = snapshot_.rotation.selectedTransitionSeconds;
    next.rotation.paused = snapshot_.rotation.paused;
    next.rotation.revision = snapshot_.rotation.revision;
    snapshot_ = std::move(next);
    // Drop decoded images nothing in the new snapshot refers to. Uploads
    // accumulate in the library and a 4K RGBA buffer is 32 MB, so a cache that
    // only ever grew would be a slow leak keyed on how often somebody changes
    // their mind.
    {
      std::unordered_set<std::string> live;
      for (const auto& image : snapshot_.rotation.images) {
        if (image) live.insert(image->sourceUrl);
      }
      for (auto entry = imageCache_.begin(); entry != imageCache_.end();) {
        // A cached null is a fetch that failed; keeping it is what stops the
        // retry storm, so it survives the sweep.
        const bool keep = entry->second == nullptr || live.count(entry->first) != 0;
        entry = keep ? std::next(entry) : imageCache_.erase(entry);
      }
    }
    auto etag = response.headers.find("etag");
    configEtag_ = etag != response.headers.end() ? etag->second : std::string();
    lastError_.clear();
    revision_.fetch_add(1, std::memory_order_relaxed);
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
