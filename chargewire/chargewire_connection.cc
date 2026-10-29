/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "chargewire_connection.h"

#include <odbcabstraction/platform.h>
#include <odbcabstraction/utils.h>

#include "chargewire_client.h"
#include "driver_profile.h"


#include "chargewire_statement.h"

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
namespace chargewire_odbc {

using arrow::Result;
using arrow::Status;
using driver::odbcabstraction::AsBool;
using driver::odbcabstraction::Connection;
using driver::odbcabstraction::DriverException;
using driver::odbcabstraction::CommunicationException;
using driver::odbcabstraction::OdbcVersion;
using driver::odbcabstraction::Statement;

const std::string ChargewireConnection::DSN = "dsn";
const std::string ChargewireConnection::DRIVER = "driver";
const std::string ChargewireConnection::HOST = "host";
const std::string ChargewireConnection::PORT = "port";
const std::string ChargewireConnection::USER = "user";
const std::string ChargewireConnection::USER_ID = "user id";
const std::string ChargewireConnection::UID = "uid";
const std::string ChargewireConnection::PASSWORD = "password";
const std::string ChargewireConnection::PWD = "pwd";
const std::string ChargewireConnection::TOKEN = "token";
const std::string ChargewireConnection::USE_ENCRYPTION = "useEncryption";
const std::string ChargewireConnection::DISABLE_CERTIFICATE_VERIFICATION = "disableCertificateVerification";
const std::string ChargewireConnection::TRUSTED_CERTS = "trustedCerts";
const std::string ChargewireConnection::USE_SYSTEM_TRUST_STORE = "useSystemTrustStore";
const std::string ChargewireConnection::STRING_COLUMN_LENGTH = "StringColumnLength";
const std::string ChargewireConnection::USE_EXTENDED_FLIGHTSQL_BUFFER = "UseExtendedFlightSQLBuffer";
const std::string ChargewireConnection::USE_WIDE_CHAR = "UseWideChar";
const std::string ChargewireConnection::CHUNK_BUFFER_CAPACITY = "ChunkBufferCapacity";
const std::string ChargewireConnection::HIDE_SQL_TABLES_LISTING = "HideSQLTablesListing";
const std::string ChargewireConnection::AUTH_TYPE = "authType";
const std::string ChargewireConnection::OAUTH_SERVER_PORT = "oauthServerPort";
const std::string ChargewireConnection::SEND_PING_FRAME = "SendPingFrame";
const std::string ChargewireConnection::PING_FRAME_INTERVAL_MS = "PingFrameIntervalMilliseconds";
const std::string ChargewireConnection::PING_FRAME_TIMEOUT_MS = "PingFrameTimeoutMilliseconds";
const std::string ChargewireConnection::MAX_PINGS_WITHOUT_DATA = "MaxPingsWithoutData";
const std::string ChargewireConnection::DEFAULT_CATALOG = "defaultCatalog";

const std::vector<std::string> ChargewireConnection::ALL_KEYS = {
    ChargewireConnection::DSN, ChargewireConnection::DRIVER, ChargewireConnection::HOST, ChargewireConnection::PORT,
    ChargewireConnection::TOKEN, ChargewireConnection::UID, ChargewireConnection::PWD,
    ChargewireConnection::USE_ENCRYPTION, ChargewireConnection::TRUSTED_CERTS, ChargewireConnection::USE_SYSTEM_TRUST_STORE,
    ChargewireConnection::DISABLE_CERTIFICATE_VERIFICATION, ChargewireConnection::AUTH_TYPE,
    ChargewireConnection::DEFAULT_CATALOG, "uri"};

namespace {

const std::set<std::string, odbcabstraction::CaseInsensitiveComparator> BUILT_IN_PROPERTIES = {
    ChargewireConnection::HOST,
    ChargewireConnection::PORT,
    ChargewireConnection::USER,
    ChargewireConnection::USER_ID,
    ChargewireConnection::UID,
    ChargewireConnection::PASSWORD,
    ChargewireConnection::PWD,
    ChargewireConnection::TOKEN,
    ChargewireConnection::USE_ENCRYPTION,
    ChargewireConnection::DISABLE_CERTIFICATE_VERIFICATION,
    ChargewireConnection::TRUSTED_CERTS,
    ChargewireConnection::USE_SYSTEM_TRUST_STORE,
    ChargewireConnection::STRING_COLUMN_LENGTH,
    ChargewireConnection::USE_WIDE_CHAR,
    ChargewireConnection::USE_EXTENDED_FLIGHTSQL_BUFFER,
    ChargewireConnection::AUTH_TYPE,
    ChargewireConnection::OAUTH_SERVER_PORT,
    ChargewireConnection::SEND_PING_FRAME,
    ChargewireConnection::PING_FRAME_INTERVAL_MS,
    ChargewireConnection::PING_FRAME_TIMEOUT_MS,
    ChargewireConnection::MAX_PINGS_WITHOUT_DATA,
    ChargewireConnection::DEFAULT_CATALOG
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
chargewire::client::ConnectOptions ChargewireConnection::BuildConnectOptions(const ConnPropertyMap& raw) {
  const auto p = NormalizeConnectionProperties(raw);
  chargewire::client::ConnectOptions o;
  o.host = Property(p, HOST, "localhost");
  const auto port = Property(p, PORT, std::to_string(GetDriverProfile().default_port));
  size_t used = 0;
  unsigned long n = 0;
  try { n = std::stoul(port, &used); } catch (...) { throw DriverException("Invalid port: " + port, "HY024"); }
  if (used != port.size() || n == 0 || n > 65535) throw DriverException("Invalid port: " + port, "HY024");
  o.port = static_cast<uint16_t>(n);
  o.base_path = Property(p, "basePath", "/chargewire");
  if (o.base_path.empty() || o.base_path[0] != '/' || o.base_path.find_first_of("?#\r\n") != std::string::npos)
    throw DriverException("basePath must be an absolute URL path", "HY024");
  o.tls.enabled = AsBool(p, USE_ENCRYPTION).value_or(true);
  o.tls.skip_verify = AsBool(p, DISABLE_CERTIFICATE_VERIFICATION).value_or(false);
  o.tls.root_certs_file = Property(p, TRUSTED_CERTS);
  o.tls.cert_file = Property(p, "clientCertificate");
  o.tls.key_file = Property(p, "clientKey");
  o.user_agent = GetDriverProfile().user_agent;
  return o;
}
void ChargewireConnection::Connect(const ConnPropertyMap& raw, std::vector<std::string>& missing_attr) {
  const auto properties = NormalizeConnectionProperties(raw);
  try {
    auto options = BuildConnectOptions(properties);
    if (auto timeout = GetAttribute(LOGIN_TIMEOUT); timeout && boost::get<uint32_t>(*timeout) > 0)
      options.connect_timeout = std::chrono::seconds(boost::get<uint32_t>(*timeout));
    auto client = std::make_unique<chargewire::client::Client>(options);
    const auto user = Property(properties, UID, Property(properties, USER_ID, Property(properties, USER)));
    const auto password = Property(properties, PWD, Property(properties, PASSWORD));
    const auto auth = boost::algorithm::to_lower_copy(Property(properties, AUTH_TYPE, "basic"));
    ThrowChargewireErrors([&] {
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
    sql_client_ = std::make_unique<ChargewireClient>(std::move(client), session_options);
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
    InitializeProductSession(*sql_client_, properties, call_options_);
  } catch (...) {
    closed_ = true;
    attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_TRUE);
    sql_client_.reset();
    throw;
  }
}

void ChargewireConnection::PopulateMetadataSettings(const Connection::ConnPropertyMap &conn_property_map) {
  metadata_settings_.string_column_length_ = GetStringColumnLength(conn_property_map);
  metadata_settings_.use_wide_char_ = GetUseWideChar(conn_property_map);
  metadata_settings_.use_extended_flightsql_buffer_ = GetUseExtendedFlightSQLBuffer(conn_property_map);
  metadata_settings_.chunk_buffer_capacity_ = GetChunkBufferCapacity(conn_property_map);
  metadata_settings_.hide_sql_tables_listing_ = GetHideSQLTablesListing(conn_property_map);
}

boost::optional<int32_t> ChargewireConnection::GetStringColumnLength(const Connection::ConnPropertyMap &conn_property_map) {
  const int32_t min_string_column_length = 1;

  try {
    return AsInt32(min_string_column_length, conn_property_map, ChargewireConnection::STRING_COLUMN_LENGTH);
  } catch (const std::exception& e) {
    diagnostics_.AddWarning(
            std::string("Invalid value for connection property " + ChargewireConnection::STRING_COLUMN_LENGTH +
                        ". Please ensure it has a valid numeric value. Message: " + e.what()),
            "01000", odbcabstraction::ODBCErrorCodes_GENERAL_WARNING);
  }

  return boost::none;
}

bool ChargewireConnection::GetUseExtendedFlightSQLBuffer(const ConnPropertyMap &connPropertyMap) {
  bool default_value = false;
  return AsBool(connPropertyMap, ChargewireConnection::USE_EXTENDED_FLIGHTSQL_BUFFER).value_or(default_value);
}

bool ChargewireConnection::GetUseWideChar(const ConnPropertyMap &connPropertyMap) {
  #if defined _WIN32 || defined _WIN64
  // Windows should use wide chars by default
  bool default_value = true;
  #else
  // Mac and Linux should not use wide chars by default
  bool default_value = false;
#endif
  return AsBool(connPropertyMap, ChargewireConnection::USE_WIDE_CHAR).value_or(default_value);
}

size_t ChargewireConnection::GetChunkBufferCapacity(const ConnPropertyMap &connPropertyMap) {
  size_t default_value = 5;
  try {
    return AsInt32(1, connPropertyMap, ChargewireConnection::CHUNK_BUFFER_CAPACITY).value_or(default_value);
  } catch (const std::exception& e) {
    diagnostics_.AddWarning(
            std::string("Invalid value for connection property " + ChargewireConnection::CHUNK_BUFFER_CAPACITY +
                        ". Please ensure it has a valid numeric value. Message: " + e.what()),
            "01000", odbcabstraction::ODBCErrorCodes_GENERAL_WARNING);
  }

  return default_value;
}

bool ChargewireConnection::GetHideSQLTablesListing(const ConnPropertyMap &connPropertyMap) {
  bool default_value = false;
  return AsBool(connPropertyMap, ChargewireConnection::HIDE_SQL_TABLES_LISTING).value_or(default_value);
}

bool ChargewireConnection::GetSendPingFrame(const ConnPropertyMap &connPropertyMap) {
  bool default_value = false;
  return AsBool(connPropertyMap, ChargewireConnection::SEND_PING_FRAME).value_or(default_value);
}

boost::optional<int> ChargewireConnection::GetPingFrameIntervalMilliseconds(const ConnPropertyMap &connPropertyMap) {
  const int min_ping_frame_interval = 1000;

  try {
    return AsInt32(min_ping_frame_interval, connPropertyMap, ChargewireConnection::PING_FRAME_INTERVAL_MS);
  } catch (const std::exception& e) {
    return boost::none;
  }
}

boost::optional<int> ChargewireConnection::GetPingFrameTimeoutMilliseconds(const ConnPropertyMap &connPropertyMap) {
  const int min_ping_frame_timeout = 1000;

  try {
    return AsInt32(min_ping_frame_timeout, connPropertyMap, ChargewireConnection::PING_FRAME_TIMEOUT_MS);
  } catch (const std::exception& e) {
    return boost::none;
  }
}

boost::optional<int> ChargewireConnection::GetMaxPingsWithoutData(const ConnPropertyMap &connPropertyMap) {
  const int min_max_pings_without_data = 0;

  try {
    return AsInt32(min_max_pings_without_data, connPropertyMap, ChargewireConnection::MAX_PINGS_WITHOUT_DATA);
  } catch (const std::exception& e) {
    return boost::none;
  }
}

const CallOptions &
ChargewireConnection::PopulateCallOptions(const ConnPropertyMap &props) {
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

void ChargewireConnection::Close() {
  if (sql_client_) {
    // Closing a Chargewire session rolls back an uncommitted transaction.
    try { sql_client_->Close(); } catch (...) { sql_client_.reset(); closed_ = true; throw; }
    sql_client_.reset();
  }
  transaction_ = Transaction("");
  closed_ = true;
  attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_TRUE);
}

void ChargewireConnection::SetAutoCommit(bool autocommit) {
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

bool ChargewireConnection::GetAutoCommit() const {
  return autocommit_;
}

void ChargewireConnection::EndTransaction(bool commit) {
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

const Transaction& ChargewireConnection::GetCurrentTransaction() const {
  return transaction_;
}

void ChargewireConnection::EnsureTransaction() {
  if (autocommit_ || transaction_.is_valid()) {
    return; // Autocommit mode or transaction already active
  }

  auto result = sql_client_->BeginTransaction(call_options_);
  ThrowIfNotOK(result.status());
  transaction_ = std::move(result).ValueUnsafe();
}

std::shared_ptr<Statement> ChargewireConnection::CreateStatement() {
  return std::shared_ptr<Statement>(
      new ChargewireStatement(
              diagnostics_,
              *sql_client_,
              call_options_,
              metadata_settings_,
              *this
              )
      );
}

bool ChargewireConnection::SetAttribute(Connection::AttributeId attribute,
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
ChargewireConnection::GetAttribute(Connection::AttributeId attribute) {
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

Connection::Info ChargewireConnection::GetInfo(uint16_t info_type) {
  auto result = info_.GetInfo(info_type);
  if (info_type == SQL_DBMS_NAME || info_type == SQL_SERVER_NAME) {
    // Update the database component reported in error messages.
    // We do this lazily for performance reasons.
    diagnostics_.SetDataSourceComponent(boost::get<std::string>(result));
  }
  return result;
}

ChargewireConnection::ChargewireConnection(OdbcVersion odbc_version, const std::string &driver_version)
    : diagnostics_("GizmoData", GetDriverProfile().product, odbc_version),
      odbc_version_(odbc_version), info_(call_options_, sql_client_, driver_version),
      closed_(true) {
  attribute_[CONNECTION_DEAD] = static_cast<uint32_t>(SQL_TRUE);
  attribute_[LOGIN_TIMEOUT] = static_cast<uint32_t>(0);
  attribute_[CONNECTION_TIMEOUT] = static_cast<uint32_t>(0);
  attribute_[CURRENT_CATALOG] = "";
}
odbcabstraction::Diagnostics &ChargewireConnection::GetDiagnostics() {
  return diagnostics_;
}

void ChargewireConnection::SetClosed(bool is_closed) {
  closed_ = is_closed;
}

} // namespace chargewire
} // namespace driver
