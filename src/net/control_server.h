// Small HTTP control/diagnostics surface.
//
// The Apple TV sends its UX commands to Nova, which owns the configuration --
// that is what keeps the dashboard the single source of truth. This endpoint
// exists for the two things Nova cannot answer: renderer status (for the
// diagnostics page and the deploy check) and commands that only affect the
// running render, such as forcing a keyframe or nudging house-party on.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace nova::net {

struct ControlRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct ControlResponse {
  int status = 200;
  std::string contentType = "application/json";
  std::string body = "{}";
};

class ControlServer {
 public:
  using Handler = std::function<ControlResponse(const ControlRequest&)>;

  ControlServer() = default;
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  bool start(int port, Handler handler, std::string& error);
  void stop();

 private:
  void run();

  int listenSocket_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
  Handler handler_;
};

}  // namespace nova::net
