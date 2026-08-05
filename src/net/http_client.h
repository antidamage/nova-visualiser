// Minimal blocking HTTP/1.1 client.
//
// The renderer and the dashboard are the same box now, so every request is a
// loopback hop with no TLS and no proxy in the way. That makes a tiny purpose
// built client preferable to dragging in curl: fewer moving parts on the host
// that runs the whole house.
#pragma once

#include <map>
#include <string>

namespace nova::net {

struct HttpResponse {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string error;

  bool ok() const { return status >= 200 && status < 300; }
};

struct Url {
  std::string host = "127.0.0.1";
  int port = 80;
  std::string path = "/";

  static bool parse(const std::string& text, Url& out);
};

HttpResponse httpGet(const std::string& url, const std::map<std::string, std::string>& headers = {},
                     int timeoutSeconds = 10);

HttpResponse httpPost(const std::string& url, const std::string& body,
                      const std::string& contentType = "application/json",
                      const std::map<std::string, std::string>& headers = {},
                      int timeoutSeconds = 10);

// Opens a socket and returns the descriptor, or -1. Shared with the SSE client,
// which needs to keep reading rather than consuming a whole response.
int connectTo(const std::string& host, int port, int timeoutSeconds, std::string& error);

}  // namespace nova::net
