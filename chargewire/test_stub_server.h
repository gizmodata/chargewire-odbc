// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Tests only: a Chargewire stand-in on 127.0.0.1 that answers each request with whatever the
// test's handler says, and counts the requests it got.
#pragma once

#include <odbcabstraction/spi/connection.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace driver::chargewire_odbc::test_stub {

struct Answer {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::map<std::string, std::string> headers;
};

inline Answer Resuming(const std::string& retry_after) {
  return {503, "application/problem+json",
          R"({"type":"urn:chargewire:error:server_resuming","title":"Server resuming","status":503,)"
          R"("code":"server_resuming","detail":"the server is resuming; retry after )" + retry_after + R"( s"})",
          {{"Retry-After", retry_after}}};
}

// `handler` gets the method, the path and how many requests with that method and path came
// before; every connection has a thread.
class StubServer {
 public:
  using Handler = std::function<Answer(const std::string& method, const std::string& path, int nth)>;

  explicit StubServer(Handler handler)
      : handler_(std::move(handler)), acceptor_(io_, {boost::asio::ip::make_address("127.0.0.1"), 0}) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] { Run(); });
  }
  ~StubServer() {
    stopping_ = true;
    {
      std::lock_guard lock(mutex_);
      for (auto* socket : open_) {
        boost::system::error_code ignored;
        socket->shutdown(tcp::socket::shutdown_both, ignored);
      }
    }
    try {
      boost::asio::io_context io;
      tcp::socket poke(io);
      poke.connect({boost::asio::ip::make_address("127.0.0.1"), port_});
    } catch (...) {
    }
    thread_.join();
    for (auto& connection : connections_) connection.join();
  }

  odbcabstraction::Connection::ConnPropertyMap Properties(const std::map<std::string, std::string>& extra = {}) const {
    odbcabstraction::Connection::ConnPropertyMap properties = {
        {"host", "127.0.0.1"}, {"port", std::to_string(port_)}, {"useEncryption", "false"}, {"uid", "joe"}, {"pwd", "pw"}};
    for (const auto& [key, value] : extra) properties[key] = value;
    return properties;
  }
  int Count(const std::string& method, const std::string& path) {
    std::lock_guard lock(mutex_);
    int n = 0;
    for (const auto& [m, p] : seen_) n += m == method && p == path;
    return n;
  }
  // In a handler: the Authorization header of the request it is answering.
  static std::string& CurrentAuthorization() {
    thread_local std::string authorization;
    return authorization;
  }
  // The body of the last request with this method and path ("" when there was none).
  std::string Body(const std::string& method, const std::string& path) {
    std::lock_guard lock(mutex_);
    std::string body;
    for (size_t i = 0; i < seen_.size(); i++) {
      if (seen_[i].first == method && seen_[i].second == path) body = bodies_[i];
    }
    return body;
  }
  // Every request so far, as "METHOD /path", in the order they came.
  std::vector<std::string> Seen() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> seen;
    for (const auto& [m, p] : seen_) seen.push_back(m + " " + p);
    return seen;
  }

 private:
  using tcp = boost::asio::ip::tcp;

  void Run() {
    while (!stopping_) {
      auto socket = std::make_shared<tcp::socket>(io_);
      boost::system::error_code ec;
      acceptor_.accept(*socket, ec);
      if (ec || stopping_) break;
      std::lock_guard lock(mutex_);
      open_.push_back(socket.get());
      connections_.emplace_back([this, socket] {
        Serve(*socket);
        std::lock_guard lock(mutex_);
        std::erase(open_, socket.get());
      });
    }
  }
  void Serve(tcp::socket& socket) {
    namespace http = boost::beast::http;
    boost::beast::flat_buffer buffer;
    while (!stopping_) {
      http::request<http::string_body> request;
      boost::system::error_code ec;
      http::read(socket, buffer, request, ec);
      if (ec) return;
      const std::string method(request.method_string());
      std::string path(request.target());
      path = path.substr(0, path.find('?'));
      int nth = 0;
      {
        std::lock_guard lock(mutex_);
        for (const auto& [m, p] : seen_) nth += m == method && p == path;
        seen_.emplace_back(method, path);
        bodies_.push_back(request.body());
      }
      CurrentAuthorization() = std::string(request[http::field::authorization]);
      const Answer answer = handler_(method, path, nth);
      http::response<http::string_body> response(static_cast<http::status>(answer.status), 11);
      response.set(http::field::content_type, answer.content_type);
      for (const auto& [name, value] : answer.headers) response.set(name, value);
      response.body() = answer.body;
      response.keep_alive(request.keep_alive());
      response.prepare_payload();
      http::write(socket, response, ec);
      if (ec || !request.keep_alive()) return;
    }
  }

  Handler handler_;
  boost::asio::io_context io_;
  tcp::acceptor acceptor_;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_ = false;
  std::mutex mutex_;
  std::vector<tcp::socket*> open_;
  std::vector<std::pair<std::string, std::string>> seen_;
  std::vector<std::string> bodies_;  // each request's body, in the same order
  std::thread thread_;
  std::vector<std::thread> connections_;
};

inline const char* kToken = "/chargewire/v1/auth/token";
inline const char* kSessions = "/chargewire/v1/sessions";

// A server that serves: discovery, a sign-in whose token lasts `expires_in` seconds, session s1,
// and 204 for every DELETE.
inline Answer Serving(const std::string& method, const std::string& path, int expires_in = 3600) {
  if (method == "GET" && path == "/chargewire") {
    return {200, "application/json",
            R"({"protocol":"chargewire","protocol_versions":["1"],"server":{"name":"stub","version":"0"},)"
            R"("capabilities":[],"auth":{"grant_types":["password"]}})",
            {}};
  }
  if (method == "POST" && path == kToken) {
    return {200, "application/json", R"({"access_token":"t","expires_in":)" + std::to_string(expires_in) + "}", {}};
  }
  if (method == "POST" && path == kSessions) {
    return {201, "application/json", R"({"session_id":"s1","idle_timeout_s":900,"options":{},"transaction":"none"})", {}};
  }
  if (method == "DELETE") return {204, "application/json", "", {}};
  return {404, "application/problem+json", R"({"status":404,"code":"not_found","detail":"no"})", {}};
}

}  // namespace driver::chargewire_odbc::test_stub
