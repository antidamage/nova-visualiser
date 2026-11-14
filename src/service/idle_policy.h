#pragma once

// When the service ends itself for want of anything to do (specs/idle-exit.md).
//
// Pure: the caller supplies the clock and what counts as activity, so the rule
// is testable without a GPU, a socket or a dashboard.

namespace nova::service {

struct IdleExitPolicy {
  // 0 disables the exit entirely.
  double idleExitSeconds = 0;
  // House-party lighting is driven from this process; ending it would cut the
  // lights off, so the exit never applies while it is enabled.
  bool housePartyEnabled = false;
  // Time of the last activity. Starts at launch, so a wake that no viewer
  // follows ends after the same idle period.
  double lastActive = 0;

  void start(double now) { lastActive = now; }

  // Activity is a viewer (TCP or SRT client, browser reader) or music playing.
  // A control-port request is deliberately not activity.
  bool shouldExit(double now, bool viewerConnected, bool musicPlaying) {
    if (viewerConnected || musicPlaying) {
      lastActive = now;
      return false;
    }
    if (idleExitSeconds <= 0 || housePartyEnabled) return false;
    return now - lastActive >= idleExitSeconds;
  }
};

}  // namespace nova::service
