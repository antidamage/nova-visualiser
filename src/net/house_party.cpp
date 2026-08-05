#include "net/house_party.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/json.h"
#include "net/http_client.h"

namespace nova::net {
namespace {

// The dashboard leases a session for 5 s (HOUSE_PARTY_LEASE_MS), so frames have
// to arrive comfortably inside that or the lighting restores itself.
constexpr double kFrameIntervalSeconds = 0.2;

int channel(float value) {
  return static_cast<int>(std::lround(clampValue(value, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

HousePartyProducer::~HousePartyProducer() { stop(); }

void HousePartyProducer::start(std::string baseUrl) {
  baseUrl_ = std::move(baseUrl);
  if (running_.exchange(true)) return;
  thread_ = std::thread(&HousePartyProducer::run, this);
}

void HousePartyProducer::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (sessionActive_.load()) endSession();
}

void HousePartyProducer::setEnabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
}

void HousePartyProducer::update(const SignalFrame& signal, const Palette& palette) {
  std::lock_guard<std::mutex> lock(mutex_);
  signal_ = signal;
  palette_ = palette;
}

void HousePartyProducer::run() {
  while (running_.load(std::memory_order_relaxed)) {
    const bool wanted = enabled_.load(std::memory_order_relaxed);
    if (wanted && !sessionActive_.load()) {
      if (!beginSession()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
    } else if (!wanted && sessionActive_.load()) {
      endSession();
    }

    if (sessionActive_.load()) {
      if (!sendFrame()) {
        // A rejected frame means the lease lapsed or the dashboard restarted;
        // drop the session and let the next pass start a fresh one rather than
        // spraying frames at a session id the server has forgotten.
        sessionActive_.store(false);
      }
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(kFrameIntervalSeconds * 1000)));
  }
}

bool HousePartyProducer::beginSession() {
  const HttpResponse response =
      httpPost(baseUrl_ + "/api/phonoscope/house-party/session", "{}", "application/json", {}, 10);
  if (!response.ok()) return false;
  auto value = json::Value::parse(response.body);
  if (!value) return false;
  const json::Value* id = value->find("id");
  if (id == nullptr) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  sessionId_ = id->stringOr("");
  sequence_ = 0;
  sessionActive_.store(!sessionId_.empty());
  return sessionActive_.load();
}

void HousePartyProducer::endSession() {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    id = sessionId_;
    sessionId_.clear();
  }
  sessionActive_.store(false);
  if (id.empty()) return;
  // The dashboard restores the captured lighting state when the session ends,
  // so this must be attempted even during shutdown.
  httpPost(baseUrl_ + "/api/phonoscope/house-party/session/" + id + "?end=1", "{}",
           "application/json", {}, 5);
}

bool HousePartyProducer::sendFrame() {
  SignalFrame signal;
  Palette palette;
  std::string id;
  int sequence = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    signal = signal_;
    palette = palette_;
    id = sessionId_;
    sequence = ++sequence_;
  }
  if (id.empty()) return false;

  const Vec4 primary = palette.color("primary");
  const Vec4 secondary = palette.color("secondary");
  // Follow the beat with the same shaping the tvOS producer used: the peak
  // colour leans towards Secondary on the pulse so the room flashes with the
  // crest rather than sitting at a flat colour.
  const float pulse = static_cast<float>(clampValue(signal.beatPulse, 0.0, 1.0));
  const Vec4 peak = mix(primary, secondary, pulse);

  const double targetLocal = 25 + 65 * clampValue(signal.beatPulse * (0.5 + signal.energy), 0.0, 1.0);
  const double targetCloud = 20 + 55 * clampValue(signal.beatPulse * (0.5 + signal.energy), 0.0, 1.0);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Cloud bulbs respond far more slowly than local ones; smoothing them
    // harder stops the cloud queue backing up behind unreachable targets.
    smoothedLocalBrightness_ += (targetLocal - smoothedLocalBrightness_) * 0.5;
    smoothedCloudBrightness_ += (targetCloud - smoothedCloudBrightness_) * 0.2;
  }

  double localBrightness = 0;
  double cloudBrightness = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    localBrightness = smoothedLocalBrightness_;
    cloudBrightness = smoothedCloudBrightness_;
  }

  std::string body = "{";
  body += "\"sequence\":" + std::to_string(sequence) + ",";
  body += "\"peakRgb\":[" + std::to_string(channel(peak.x)) + "," +
          std::to_string(channel(peak.y)) + "," + std::to_string(channel(peak.z)) + "],";
  body += "\"peakBrightnessPct\":" + std::to_string(localBrightness) + ",";
  body += "\"cloudPeakBrightnessPct\":" + std::to_string(cloudBrightness) + ",";
  body += "\"transitionSeconds\":0.2,";
  body += "\"hueMode\":\"follow\",";
  body += "\"brightnessMode\":\"follow\",";
  body += "\"ambient\":" + std::string(signal.playing ? "false" : "true") + ",";
  body += "\"themeTransitionSeconds\":0.6,";
  body += "\"clock\":{";
  body += "\"trackKey\":null,";
  body += "\"position\":" + std::to_string(signal.time) + ",";
  body += "\"duration\":" + std::to_string(signal.duration) + ",";
  body += "\"playing\":" + std::string(signal.playing ? "true" : "false") + ",";
  body += "\"sampledAtMs\":" +
          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count());
  body += "}}";

  const HttpResponse response = httpPost(
      baseUrl_ + "/api/phonoscope/house-party/session/" + id, body, "application/json", {}, 5);
  return response.ok();
}

}  // namespace nova::net
