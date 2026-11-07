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

#include "gtest/gtest.h"
#include "test_stub_server.h"

namespace driver {
namespace chargewire_odbc {
using odbcabstraction::Connection;
using namespace std::chrono_literals;
using namespace test_stub;

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
