// The music signal the simulation reacts to.
//
// Port of `PhonoscopeSignalFrame` plus `PhonoscopeStore.makeSignal`. On tvOS
// the frame was derived locally from MusicKit; here it is derived on the server
// from the now-playing uplink plus the resolved track analysis, and advanced
// against the master clock so the streamed render, the tvOS fallback engine and
// house-party lighting all agree on where in the song they are.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nova {

enum class SignalQuality : int {
  Idle = 0,
  Metadata = 1,
  Bpm = 2,
  Timeline = 3,
  Live = 4,
};

struct TimedLyric {
  double time = 0;
  std::string text;
};

struct TrackIdentity {
  std::string appleMusicId;
  std::string isrc;
  std::string title;
  std::string artist;
  std::string album;
  double duration = 0;
  std::string artworkUrl;
  std::vector<std::string> genreNames;

  std::string key() const;
  bool empty() const { return title.empty() && artist.empty(); }
};

struct TrackAnalysis {
  std::string trackKey;
  bool matched = false;
  std::optional<double> bpm;
  double beatOffset = 0;
  std::vector<double> beatTimes;
  int timeSignature = 4;
  std::optional<double> energy;
  std::optional<double> valence;
  std::vector<TimedLyric> lyrics;
};

struct SignalFrame {
  double time = 0;
  double delta = 1.0 / 60.0;
  double duration = 0;
  double progress = 0;
  bool playing = false;
  double bpm = 72;
  double beatPhase = 0;
  double beatPulse = 0;
  int beatIndex = 0;
  double barPhase = 0;
  int barIndex = 0;
  int timeSignature = 4;
  double downbeatPulse = 0;
  double energy = 0.28;
  double valence = 0.5;
  double lyricProgress = 0;
  double lyricPulse = 0;
  int lyricIndex = -1;
  std::string lyricCurrent;
  std::string lyricNext;
  std::array<float, 32> spectrum{};
  SignalQuality quality = SignalQuality::Idle;
  uint64_t trackSeed = 0x4e4f5641;

  static SignalFrame idle();

  // Free-running advance used between master-clock samples. Port of
  // `PhonoscopeSimulation.advanceSignal(by:)`.
  void advance(double delta);
};

// Builds a signal frame from an absolute track position. Port of
// `PhonoscopeStore.makeSignal`. `previousLyricIndex` and `previousLyricPulse`
// carry the one piece of frame-to-frame state that function relied on.
SignalFrame buildSignal(double position, bool playing, const TrackIdentity* identity,
                        const TrackAnalysis* analysis, double delta, int& previousLyricIndex,
                        double previousLyricPulse);

std::array<float, 32> syntheticSpectrum(double position, double beatPhase, double beatPulse,
                                        double energy, uint64_t seed);

uint64_t stableSeed(const std::string& value);

}  // namespace nova
