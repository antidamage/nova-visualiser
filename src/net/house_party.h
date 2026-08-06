// House-party lighting producer.
//
// This loop used to run on the Apple TV (`PhonoscopeStore.transmitHousePartyFrame`
// and friends). It moves here because the renderer now owns the signal and the
// active palette, and because lighting should not stop when the TV app is
// backgrounded. The server side is unchanged: the same
// `/api/phonoscope/house-party/*` endpoints, the same lease semantics, the same
// zone suppression and restore in `lib/house-party-coordinator.ts`. Only the
// producer moved.
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "core/palette.h"
#include "core/signal.h"

namespace nova::net {

class HousePartyProducer {
 public:
  HousePartyProducer() = default;
  ~HousePartyProducer();

  HousePartyProducer(const HousePartyProducer&) = delete;
  HousePartyProducer& operator=(const HousePartyProducer&) = delete;

  void start(std::string baseUrl);
  void stop();

  // Called from the engine each tick with the live signal and palette. Cheap:
  // it only stores values, the network loop runs on its own thread.
  // `hueOffsetDegrees` is the resolved `__hueOffset` effect. It is resolved
  // here rather than by the dashboard because only this engine holds the
  // spectrum a bass or energy driver reads.
  void update(const SignalFrame& signal, const Palette& palette, double hueOffsetDegrees);

  void setEnabled(bool enabled);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  bool active() const { return sessionActive_.load(std::memory_order_relaxed); }

 private:
  void run();
  bool beginSession();
  void endSession();
  bool sendFrame();

  std::string baseUrl_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> sessionActive_{false};

  std::mutex mutex_;
  SignalFrame signal_;
  Palette palette_;
  // The resolved `__hueOffset` effect, sampled with the signal it belongs to.
  double hueOffsetDegrees_ = 0;
  std::string sessionId_;
  int sequence_ = 0;
  double smoothedLocalBrightness_ = 50;
  double smoothedCloudBrightness_ = 50;
};

}  // namespace nova::net
