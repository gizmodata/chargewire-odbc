/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <odbcabstraction/spi/connection.h>

#include "warp_client.h"
#include <vector>

#include "get_info_cache.h"
#include "odbcabstraction/types.h"

namespace driver {
namespace warpsql_odbc {

class WarpSqlConnection : public odbcabstraction::Connection {

private:
  odbcabstraction::MetadataSettings metadata_settings_;
  std::map<AttributeId, Attribute> attribute_;
  CallOptions call_options_;
  std::unique_ptr<WarpSqlClient> sql_client_;
  GetInfoCache info_;
  odbcabstraction::Diagnostics diagnostics_;
  odbcabstraction::OdbcVersion odbc_version_;
  bool closed_;
  bool autocommit_ = true;
  Transaction transaction_{""};  // empty = no active txn

  void PopulateMetadataSettings(const Connection::ConnPropertyMap &connPropertyMap);

public:
  static const std::vector<std::string> ALL_KEYS;
  static const std::string DSN;
  static const std::string DRIVER;
  static const std::string HOST;
  static const std::string PORT;
  static const std::string USER;
  static const std::string UID;
  static const std::string USER_ID;
  static const std::string PASSWORD;
  static const std::string PWD;
  static const std::string TOKEN;
  static const std::string USE_ENCRYPTION;
  static const std::string DISABLE_CERTIFICATE_VERIFICATION;
  static const std::string TRUSTED_CERTS;
  static const std::string USE_SYSTEM_TRUST_STORE;
  static const std::string STRING_COLUMN_LENGTH;
  static const std::string USE_WIDE_CHAR;
  static const std::string USE_EXTENDED_FLIGHTSQL_BUFFER;
  static const std::string CHUNK_BUFFER_CAPACITY;
  static const std::string HIDE_SQL_TABLES_LISTING;
  static const std::string AUTH_TYPE;
  static const std::string OAUTH_SERVER_PORT;
  static const std::string SEND_PING_FRAME;
  static const std::string PING_FRAME_INTERVAL_MS;
  static const std::string PING_FRAME_TIMEOUT_MS;
  static const std::string MAX_PINGS_WITHOUT_DATA;
  static const std::string DEFAULT_CATALOG;

  explicit WarpSqlConnection(odbcabstraction::OdbcVersion odbc_version, const std::string &driver_version = "0.9.0.0");

  void SetAutoCommit(bool autocommit) override;
  bool GetAutoCommit() const override;
  void EndTransaction(bool commit) override;

  /// \brief Return the current transaction (empty = no_transaction semantics).
  const Transaction& GetCurrentTransaction() const;

  /// \brief Begin a transaction implicitly if autocommit is OFF and no active txn.
  void EnsureTransaction();

  void Connect(const ConnPropertyMap &properties,
               std::vector<std::string> &missing_attr) override;

  void Close() override;

  std::shared_ptr<odbcabstraction::Statement> CreateStatement() override;

  bool SetAttribute(AttributeId attribute, const Attribute &value) override;

  boost::optional<Connection::Attribute>
  GetAttribute(Connection::AttributeId attribute) override;

  Info GetInfo(uint16_t info_type) override;

  static warpsql::client::ConnectOptions BuildConnectOptions(const ConnPropertyMap &properties);
  const CallOptions &PopulateCallOptions(const ConnPropertyMap &properties);

  odbcabstraction::Diagnostics &GetDiagnostics() override;

  /// \brief A setter to the field closed_.
  /// \note Visible for testing
  void SetClosed(bool is_closed);

  boost::optional<int32_t> GetStringColumnLength(const ConnPropertyMap &connPropertyMap);

  bool GetUseWideChar(const ConnPropertyMap &connPropertyMap);

  bool GetUseExtendedFlightSQLBuffer(const ConnPropertyMap &connPropertyMap);

  size_t GetChunkBufferCapacity(const ConnPropertyMap &connPropertyMap);

  bool GetHideSQLTablesListing(const ConnPropertyMap &connPropertyMap);

  static bool GetSendPingFrame(const ConnPropertyMap &connPropertyMap);

  static boost::optional<int> GetPingFrameIntervalMilliseconds(const ConnPropertyMap &connPropertyMap);

  static boost::optional<int> GetPingFrameTimeoutMilliseconds(const ConnPropertyMap &connPropertyMap);

  static boost::optional<int> GetMaxPingsWithoutData(const ConnPropertyMap &connPropertyMap);
};
} // namespace warpsql
} // namespace driver
