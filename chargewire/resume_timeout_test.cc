// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Connecting to a server that is resuming (Chargewire spec §11.2): a stub server here in the
// test process answers 503 server_resuming as a stand-in for a stopped server would, and the
// tests check the driver's ResumeTimeout and that SQL_ATTR_LOGIN_TIMEOUT doesn't cut a held
// sign-in.

#include "chargewire_connection.h"

#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "gtest/gtest.h"

namespace driver {
namespace chargewire_odbc {
namespace {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using odbcabstraction::Connection;
using namespace std::chrono_literals;

struct Answer {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::map<std::string, std::string> headers;
};

Answer Resuming(const std::string& retry_after) {
  return {503, "application/problem+json",
          R"({"type":"urn:chargewire:error:server_resuming","title":"Server resuming","status":503,)"
          R"("code":"server_resuming","detail":"the server is resuming; retry after )" + retry_after + R"( s"})",
          {{"Retry-After", retry_after}}};
}

// A Chargewire stand-in on 127.0.0.1. `handler` gets the method, the path and how many
// requests with that method and path came before; every connection has a thread.
class StubServer {
 public:
  using Handler = std::function<Answer(const std::string& method, const std::string& path, int nth)>;

  explicit StubServer(Handler handler) : handler_(std::move(handler)), acceptor_(io_, {net::ip::make_address("127.0.0.1"), 0}) {
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
      net::io_context io;
      tcp::socket poke(io);
      poke.connect({net::ip::make_address("127.0.0.1"), port_});
    } catch (...) {
    }
    thread_.join();
    for (auto& connection : connections_) connection.join();
  }

  Connection::ConnPropertyMap Properties(const std::map<std::string, std::string>& extra = {}) const {
    Connection::ConnPropertyMap properties = {{"host", "127.0.0.1"}, {"port", std::to_string(port_)},
                                              {"useEncryption", "false"}, {"uid", "joe"}, {"pwd", "pw"}};
    for (const auto& [key, value] : extra) properties[key] = value;
    return properties;
  }
  int Count(const std::string& method, const std::string& path) {
    std::lock_guard lock(mutex_);
    int n = 0;
    for (const auto& [m, p] : seen_) n += m == method && p == path;
    return n;
  }

 private:
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
    beast::flat_buffer buffer;
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
      }
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
  net::io_context io_;
  tcp::acceptor acceptor_;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_ = false;
  std::mutex mutex_;
  std::vector<tcp::socket*> open_;
  std::vector<std::pair<std::string, std::string>> seen_;
  std::thread thread_;
  std::vector<std::thread> connections_;
};

const char* kToken = "/chargewire/v1/auth/token";
const char* kSessions = "/chargewire/v1/sessions";

Answer Serving(const std::string& method, const std::string& path) {
  if (method == "GET" && path == "/chargewire") {
    return {200, "application/json",
            R"({"protocol":"chargewire","protocol_versions":["1"],"server":{"name":"stub","version":"0"},)"
            R"("capabilities":[],"auth":{"grant_types":["password"]}})",
            {}};
  }
  if (method == "POST" && path == kToken) return {200, "application/json", R"({"access_token":"t","expires_in":3600})", {}};
  if (method == "POST" && path == kSessions) {
    return {201, "application/json", R"({"session_id":"s1","idle_timeout_s":900,"options":{},"transaction":"none"})", {}};
  }
  if (method == "DELETE") return {204, "application/json", "", {}};
  return {404, "application/problem+json", R"({"status":404,"code":"not_found","detail":"no"})", {}};
}

}  // namespace

TEST(ResumeTimeoutTests, DefaultsToTenMinutesAndZeroTurnsTheRetryOff) {
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({}).resume_timeout, 600s);
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"ResumeTimeout", "0"}}).resume_timeout, 0s);
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"resumetimeout", "45"}}).resume_timeout, 45s);
  for (const auto* bad : {"-1", "x", "1.5", "", " 5", "86401", "99999999999999999999"}) {
    try {
      ChargewireConnection::BuildConnectOptions({{"ResumeTimeout", bad}});
      ADD_FAILURE() << "accepted ResumeTimeout=" << bad;
    } catch (const odbcabstraction::DriverException& e) {
      EXPECT_EQ(e.GetSqlState(), "HY024") << bad;
      EXPECT_NE(e.GetMessageText().find("ResumeTimeout must be a whole number of seconds"), std::string::npos)
          << e.GetMessageText();
    }
  }
}

TEST(ResumeTimeoutTests, ConnectWaitsWhileTheServerResumes) {
  StubServer stub([](const std::string& method, const std::string& path, int nth) {
    if (method == "POST" && path == kToken && nth < 3) return Resuming("0");
    if (method == "POST" && path == kSessions && nth < 2) return Resuming("1");
    return Serving(method, path);
  });
  ChargewireConnection connection(odbcabstraction::V_3);
  std::vector<std::string> missing;
  connection.Connect(stub.Properties(), missing);
  EXPECT_EQ(stub.Count("POST", kToken), 4);
  EXPECT_EQ(stub.Count("POST", kSessions), 3);
  connection.Close();
}

TEST(ResumeTimeoutTests, ResumeTimeoutEndsTheWaitWithTheServersMessage) {
  StubServer stub([](const std::string& method, const std::string& path, int nth) {
    if (method == "POST" && path == kToken && nth < 3) return Resuming("1");  // a longer wait would connect
    return Serving(method, path);
  });
  ChargewireConnection connection(odbcabstraction::V_3);
  std::vector<std::string> missing;
  const auto started = std::chrono::steady_clock::now();
  try {
    connection.Connect(stub.Properties({{"ResumeTimeout", "2"}}), missing);
    FAIL() << "connected";
  } catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetSqlState(), "HYT01");
    EXPECT_NE(e.GetMessageText().find("the server is resuming; retry after 1 s"), std::string::npos)
        << e.GetMessageText();
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1900ms);
  EXPECT_EQ(stub.Count("POST", kToken), 2);
}

TEST(ResumeTimeoutTests, ZeroConnectsOnceAndReportsTheServerResuming) {
  StubServer stub([](const std::string& method, const std::string& path, int nth) {
    if (method == "POST" && path == kToken && nth < 3) return Resuming("0");  // a retry would connect
    return Serving(method, path);
  });
  ChargewireConnection connection(odbcabstraction::V_3);
  std::vector<std::string> missing;
  try {
    connection.Connect(stub.Properties({{"ResumeTimeout", "0"}}), missing);
    FAIL() << "connected";
  } catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetSqlState(), "HYT01");
    EXPECT_EQ(e.GetMessageText().find("the server is resuming"), 0u) << e.GetMessageText();
  }
  EXPECT_EQ(stub.Count("POST", kToken), 1);
}

// A stand-in holds a sign-in up to 25 s while the server comes up. SQL_ATTR_LOGIN_TIMEOUT
// bounds connecting (TCP and TLS), so a 5 s login timeout still gets the answer. Real time.
TEST(ResumeTimeoutTests, ASignInHeldTwentyFiveSecondsOutlastsTheLoginTimeout) {
  StubServer stub([](const std::string& method, const std::string& path, int) {
    if (method == "POST" && path == kToken) std::this_thread::sleep_for(25s);
    return Serving(method, path);
  });
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetAttribute(Connection::LOGIN_TIMEOUT, static_cast<uint32_t>(5));
  std::vector<std::string> missing;
  const auto started = std::chrono::steady_clock::now();
  connection.Connect(stub.Properties(), missing);
  EXPECT_GE(std::chrono::steady_clock::now() - started, 25s);
  connection.Close();
}

}  // namespace chargewire_odbc
}  // namespace driver
