/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "hardcharger_connection.h"

#include <odbcabstraction/platform.h>
#include <odbcabstraction/utils.h>

#include "warp_client.h"


#include "hardcharger_statement.h"

#include "utils.h"

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>
#include <odbcabstraction/exceptions.h>

#include <fstream>
#include "browser.h"
#include <sql.h>
#include <sqlext.h>



#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

namespace driver {
namespace hardcharger {

using arrow::Result;
using arrow::Status;
using driver::odbcabstraction::AsBool;
using driver::odbcabstraction::Connection;
using driver::odbcabstraction::DriverException;
using driver::odbcabstraction::CommunicationException;
using driver::odbcabstraction::OdbcVersion;
using driver::odbcabstraction::Statement;

const std::string HardchargerConnection::DSN = "dsn";
const std::string HardchargerConnection::DRIVER = "driver";
const std::string HardchargerConnection::HOST = "host";
const std::string HardchargerConnection::PORT = "port";
const std::string HardchargerConnection::USER = "user";
const std::string HardchargerConnection::USER_ID = "user id";
const std::string HardchargerConnection::UID = "uid";
const std::string HardchargerConnection::PASSWORD = "password";
const std::string HardchargerConnection::PWD = "pwd";
const std::string HardchargerConnection::TOKEN = "token";
const std::string HardchargerConnection::USE_ENCRYPTION = "useEncryption";
const std::string HardchargerConnection::DISABLE_CERTIFICATE_VERIFICATION = "disableCertificateVerification";
const std::string HardchargerConnection::TRUSTED_CERTS = "trustedCerts";
const std::string HardchargerConnection::USE_SYSTEM_TRUST_STORE = "useSystemTrustStore";
const std::string HardchargerConnection::STRING_COLUMN_LENGTH = "StringColumnLength";
const std::string HardchargerConnection::USE_EXTENDED_FLIGHTSQL_BUFFER = "UseExtendedFlightSQLBuffer";
const std::string HardchargerConnection::USE_WIDE_CHAR = "UseWideChar";
const std::string HardchargerConnection::CHUNK_BUFFER_CAPACITY = "ChunkBufferCapacity";
const std::string HardchargerConnection::HIDE_SQL_TABLES_LISTING = "HideSQLTablesListing";
const std::string HardchargerConnection::AUTH_TYPE = "authType";
const std::string HardchargerConnection::OAUTH_SERVER_PORT = "oauthServerPort";
const std::string HardchargerConnection::SEND_PING_FRAME = "SendPingFrame";
const std::string HardchargerConnection::PING_FRAME_INTERVAL_MS = "PingFrameIntervalMilliseconds";
const std::string HardchargerConnection::PING_FRAME_TIMEOUT_MS = "PingFrameTimeoutMilliseconds";
const std::string HardchargerConnection::MAX_PINGS_WITHOUT_DATA = "MaxPingsWithoutData";
const std::string HardchargerConnection::DEFAULT_CATALOG = "defaultCatalog";

const std::vector<std::string> HardchargerConnection::ALL_KEYS = {
    HardchargerConnection::DSN, HardchargerConnection::DRIVER, HardchargerConnection::HOST, HardchargerConnection::PORT,
    HardchargerConnection::TOKEN, HardchargerConnection::UID, HardchargerConnection::USER_ID, HardchargerConnection::PWD,
    HardchargerConnection::USE_ENCRYPTION, HardchargerConnection::TRUSTED_CERTS, HardchargerConnection::USE_SYSTEM_TRUST_STORE,
    HardchargerConnection::DISABLE_CERTIFICATE_VERIFICATION, HardchargerConnection::STRING_COLUMN_LENGTH,
    HardchargerConnection::USE_WIDE_CHAR,
    HardchargerConnection::HIDE_SQL_TABLES_LISTING, HardchargerConnection::AUTH_TYPE,
    HardchargerConnection::DEFAULT_CATALOG, "basePath", "defaultSchema", "queryTag", "sessionTag",
    "clientCertificate", "clientKey", "oauthTimeoutSeconds", "oauthBrowser"};

namespace {

const std::set<std::string, odbcabstraction::CaseInsensitiveComparator> BUILT_IN_PROPERTIES = {
    HardchargerConnection::HOST,
    HardchargerConnection::PORT,
    HardchargerConnection::USER,
    HardchargerConnection::USER_ID,
    HardchargerConnection::UID,
    HardchargerConnection::PASSWORD,
    HardchargerConnection::PWD,
    HardchargerConnection::TOKEN,
    HardchargerConnection::USE_ENCRYPTION,
    HardchargerConnection::DISABLE_CERTIFICATE_VERIFICATION,
    HardchargerConnection::TRUSTED_CERTS,
    HardchargerConnection::USE_SYSTEM_TRUST_STORE,
    HardchargerConnection::STRING_COLUMN_LENGTH,
    HardchargerConnection::USE_WIDE_CHAR,
    HardchargerConnection::USE_EXTENDED_FLIGHTSQL_BUFFER,
    HardchargerConnection::AUTH_TYPE,
    HardchargerConnection::OAUTH_SERVER_PORT,
    HardchargerConnection::SEND_PING_FRAME,
    HardchargerConnection::PING_FRAME_INTERVAL_MS,
    HardchargerConnection::PING_FRAME_TIMEOUT_MS,
    HardchargerConnection::MAX_PINGS_WITHOUT_DATA,
    HardchargerConnection::DEFAULT_CATALOG
};

Connection::ConnPropertyMap::const_iterator
TrackMissingRequiredProperty(const std::string &property,
                             const Connection::ConnPropertyMap &properties,
                             std::vector<std::string> &missing_attr) {
  auto prop_iter =
      properties.find(property);
  if (properties.end() == prop_iter) {
    missing_attr.push_back(property);
  }
  return prop_iter;
}
} // namespace

namespace {
std::string Property(const Connection::ConnPropertyMap& p, const std::string& k, const std::string& fallback = "") {
  auto it = p.find(k); return it == p.end() ? fallback : it->second;
}
std::string QuoteIdentifier(const std::string& s) {
  std::string result = "\"";
  for (char c : s) { result += c; if (c == '\"') result += c; }
  return result + "\"";
}
}
warpsql::client::ConnectOptions HardchargerConnection::BuildConnectOptions(const ConnPropertyMap& p) {
  warpsql::client::ConnectOptions o;
  o.host = Property(p, HOST, "localhost");
  const auto port = Property(p, PORT, "31337");
  size_t used = 0;
  unsigned long n = 0;
  try { n = std::stoul(port, &used); } catch (...) { throw DriverException("Invalid port: " + port, "HY024"); }
  if (used != port.size() || n == 0 || n > 65535) throw DriverException("Invalid port: " + port, "HY024");
  o.port = static_cast<uint16_t>(n);
  o.base_path = Property(p, "basePath", "/warp");
  if (o.base_path.empty() || o.base_path[0] != '/' || o.base_path.find_first_of("?#\r\n") != std::string::npos)
    throw DriverException("basePath must be an absolute URL path", "HY024");
  o.tls.enabled = AsBool(p, USE_ENCRYPTION).value_or(true);
  o.tls.skip_verify = AsBool(p, DISABLE_CERTIFICATE_VERIFICATION).value_or(false);
  o.tls.root_certs_file = Property(p, TRUSTED_CERTS);
  o.tls.cert_file = Property(p, "clientCertificate");
  o.tls.key_file = Property(p, "clientKey");
  o.user_agent = "hardcharger-odbc/" HARDCHARGER_ODBC_VERSION;
  return o;
}
void HardchargerConnection::Connect(const ConnPropertyMap& properties, std::vector<std::string>& missing_attr) {
  try {
    auto options = BuildConnectOptions(properties);
    if (auto timeout = GetAttribute(LOGIN_TIMEOUT); timeout && boost::get<uint32_t>(*timeout) > 0)
      options.connect_timeout = std::chrono::seconds(boost::get<uint32_t>(*timeout));
    auto client = std::make_unique<warpsql::client::Client>(options);
    const auto user = Property(properties, UID, Property(properties, USER_ID, Property(properties, USER)));
    const auto password = Property(properties, PWD, Property(properties, PASSWORD));
    const auto auth = boost::algorithm::to_lower_copy(Property(properties, AUTH_TYPE, "basic"));
    ThrowWarpErrors([&] {
      if (auth == "token" || auth == "access_token") {
        auto token = Property(properties, TOKEN);
        if (token.empty()) throw DriverException("token is required for authType=token", "28000");
        client->SetAccessToken(token);
      } else if (auth == "token_exchange") {
        auto token = Property(properties, TOKEN);
        if (token.empty()) throw DriverException("token is required for authType=access_token", "28000");
        client->SignInWithToken(token);
      } else if (auth == "basic" || auth == "password") {
        if (!Property(properties, TOKEN).empty()) client->SetAccessToken(Property(properties, TOKEN));
        else client->SignInWithPassword(user, password);
      } else if (auth == "client_certificate" || auth == "mtls") {
        client->SignInWithClientCertificate();
      } else if (auth == "external") {
        const auto seconds = AsInt32(1, properties, "oauthTimeoutSeconds").value_or(300);
        client->SignInWithBrowser([&](const std::string& url) { OpenBrowser(url, Property(properties, "oauthBrowser")); }, std::chrono::seconds(seconds));
      } else throw DriverException("Unsupported authType: " + auth, "HY024");
    });
    boost::json::object session_options;
    if (!Property(properties, DEFAULT_CATALOG).empty()) session_options["catalog"] = Property(properties, DEFAULT_CATALOG);
    if (!Property(properties, "defaultSchema").empty()) session_options["schema"] = Property(properties, "defaultSchema");
    sql_client_ = std::make_unique<WarpSqlClient>(std::move(client), session_options);
    closed_ = false;
    transaction_ = Transaction("");
    info_.SetProperty(SQL_USER_NAME, user);
    attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_FALSE);
    attribute_[CURRENT_CATALOG] = Property(properties, DEFAULT_CATALOG);
    PopulateMetadataSettings(properties);
    PopulateCallOptions(properties);
    call_options_.query_tag = Property(properties, "queryTag");
    for (const auto& name : {"queryTag", "sessionTag"}) {
      const auto value = Property(properties, name);
      if (value.empty()) continue;
      boost::system::error_code ec;
      const auto tag = boost::json::parse(value, ec);
      if (ec || !tag.is_object()) throw DriverException(std::string(name) + " must be a JSON object", "HY024");
    }
    if (const auto tag = Property(properties, "sessionTag"); !tag.empty()) {
      std::string literal = "'";
      for (char c : tag) { literal += c; if (c == '\'') literal += c; }
      literal += "'";
      ThrowIfNotOK(sql_client_->ExecuteUpdate(call_options_, "SET SESSION hardcharger.session_tag = " + literal).status());
    }
  } catch (...) {
    closed_ = true;
    attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_TRUE);
    sql_client_.reset();
    throw;
  }
}

void HardchargerConnection::PopulateMetadataSettings(const Connection::ConnPropertyMap &conn_property_map) {
  metadata_settings_.string_column_length_ = GetStringColumnLength(conn_property_map);
  metadata_settings_.use_wide_char_ = GetUseWideChar(conn_property_map);
  metadata_settings_.use_extended_flightsql_buffer_ = GetUseExtendedFlightSQLBuffer(conn_property_map);
  metadata_settings_.chunk_buffer_capacity_ = GetChunkBufferCapacity(conn_property_map);
  metadata_settings_.hide_sql_tables_listing_ = GetHideSQLTablesListing(conn_property_map);
}

boost::optional<int32_t> HardchargerConnection::GetStringColumnLength(const Connection::ConnPropertyMap &conn_property_map) {
  const int32_t min_string_column_length = 1;

  try {
    return AsInt32(min_string_column_length, conn_property_map, HardchargerConnection::STRING_COLUMN_LENGTH);
  } catch (const std::exception& e) {
    diagnostics_.AddWarning(
            std::string("Invalid value for connection property " + HardchargerConnection::STRING_COLUMN_LENGTH +
                        ". Please ensure it has a valid numeric value. Message: " + e.what()),
            "01000", odbcabstraction::ODBCErrorCodes_GENERAL_WARNING);
  }

  return boost::none;
}

bool HardchargerConnection::GetUseExtendedFlightSQLBuffer(const ConnPropertyMap &connPropertyMap) {
  bool default_value = false;
  return AsBool(connPropertyMap, HardchargerConnection::USE_EXTENDED_FLIGHTSQL_BUFFER).value_or(default_value);
}

bool HardchargerConnection::GetUseWideChar(const ConnPropertyMap &connPropertyMap) {
  #if defined _WIN32 || defined _WIN64
  // Windows should use wide chars by default
  bool default_value = true;
  #else
  // Mac and Linux should not use wide chars by default
  bool default_value = false;
#endif
  return AsBool(connPropertyMap, HardchargerConnection::USE_WIDE_CHAR).value_or(default_value);
}

size_t HardchargerConnection::GetChunkBufferCapacity(const ConnPropertyMap &connPropertyMap) {
  size_t default_value = 5;
  try {
    return AsInt32(1, connPropertyMap, HardchargerConnection::CHUNK_BUFFER_CAPACITY).value_or(default_value);
  } catch (const std::exception& e) {
    diagnostics_.AddWarning(
            std::string("Invalid value for connection property " + HardchargerConnection::CHUNK_BUFFER_CAPACITY +
                        ". Please ensure it has a valid numeric value. Message: " + e.what()),
            "01000", odbcabstraction::ODBCErrorCodes_GENERAL_WARNING);
  }

  return default_value;
}

bool HardchargerConnection::GetHideSQLTablesListing(const ConnPropertyMap &connPropertyMap) {
  bool default_value = false;
  return AsBool(connPropertyMap, HardchargerConnection::HIDE_SQL_TABLES_LISTING).value_or(default_value);
}

bool HardchargerConnection::GetSendPingFrame(const ConnPropertyMap &connPropertyMap) {
  bool default_value = false;
  return AsBool(connPropertyMap, HardchargerConnection::SEND_PING_FRAME).value_or(default_value);
}

boost::optional<int> HardchargerConnection::GetPingFrameIntervalMilliseconds(const ConnPropertyMap &connPropertyMap) {
  const int min_ping_frame_interval = 1000;

  try {
    return AsInt32(min_ping_frame_interval, connPropertyMap, HardchargerConnection::PING_FRAME_INTERVAL_MS);
  } catch (const std::exception& e) {
    return boost::none;
  }
}

boost::optional<int> HardchargerConnection::GetPingFrameTimeoutMilliseconds(const ConnPropertyMap &connPropertyMap) {
  const int min_ping_frame_timeout = 1000;

  try {
    return AsInt32(min_ping_frame_timeout, connPropertyMap, HardchargerConnection::PING_FRAME_TIMEOUT_MS);
  } catch (const std::exception& e) {
    return boost::none;
  }
}

boost::optional<int> HardchargerConnection::GetMaxPingsWithoutData(const ConnPropertyMap &connPropertyMap) {
  const int min_max_pings_without_data = 0;

  try {
    return AsInt32(min_max_pings_without_data, connPropertyMap, HardchargerConnection::MAX_PINGS_WITHOUT_DATA);
  } catch (const std::exception& e) {
    return boost::none;
  }
}

const CallOptions &
HardchargerConnection::PopulateCallOptions(const ConnPropertyMap &props) {
  // Set CONNECTION_TIMEOUT attribute or LOGIN_TIMEOUT depending on if this
  // is the first request.
  const boost::optional<Connection::Attribute> &connection_timeout = closed_ ?
      GetAttribute(LOGIN_TIMEOUT) : GetAttribute(CONNECTION_TIMEOUT);
  if (connection_timeout && boost::get<uint32_t>(*connection_timeout) > 0) {
    call_options_.timeout =
        TimeoutDuration{static_cast<double>(boost::get<uint32_t>(*connection_timeout))};
  }

  for (auto prop : props) {
    if (BUILT_IN_PROPERTIES.count(prop.first) != 0) {
      continue;
    }

    if (prop.first.find(' ') != std::string::npos) {
      // Connection properties containing spaces will crash gRPC, but some tools
      // such as the OLE DB to ODBC bridge generate unused properties containing spaces.
      diagnostics_.AddWarning(
          std::string("Ignoring connection option " + prop.first) +
              ". Server-specific options must be valid HTTP header names and " +
              "cannot contain spaces.",
          "01000", odbcabstraction::ODBCErrorCodes_GENERAL_WARNING);
      continue;
    }

    // Note: header names must be lower case for gRPC.
    // gRPC will crash if they are not lower-case.
    std::string key_lc = boost::algorithm::to_lower_copy(prop.first);
    call_options_.headers.emplace_back(std::make_pair(key_lc, prop.second));
  }

  return call_options_;
}

void HardchargerConnection::Close() {
  if (sql_client_) {
    // Closing a Warp SQL session rolls back an uncommitted transaction.
    try { sql_client_->Close(); } catch (...) { sql_client_.reset(); closed_ = true; throw; }
    sql_client_.reset();
  }
  transaction_ = Transaction("");
  closed_ = true;
  attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_TRUE);
}

void HardchargerConnection::SetAutoCommit(bool autocommit) {
  if (autocommit == autocommit_) {
    return; // No change
  }

  // Switching OFF → ON: implicitly commit any open transaction
  if (autocommit && transaction_.is_valid()) {
    ThrowIfNotOK(sql_client_->Commit(call_options_, transaction_));
    transaction_ = Transaction("");
  }

  autocommit_ = autocommit;
}

bool HardchargerConnection::GetAutoCommit() const {
  return autocommit_;
}

void HardchargerConnection::EndTransaction(bool commit) {
  if (!transaction_.is_valid()) {
    // No active transaction — nothing to do (ODBC spec allows this)
    return;
  }

  if (commit) {
    ThrowIfNotOK(sql_client_->Commit(call_options_, transaction_));
  } else {
    ThrowIfNotOK(sql_client_->Rollback(call_options_, transaction_));
  }
  transaction_ = Transaction("");
}

const Transaction& HardchargerConnection::GetCurrentTransaction() const {
  return transaction_;
}

void HardchargerConnection::EnsureTransaction() {
  if (autocommit_ || transaction_.is_valid()) {
    return; // Autocommit mode or transaction already active
  }

  auto result = sql_client_->BeginTransaction(call_options_);
  ThrowIfNotOK(result.status());
  transaction_ = std::move(result).ValueUnsafe();
}

std::shared_ptr<Statement> HardchargerConnection::CreateStatement() {
  return std::shared_ptr<Statement>(
      new HardchargerStatement(
              diagnostics_,
              *sql_client_,
              call_options_,
              metadata_settings_,
              *this
              )
      );
}

bool HardchargerConnection::SetAttribute(Connection::AttributeId attribute,
                                       const Connection::Attribute &value) {
  switch (attribute) {
  case ACCESS_MODE:
    // We will always return read-write.
    return CheckIfSetToOnlyValidValue(value, static_cast<uint32_t>(SQL_MODE_READ_WRITE));
  case PACKET_SIZE:
    return CheckIfSetToOnlyValidValue(value, static_cast<uint32_t>(0));
  case CURRENT_CATALOG:
    if (sql_client_) ThrowIfNotOK(sql_client_->ExecuteUpdate(call_options_, "USE " + QuoteIdentifier(boost::get<std::string>(value))).status());
    attribute_[attribute] = value;
    return true;
  default:
    attribute_[attribute] = value;
    return true;
  }
}

boost::optional<Connection::Attribute>
HardchargerConnection::GetAttribute(Connection::AttributeId attribute) {
  switch (attribute) {
  case ACCESS_MODE:
    // FlightSQL does not provide this metadata.
    return boost::make_optional(Attribute(static_cast<uint32_t>(SQL_MODE_READ_WRITE)));
  case PACKET_SIZE:
    return boost::make_optional(Attribute(static_cast<uint32_t>(0)));
  default:
    const auto &it = attribute_.find(attribute);
    return boost::make_optional(it != attribute_.end(), it->second);
  }
}

Connection::Info HardchargerConnection::GetInfo(uint16_t info_type) {
  auto result = info_.GetInfo(info_type);
  if (info_type == SQL_DBMS_NAME || info_type == SQL_SERVER_NAME) {
    // Update the database component reported in error messages.
    // We do this lazily for performance reasons.
    diagnostics_.SetDataSourceComponent(boost::get<std::string>(result));
  }
  return result;
}

HardchargerConnection::HardchargerConnection(OdbcVersion odbc_version, const std::string &driver_version)
    : diagnostics_("GizmoData", "Hardcharger", odbc_version),
      odbc_version_(odbc_version), info_(call_options_, sql_client_, driver_version),
      closed_(true) {
  attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_TRUE);
  attribute_[LOGIN_TIMEOUT] = static_cast<uint32_t>(0);
  attribute_[CONNECTION_TIMEOUT] = static_cast<uint32_t>(0);
  attribute_[CURRENT_CATALOG] = "";
}
odbcabstraction::Diagnostics &HardchargerConnection::GetDiagnostics() {
  return diagnostics_;
}

void HardchargerConnection::SetClosed(bool is_closed) {
  closed_ = is_closed;
}

} // namespace hardcharger
} // namespace driver
