/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "chargewire_connection.h"

#include <odbcabstraction/platform.h>

#include "gtest/gtest.h"
#include "chargewire_client.h"
#include "driver_profile.h"
#include <odbcabstraction/exceptions.h>

namespace driver {
namespace chargewire_odbc {

using odbcabstraction::Connection;

TEST(AttributeTests, SetAndGetAttribute) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  connection.SetAttribute(Connection::CONNECTION_TIMEOUT, static_cast<uint32_t>(200));
  const boost::optional<Connection::Attribute> firstValue =
      connection.GetAttribute(Connection::CONNECTION_TIMEOUT);

  EXPECT_TRUE(firstValue);

  EXPECT_EQ(boost::get<uint32_t>(*firstValue), static_cast<uint32_t>(200));

  connection.SetAttribute(Connection::CONNECTION_TIMEOUT, static_cast<uint32_t>(300));

  const boost::optional<Connection::Attribute> changeValue =
      connection.GetAttribute(Connection::CONNECTION_TIMEOUT);

  EXPECT_TRUE(changeValue);
  EXPECT_EQ(boost::get<uint32_t>(*changeValue), static_cast<uint32_t>(300));

  connection.Close();
}

TEST(AttributeTests, GetAttributeWithoutSetting) {
  ChargewireConnection connection(odbcabstraction::V_3);

  const boost::optional<Connection::Attribute> optional =
      connection.GetAttribute(Connection::CONNECTION_TIMEOUT);
  connection.SetClosed(false);

  EXPECT_EQ(0, boost::get<uint32_t>(*optional));

  connection.Close();
}

TEST(MetadataSettingsTest, StringColumnLengthTest) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  const int32_t expected_string_column_length = 100000;

  const Connection::ConnPropertyMap properties = {
      {ChargewireConnection::HOST, std::string("localhost")},  // expect not used
      {ChargewireConnection::PORT, std::string("31337")},  // expect not used
      {ChargewireConnection::USE_ENCRYPTION, std::string("false")},  // expect not used
      {ChargewireConnection::STRING_COLUMN_LENGTH, std::to_string(expected_string_column_length)},
  };

  const boost::optional<int32_t> actual_string_column_length = connection.GetStringColumnLength(properties);

  EXPECT_TRUE(actual_string_column_length);
  EXPECT_EQ(expected_string_column_length, *actual_string_column_length);

  connection.Close();
}

TEST(MetadataSettingsTest, UseWideCharTest) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  const Connection::ConnPropertyMap properties1 = {
          {ChargewireConnection::USE_WIDE_CHAR, std::string("true")},
  };
  const Connection::ConnPropertyMap properties2 = {
          {ChargewireConnection::USE_WIDE_CHAR, std::string("false")},
  };

  EXPECT_EQ(true, connection.GetUseWideChar(properties1));
  EXPECT_EQ(false, connection.GetUseWideChar(properties2));

  connection.Close();
}

TEST(BuildConnectOptionsTests, ForTcp) {
  auto o = ChargewireConnection::BuildConnectOptions({{"host", "localhost"}, {"port", "31338"}, {"useEncryption", "false"}});
  EXPECT_EQ(o.host, "localhost"); EXPECT_EQ(o.port, 31338); EXPECT_FALSE(o.tls.enabled);
  EXPECT_EQ(o.base_path, "/chargewire");
}
TEST(BuildConnectOptionsTests, ForTls) {
  auto o = ChargewireConnection::BuildConnectOptions({{"host", "example.com"}, {"trustedCerts", "/ca.pem"}});
  EXPECT_TRUE(o.tls.enabled); EXPECT_FALSE(o.tls.skip_verify);
  EXPECT_EQ(o.port, GetDriverProfile().default_port); EXPECT_EQ(o.tls.root_certs_file, "/ca.pem");
}
TEST(BuildConnectOptionsTests, InvalidPort) {
  for (const auto& p : {"0", "65536", "-1", "x", "31337oops"})
    EXPECT_THROW(ChargewireConnection::BuildConnectOptions({{"port", p}}), odbcabstraction::DriverException);
}
TEST(BuildConnectOptionsTests, MutualTlsAndPath) {
  auto o = ChargewireConnection::BuildConnectOptions({{"clientCertificate", "/client.pem"}, {"clientKey", "/key.pem"}, {"basePath", "/custom/chargewire"}});
  EXPECT_EQ(o.tls.cert_file, "/client.pem"); EXPECT_EQ(o.tls.key_file, "/key.pem"); EXPECT_EQ(o.base_path, "/custom/chargewire");
}

TEST(PopulateCallOptionsTest, ConnectionTimeout) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  // Expect default timeout to be -1
  ASSERT_EQ(TimeoutDuration{-1.0},
            connection.PopulateCallOptions(Connection::ConnPropertyMap()).timeout);

  connection.SetAttribute(Connection::CONNECTION_TIMEOUT, static_cast<uint32_t>(10));
  ASSERT_EQ(TimeoutDuration{10.0},
            connection.PopulateCallOptions(Connection::ConnPropertyMap()).timeout);
}

TEST(PopulateCallOptionsTest, GenericOption) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  Connection::ConnPropertyMap properties;
  properties["Foo"] = "Bar";
  auto options = connection.PopulateCallOptions(properties);
  auto headers = options.headers;
  ASSERT_EQ(1, headers.size());

  // Header name must be lower-case because gRPC will crash if it is not lower-case.
  ASSERT_EQ("foo", headers[0].first);

  // Header value should preserve case.
  ASSERT_EQ("Bar", headers[0].second);
}

TEST(PopulateCallOptionsTest, GenericOptionWithSpaces) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  Connection::ConnPropertyMap properties;
  properties["Persist Security Info"] = "False";
  auto options = connection.PopulateCallOptions(properties);
  auto headers = options.headers;
  // Header names with spaces must be omitted or gRPC will crash.
  ASSERT_TRUE(headers.empty());
}

TEST(AutocommitTests, DefaultAutocommitOn) {
  ChargewireConnection connection(odbcabstraction::V_3);
  EXPECT_TRUE(connection.GetAutoCommit());
}

TEST(AutocommitTests, SetAutocommitOff) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  connection.SetAutoCommit(false);
  EXPECT_FALSE(connection.GetAutoCommit());

  // Setting to same value is a no-op
  connection.SetAutoCommit(false);
  EXPECT_FALSE(connection.GetAutoCommit());

  connection.Close();
}

TEST(AutocommitTests, GetCurrentTransactionInvalid) {
  ChargewireConnection connection(odbcabstraction::V_3);

  // When autocommit is ON, there should be no active transaction
  const auto& txn = connection.GetCurrentTransaction();
  EXPECT_FALSE(txn.is_valid());
}

TEST(AutocommitTests, EndTransactionNoOpWhenNoTransaction) {
  ChargewireConnection connection(odbcabstraction::V_3);
  connection.SetClosed(false);

  // EndTransaction with no active transaction should be a no-op (not throw)
  connection.EndTransaction(true);
  connection.EndTransaction(false);

  connection.Close();
}

TEST(ConnectionErrorTests, ConnectToNonExistentServerGivesUserFriendlyError) {
  ChargewireConnection connection(odbcabstraction::V_3);

  // Use a port where no server is running
  Connection::ConnPropertyMap properties = {
    {ChargewireConnection::HOST, std::string("127.0.0.1")},
    {ChargewireConnection::PORT, std::string("19999")},
    {ChargewireConnection::USE_ENCRYPTION, std::string("false")},
    {ChargewireConnection::USER, std::string("user")},
    {ChargewireConnection::PASSWORD, std::string("pass")},
  };
  // Set a short login timeout so the test doesn't hang
  connection.SetAttribute(Connection::LOGIN_TIMEOUT, static_cast<uint32_t>(3));

  std::vector<std::string> missing_attr;
  try {
    connection.Connect(properties, missing_attr);
    FAIL() << "Expected exception when connecting to non-existent server";
  } catch (const odbcabstraction::CommunicationException &e) {
    // Should be a CommunicationException with actionable message
    std::string msg = e.GetMessageText();
    EXPECT_NE(msg.find("Verify the host and port"), std::string::npos)
        << "Error message should contain actionable guidance, got: " << msg;
    EXPECT_EQ(e.GetSqlState(), "08S01")
        << "SQLSTATE should be 08S01 (Communication link failure)";
  } catch (const std::exception &e) {
    // If it's a different exception type, that's also OK as long as
    // it's not the raw gRPC message
    std::string msg = e.what();
    EXPECT_EQ(msg.find("Could not finish writing"), std::string::npos)
        << "Raw gRPC error should not be exposed to users, got: " << msg;
  }
}

} // namespace chargewire
} // namespace driver
