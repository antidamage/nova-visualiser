// nova-visualiser -- headless GPU Phonoscope renderer for iridium.
//
// Renders the same module format the Apple TV's Metal engine renders, at 4K60,
// and streams it as hardware-encoded HEVC. The Apple TV keeps its local engine
// as a permanent fallback, so the two implementations are held in step by the
// conformance corpus in tests/conformance.
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <chrono>
#include <memory>
#include <thread>

#include "service/engine.h"
#include "service/options.h"

namespace {

std::unique_ptr<nova::service::Engine> gEngine;

void handleSignal(int) {
  if (gEngine) gEngine->requestStop();
}

}  // namespace

int main(int argc, char** argv) {
  // Line buffering so journald and a redirected log see progress as it happens
  // rather than only when the process exits.
  setvbuf(stdout, nullptr, _IOLBF, 0);

  nova::service::Options options;
  std::string error;
  if (!nova::service::Options::parse(argc, argv, options, error)) {
    std::fputs(error.c_str(), stderr);
    std::fputc('\n', stderr);
    return 2;
  }

  gEngine = std::make_unique<nova::service::Engine>(options);

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
  // A client vanishing mid-write must not take the process down.
  std::signal(SIGPIPE, SIG_IGN);

  // The one-shot modes _exit() rather than return.
  //
  // Returning from main destroys gEngine, and ~Engine tears down CUDA, which
  // can deadlock inside the driver -- the main thread ends up joining a wedged
  // driver thread and never comes back. The long-running service has a shutdown
  // watchdog for exactly this, but a one-shot has nothing to protect it: the
  // work is finished, the file is written, and the process simply never exits.
  // A `--dump-frame` invocation was found still alive 44 minutes later, holding
  // an NVENC session and 600 MB of VRAM on a card the voice stack shares.
  //
  // There is nothing to clean up that the kernel will not reclaim, so skip the
  // static destructors entirely. Flush first, because _exit does not.
  if (!options.dumpFramePath.empty()) {
    const bool ok = gEngine->dumpFrame(options.dumpFramePath, error);
    if (!ok) std::fprintf(stderr, "nova-visualiser: %s\n", error.c_str());
    std::fflush(nullptr);
    _exit(ok ? 0 : 1);
  }
  if (options.selfTestFrames > 0) {
    const bool ok = gEngine->selfTest(options.selfTestFrames, error);
    if (!ok) std::fprintf(stderr, "nova-visualiser: %s\n", error.c_str());
    std::fflush(nullptr);
    _exit(ok ? 0 : 1);
  }

  std::printf("nova-visualiser: %dx%d@%d %s, stream :%d, control :%d\n", options.width,
              options.height, options.frameRate, options.tenBit ? "HEVC Main10" : "HEVC Main",
              options.streamPort, options.controlPort);
  std::fflush(stdout);

  const bool ok = gEngine->run(error);
  // Skip static destructors on every way out, as the one-shot modes do:
  // libsrt's own exit-time finaliser segfaults in __cxa_finalize (measured on
  // Iridium, libsrt-gnutls 1.5). A REQUESTED stop -- SIGTERM or the idle exit --
  // is a success whatever non-fatal error was recorded during the run (a
  // publish hiccup, an SRT rung that failed): exiting 1 there would make
  // Restart=on-failure undo every idle exit (specs/idle-exit.md). Only a run
  // that failed on its own (a port that would not bind) exits 1.
  if (!ok && !gEngine->stopRequested()) {
    std::fprintf(stderr, "nova-visualiser: %s\n", error.c_str());
    std::fflush(nullptr);
    _exit(1);
  }
  std::fflush(nullptr);
  _exit(0);
}
