// Service orchestration and the thread model.
//
//   config   -- HTTP + SSE. Publishes immutable config snapshots.
//   sim      -- fixed 1/120 s accumulator. Publishes triple-buffered scene
//               snapshots. Never blocks on the renderer.
//   render   -- sole GL context; also runs the CUDA interop and NVENC, because
//               cuGraphicsMapResources needs the GL context current on the
//               calling thread. Paced by a monotonic 60 Hz deadline, not vsync:
//               there is no display attached.
//   writers  -- one per stream client, inside StreamServer. A client that
//               cannot keep up is dropped to the next IDR and never applies
//               back-pressure to the GPU.
//   lighting -- house-party frames to the dashboard.
//   control  -- status and commands.
//   watcher  -- viewer demand and the debug full-rate lease. Separate because it
//               makes blocking HTTP calls, which must never happen on the render
//               thread: a 2 s MediaMTX poll inside the pacing loop showed up as a
//               periodic hitch on the Apple TV.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/simulation.h"
#include "core/snapshot.h"
#include "encode/cuda_interop.h"
#include "encode/encoder.h"
#include "encode/publisher.h"
#include "gfx/egl_device.h"
#include "gfx/renderer.h"
#include "net/config_client.h"
#include "net/control_server.h"
#include "net/house_party.h"
#include "net/stream_server.h"
#if NOVA_VISUALISER_HAVE_SRT
#include "net/srt_server.h"
#endif
#include "service/options.h"

namespace nova::service {

class Engine {
 public:
  explicit Engine(Options options);
  ~Engine();

  bool run(std::string& error);
  void requestStop();

  // Offline paths used by the deploy check and the conformance frame dumps.
  bool selfTest(int frames, std::string& error);
  bool dumpFrame(const std::string& path, std::string& error);

 private:
  bool startGpu(std::string& error);
  void stopGpu();
  void simulationLoop();
  void renderLoop();
  void watcherLoop();
  void publishSimulationInput();
  void applyControlLanes(const net::ConfigSnapshot& snapshot, const SignalFrame& frame,
                         std::unordered_map<std::string, double>& settings,
                         std::unordered_set<std::string>& driven);
  double renderAheadSeconds() const;
  void requestKeyframe();
  int publishDivisor() const;
  std::string statusJson() const;
  net::ControlResponse handleControl(const net::ControlRequest& request);

  Options options_;
  std::atomic<bool> running_{false};

  Simulation simulation_;
  SnapshotChannel snapshots_;

  gfx::EglDevice device_;
  gfx::Renderer renderer_;
  encode::CudaInterop interop_;
  std::unique_ptr<encode::Encoder> primaryEncoder_;
  std::unique_ptr<encode::Encoder> publishEncoder_;
  encode::RtspPublisher publisher_;
  int primaryOutput_ = -1;
  int publishOutput_ = -1;
  std::atomic<bool> gpuReady_{false};
  // Resolved backdrop speed, published for /status. Written on the render
  // thread, read on the control thread.
  std::atomic<float> fluidSpeedForStatus_{0.0f};
  // True while nothing is playing. Drives the reduced idle encode cadence.
  std::atomic<double> fluidPhaseForStatus_{0.0};
  // The centre slot, published for /status. Whether an image actually loaded is
  // otherwise unfalsifiable from outside: a missing one and a decode that
  // failed both look like an empty centre, and the only other way to tell them
  // apart is to eyeball the picture. Same rationale as `fluidPhase`.
  std::atomic<int> centreImageWidthForStatus_{0};
  std::atomic<int> centreImageHeightForStatus_{0};
  std::atomic<float> centreImageFadeForStatus_{1.0f};
  std::atomic<bool> centreMessageForStatus_{false};
  std::atomic<bool> idlePlayback_{false};
  // Set when an IDR has been asked for, so the idle cadence cannot skip it.
  std::atomic<bool> keyframeWanted_{false};

  net::ConfigClient config_;
  net::StreamServer stream_;
#if NOVA_VISUALISER_HAVE_SRT
  // Offered alongside the TCP rung, never instead of it: an older client keeps
  // working untouched and a new one falls back if SRT cannot connect.
  net::SrtServer srt_;
#endif
  net::ControlServer control_;
  net::HousePartyProducer houseParty_;

  std::thread simulationThread_;
  std::thread renderThread_;
  std::thread watcherThread_;

  // Shared status, published for the control endpoint.
  std::atomic<double> measuredFps_{0};
  std::atomic<double> renderMs_{0};
  std::atomic<double> encodeMs_{0};
  std::atomic<double> simulationMs_{0};
  std::atomic<int64_t> encodedFrames_{0};
  std::atomic<int> particleCount_{0};

  // Instrumentation. `fps` alone hides a hitch: a loop that runs 59 frames on
  // time and one 200 ms late still reports ~60. The interval tail is what
  // actually correlates with what the room sees.
  std::atomic<double> frameIntervalP99Ms_{0};
  std::atomic<double> frameIntervalMaxMs_{0};
  std::atomic<double> gpuMs_{0};
  // Access-unit sizes, to show whether periodic IDRs dominate the bitrate.
  std::atomic<double> auBytesMean_{0};
  std::atomic<int64_t> auBytesMax_{0};
  std::atomic<double> keyframeBytesMean_{0};

  // Decoder-side telemetry reported by the Apple TV once per second. Socket
  // stats cannot reveal an application that acknowledges packets but drains
  // access units too slowly, which previously hid a ~39 fps client cap.
  std::atomic<double> clientDecodedFps_{0};
  std::atomic<int64_t> clientDecodedFrames_{0};
  std::atomic<int64_t> clientDroppedFrames_{0};

  // Viewer demand, resolved on the watcher thread so the render loop never
  // blocks on HTTP.
  std::atomic<bool> browserWatching_{false};

  // Debug full-rate lease. The browser rung normally encodes every other frame
  // because Turing has a single NVENC engine; a debug session can take it to
  // full rate for a bounded time. It expires on a deadline AND on pressure,
  // because the 4K rung to the Apple TV always outranks a diagnostic view.
  std::atomic<int> publishFrameDivisor_{6};
  std::atomic<double> fullRateUntil_{0};
  std::atomic<int> fullRateExitReason_{0};  // 0 none, 1 expired, 2 fps, 3 encode
  std::atomic<double> fullRatePressureSince_{0};

  // Local palette cursor resolving Nova's authoritative selected entry. The
  // rotation walks entries, not themes: one theme can appear several times with
  // different settings groups, so a theme id cannot address a position in it.
  size_t entryIndex_ = 0;
  std::string currentEntryId_;
  std::string currentThemeId_;

  // Per driver-slot envelope state, owned here and threaded through the shared
  // evaluator in core/parameter_drivers.h.
  DriverStates parameterDriverStates_;

  // Resolved signal frames arrive directly from the original Apple TV engine.
  // A short stale timeout keeps the renderer self-contained if that uplink
  // disappears; it falls back to the dashboard's now-playing/analysis path.
  mutable std::mutex signalMutex_;
  SignalFrame upstreamSignal_;
  double upstreamSignalReceivedAt_ = 0;
  bool hasUpstreamSignal_ = false;

  // Keyframes are requested from several places (a joining client, a transport
  // that dropped a client to the next IDR, an operator). Rate-limited centrally
  // so a flapping client cannot pin the encoder into emitting nothing but IDRs.
  std::atomic<double> lastKeyframeRequest_{0};

  mutable std::mutex statusMutex_;
  std::string statusError_;
  std::string clientConnectionState_;
  std::string clientDisconnectReason_;
  int64_t clientReconnectCount_ = 0;
};

}  // namespace nova::service
