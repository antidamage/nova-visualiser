#include "service/options.h"

#include <cstdlib>
#include <string>

namespace nova::service {

std::string Options::usage() {
  return
      "nova-visualiser -- GPU Phonoscope renderer and stream server\n"
      "\n"
      "  --dashboard <url>        dashboard listener (default http://127.0.0.1:3001)\n"
      "  --stream-port <n>        Apple TV stream port (default 8770)\n"
      "  --control-port <n>       control/diagnostics port (default 8771)\n"
      "  --srt-port <n>           Apple TV SRT rung port, 0 disables (default 8772)\n"
      "  --srt-latency <ms>       SRT ARQ latency budget (default 60)\n"
      "  --size <w>x<h>           render size (default 3840x2160)\n"
      "  --fps <n>                frame rate (default 60)\n"
      "  --bitrate <bps>          Apple TV rung bitrate (default 30000000)\n"
      "  --eight-bit              encode HEVC Main instead of Main10\n"
      "  --publish <rtsp url>     browser rung target, empty to disable\n"
      "  --publish-size <w>x<h>   browser rung size (default 1920x1080)\n"
      "  --publish-bitrate <bps>  browser rung bitrate (default 8000000)\n"
      "  --full-rate-max <s>      ceiling on a debug full-rate lease (default 300)\n"
      "  --mediamtx-api <url>     MediaMTX API, used for the browser reader count\n"
      "  --exposure <f>           tonemap exposure (default 1.0)\n"
      "  --idle-release <s>       free GPU memory after this long with no client\n"
      "  --house-party            drive house-party lighting from this service\n"
      "  --self-test <frames>     render N frames headlessly and report timing\n"
      "  --dump-frame <path>      write one composited RGBA frame and exit\n"
      "  --verbose\n";
}

namespace {

bool parseSize(const std::string& text, int& width, int& height) {
  const size_t separator = text.find('x');
  if (separator == std::string::npos) return false;
  width = std::atoi(text.substr(0, separator).c_str());
  height = std::atoi(text.substr(separator + 1).c_str());
  return width > 0 && height > 0;
}

}  // namespace

bool Options::parse(int argc, char** argv, Options& out, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];
    auto next = [&](std::string& value) {
      if (index + 1 >= argc) return false;
      value = argv[++index];
      return true;
    };

    std::string value;
    if (flag == "--dashboard" && next(value)) {
      out.dashboardUrl = value;
    } else if (flag == "--stream-port" && next(value)) {
      out.streamPort = std::atoi(value.c_str());
    } else if (flag == "--control-port" && next(value)) {
      out.controlPort = std::atoi(value.c_str());
    } else if (flag == "--srt-port" && next(value)) {
      out.srtPort = std::atoi(value.c_str());
    } else if (flag == "--srt-latency" && next(value)) {
      out.srtLatencyMs = std::atoi(value.c_str());
    } else if (flag == "--size" && next(value)) {
      if (!parseSize(value, out.width, out.height)) {
        error = "--size expects <width>x<height>";
        return false;
      }
    } else if (flag == "--fps" && next(value)) {
      out.frameRate = std::atoi(value.c_str());
    } else if (flag == "--bitrate" && next(value)) {
      out.bitRate = std::atoll(value.c_str());
    } else if (flag == "--eight-bit") {
      out.tenBit = false;
    } else if (flag == "--publish" && next(value)) {
      out.publishUrl = value;
    } else if (flag == "--publish-size" && next(value)) {
      if (!parseSize(value, out.publishWidth, out.publishHeight)) {
        error = "--publish-size expects <width>x<height>";
        return false;
      }
    } else if (flag == "--publish-bitrate" && next(value)) {
      out.publishBitRate = std::atoll(value.c_str());
    } else if (flag == "--full-rate-max" && next(value)) {
      out.fullRateMaxSeconds = std::atoi(value.c_str());
    } else if (flag == "--mediamtx-api" && next(value)) {
      out.mediamtxApiUrl = value;
    } else if (flag == "--exposure" && next(value)) {
      out.exposure = static_cast<float>(std::atof(value.c_str()));
    } else if (flag == "--idle-release" && next(value)) {
      out.idleReleaseSeconds = std::atof(value.c_str());
    } else if (flag == "--idle-fps" && next(value)) {
      out.idleFrameRate = std::atoi(value.c_str());
    } else if (flag == "--house-party") {
      out.housePartyEnabled = true;
    } else if (flag == "--self-test" && next(value)) {
      out.selfTestFrames = std::atoi(value.c_str());
    } else if (flag == "--dump-frame" && next(value)) {
      out.dumpFramePath = value;
    } else if (flag == "--verbose") {
      out.verbose = true;
    } else if (flag == "--help" || flag == "-h") {
      error = usage();
      return false;
    } else {
      error = "unknown option: " + flag + "\n\n" + usage();
      return false;
    }
  }

  if (out.width % 2 != 0 || out.height % 2 != 0) {
    error = "render size must be even in both axes (4:2:0 chroma)";
    return false;
  }
  return true;
}

}  // namespace nova::service
