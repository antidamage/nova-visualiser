#include "core/signal.h"

#include <algorithm>
#include <cmath>

namespace nova {

std::string TrackIdentity::key() const {
  if (!appleMusicId.empty()) return appleMusicId;
  if (!isrc.empty()) return isrc;
  return artist + "|" + title;
}

SignalFrame SignalFrame::idle() {
  SignalFrame frame;
  frame.spectrum.fill(0.0f);
  return frame;
}

void SignalFrame::advance(double deltaSeconds) {
  if (!playing) {
    delta = deltaSeconds;
    return;
  }
  time += deltaSeconds;
  delta = deltaSeconds;
  progress = duration > 0 ? std::min(1.0, time / duration) : 0;
  const double beatLength = 60 / std::max(20.0, bpm);
  const double beatValue = time / beatLength;
  beatIndex = static_cast<int>(std::floor(beatValue));
  beatPhase = beatValue - std::floor(beatValue);
  beatPulse = std::pow(std::max(0.0, 1 - beatPhase), 5);
  constexpr int signature = 4;
  barIndex = beatIndex / signature;
  barPhase = (static_cast<double>(beatIndex % signature) + beatPhase) / signature;
  downbeatPulse = beatIndex % signature == 0 ? beatPulse : 0;
}

uint64_t stableSeed(const std::string& value) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : value) {
    hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
  }
  return hash;
}

std::array<float, 32> syntheticSpectrum(double position, double /*beatPhase*/, double beatPulse,
                                        double energy, uint64_t seed) {
  std::array<float, 32> spectrum{};
  for (int index = 0; index < 32; ++index) {
    const double band = static_cast<double>(index) / 31;
    const double wave =
        0.5 + 0.5 * std::sin(position * (1.7 + band * 4.2) +
                             static_cast<double>((seed >> (index % 16)) & 0xff) * 0.017);
    const double bass = std::exp(-band * 4) * beatPulse;
    spectrum[static_cast<size_t>(index)] =
        static_cast<float>(std::min(1.0, std::max(0.0, (wave * 0.35 + bass * 0.8) * (0.4 + energy))));
  }
  return spectrum;
}

SignalFrame buildSignal(double position, bool playing, const TrackIdentity* identity,
                        const TrackAnalysis* analysis, double delta, int& previousLyricIndex,
                        double previousLyricPulse) {
  // Nothing playing means nothing to visualise. Both engines used to keep
  // synthesising beats and a full spectrum off an advancing idle position, so
  // an idle screen animated as hard as a playing one -- driving every
  // audio-reactive parameter, every effect trigger and a full-rate encode for
  // no audio at all. Idle is now genuinely idle: an inert frame. Callers that
  // want a resting drift get it from a driver's floor, not from a fake beat.
  //
  // PARITY: mirrored in `PhonoscopeStore.makeSignal` on tvOS. See
  // PHONOSCOPE_MODULE_SPEC.md §11 -- the two engines must agree tick for tick.
  if (!playing) {
    SignalFrame idle;
    idle.spectrum.fill(0.0f);
    idle.playing = false;
    idle.delta = delta;
    idle.time = position;
    idle.duration = identity != nullptr ? identity->duration : 0;
    // bpm/valence/quality keep their canonical idle-frame values; only the
    // audio-reactive channels are zeroed. Every field here must match the tvOS
    // guard exactly or the conformance digests diverge.
    idle.bpm = 72;
    idle.energy = 0;
    idle.valence = 0.5;
    // Keep the track seed stable so a pause does not reshuffle anything that
    // derives geometry from it.
    idle.trackSeed = stableSeed(
        identity != nullptr
            ? identity->artist + "|" + identity->title + "|" + std::to_string(identity->duration)
            : std::string("ambient"));
    previousLyricIndex = -1;
    idle.lyricIndex = -1;
    return idle;
  }

  SignalFrame frame;

  const std::string seedSource =
      identity != nullptr
          ? identity->artist + "|" + identity->title + "|" + std::to_string(identity->duration)
          : std::string("ambient");
  const uint64_t seed = stableSeed(seedSource);
  const double fallbackBpm = 72 + static_cast<double>(seed % 61);
  const double bpm = (analysis != nullptr && analysis->bpm) ? *analysis->bpm : fallbackBpm;
  const double beatLength = 60 / std::max(20.0, bpm);

  int beatIndex = 0;
  double beatPhase = 0;
  double beatPulse = 0;
  if (analysis != nullptr && !analysis->beatTimes.empty()) {
    const std::vector<double>& beatTimes = analysis->beatTimes;
    // Upper bound: first index whose time is greater than `position`.
    size_t lower = 0;
    size_t upper = beatTimes.size();
    while (lower < upper) {
      const size_t middle = (lower + upper) / 2;
      if (beatTimes[middle] <= position) {
        lower = middle + 1;
      } else {
        upper = middle;
      }
    }
    const long resolvedIndex = static_cast<long>(lower) - 1;
    if (resolvedIndex >= 0) {
      const size_t current = static_cast<size_t>(resolvedIndex);
      beatIndex = static_cast<int>(resolvedIndex);
      const double nextTime = current + 1 < beatTimes.size() ? beatTimes[current + 1]
                                                            : beatTimes[current] + beatLength;
      beatPhase = std::min(1.0, std::max(0.0, (position - beatTimes[current]) /
                                                  std::max(0.05, nextTime - beatTimes[current])));
      beatPulse = std::pow(std::max(0.0, 1 - beatPhase), 5);
    } else {
      beatIndex = 0;
      beatPhase = 1;
      beatPulse = 0;
    }
  } else {
    const double adjusted = std::max(0.0, position - (analysis != nullptr ? analysis->beatOffset : 0));
    const double beatValue = adjusted / beatLength;
    beatIndex = static_cast<int>(std::floor(beatValue));
    beatPhase = beatValue - std::floor(beatValue);
    beatPulse = std::pow(std::max(0.0, 1 - beatPhase), 5);
  }

  const int timeSignature = std::max(1, analysis != nullptr ? analysis->timeSignature : 4);
  frame.timeSignature = timeSignature;
  frame.barIndex = beatIndex / timeSignature;
  frame.barPhase =
      (static_cast<double>(beatIndex % timeSignature) + beatPhase) / static_cast<double>(timeSignature);
  frame.downbeatPulse = beatIndex % timeSignature == 0 ? beatPulse : 0;

  static const std::vector<TimedLyric> kNoLyrics;
  const std::vector<TimedLyric>& lyrics = analysis != nullptr ? analysis->lyrics : kNoLyrics;
  int lyricIndex = -1;
  for (size_t index = 0; index < lyrics.size(); ++index) {
    if (lyrics[index].time <= position) lyricIndex = static_cast<int>(index);
  }
  const size_t nextIndex = static_cast<size_t>(lyricIndex + 1);
  const double currentTime = lyricIndex >= 0 ? lyrics[static_cast<size_t>(lyricIndex)].time : 0;
  const double nextTime =
      nextIndex < lyrics.size()
          ? lyrics[nextIndex].time
          : std::max(currentTime + 4, identity != nullptr ? identity->duration : currentTime + 4);
  frame.lyricProgress =
      std::min(1.0, std::max(0.0, (position - currentTime) / std::max(0.01, nextTime - currentTime)));
  frame.lyricPulse = (lyricIndex != previousLyricIndex && lyricIndex >= 0)
                         ? 1
                         : std::max(0.0, previousLyricPulse - delta * 3);
  previousLyricIndex = lyricIndex;

  const double energy = (analysis != nullptr && analysis->energy)
                            ? *analysis->energy
                            : (0.25 + static_cast<double>(seed % 50) / 100);
  const double valence = (analysis != nullptr && analysis->valence)
                             ? *analysis->valence
                             : (0.25 + static_cast<double>((seed >> 8) % 50) / 100);

  SignalQuality quality;
  if (identity == nullptr) {
    quality = SignalQuality::Idle;
  } else if (analysis != nullptr && !analysis->lyrics.empty()) {
    quality = SignalQuality::Timeline;
  } else if (analysis != nullptr && analysis->bpm) {
    quality = SignalQuality::Bpm;
  } else {
    quality = SignalQuality::Metadata;
  }

  frame.time = position;
  frame.delta = delta;
  frame.duration = identity != nullptr ? identity->duration : 0;
  frame.progress = identity != nullptr
                       ? std::min(1.0, std::max(0.0, position / std::max(0.01, identity->duration)))
                       : 0;
  frame.playing = playing;
  frame.bpm = bpm;
  frame.beatPhase = beatPhase;
  frame.beatPulse = beatPulse;
  frame.beatIndex = beatIndex;
  frame.energy = energy;
  frame.valence = valence;
  frame.lyricIndex = lyricIndex;
  frame.lyricCurrent = lyricIndex >= 0 ? lyrics[static_cast<size_t>(lyricIndex)].text : "";
  frame.lyricNext = nextIndex < lyrics.size() ? lyrics[nextIndex].text : "";
  frame.spectrum = syntheticSpectrum(position, beatPhase, beatPulse, energy, seed);
  frame.quality = quality;
  frame.trackSeed = seed;
  return frame;
}

}  // namespace nova
