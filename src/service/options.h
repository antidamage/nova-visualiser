#pragma once

#include <string>

namespace nova::service {

struct Options {
  // The dashboard's own listener, NOT its browser ingress. Caddy owns port 80
  // and routes by Host header, so a request to the bare 127.0.0.1 origin is
  // answered 200 with an empty body -- which reads here as a config that did not
  // parse, leaving the renderer with no module and the screen black. 3001 is
  // what Caddy itself proxies to; see ops/nova-visualiser.defaults.
  std::string dashboardUrl = "http://127.0.0.1:3001";
  int streamPort = 8770;
  int controlPort = 8771;
  // SRT rung for the Apple TV. Offered alongside the TCP rung on 8770, which
  // stays as the fallback; a client picks SRT and falls back on failure. 0
  // disables it.
  int srtPort = 8772;
  // ARQ latency budget in milliseconds. SRT retransmits inside this window and
  // gives up outside it, which is what bounds head-of-line blocking. On a LAN
  // this leaves room for two or three round trips.
  int srtLatencyMs = 60;

  int width = 3840;
  int height = 2160;
  int frameRate = 60;
  int64_t bitRate = 30'000'000;
  bool tenBit = true;

  // Browser rung. Disabled when the URL is empty.
  std::string publishUrl = "rtsp://127.0.0.1:8554/visualiser";
  // MediaMTX control API, used only to ask whether any browser is actually
  // reading the path. Empty disables the check (readers then count as zero).
  std::string mediamtxApiUrl = "http://127.0.0.1:9997";
  int publishWidth = 1920;
  int publishHeight = 1080;
  int64_t publishBitRate = 8'000'000;
  // Ceiling on a debug full-rate lease. The browser rung normally encodes every
  // other frame because it shares one NVENC engine with the 4K rung, so running
  // it at full rate is borrowed capacity that must always expire on its own.
  int fullRateMaxSeconds = 300;

  double configPollSeconds = 15;
  float exposure = 1.0f;

  // Release GPU memory when nothing is watching. This is this service's own idle
  // policy -- nothing outside it asks for the card back -- but the GPU is shared
  // with the voice stack, which has only a few gigabytes of headroom, so idling
  // with a 4K pipeline resident would be rude at best.
  double idleReleaseSeconds = 30;
  // Encode cadence while nothing is playing. The scene is nearly static then,
  // so a full-rate CBR encode spends the whole bitrate padding a still image.
  // Set equal to frameRate to disable.
  int idleFrameRate = 15;

  bool housePartyEnabled = false;
  bool verbose = false;

  // Offline modes.
  std::string dumpFramePath;   // render one frame to PNG-less raw RGBA and exit
  int selfTestFrames = 0;      // render N frames with no client and report timing

  static bool parse(int argc, char** argv, Options& out, std::string& error);
  static std::string usage();
};

}  // namespace nova::service
