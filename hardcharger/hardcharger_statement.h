/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "hardcharger_statement_get_tables.h"
#include "odbcabstraction/types.h"
#include <odbcabstraction/spi/statement.h>
#include <odbcabstraction/diagnostics.h>

#include "warp_client.h"

namespace driver {
namespace hardcharger {

class HardchargerConnection;

class HardchargerStatement : public odbcabstraction::Statement {

private:
  odbcabstraction::Diagnostics diagnostics_;
  std::map<StatementAttributeId, Attribute> attribute_;
  CallOptions call_options_;
  WarpSqlClient &sql_client_;
  std::shared_ptr<odbcabstraction::ResultSet> current_result_set_;
  std::shared_ptr<WarpPreparedStatement> prepared_statement_;
  std::shared_ptr<ResultInfo> flight_info_;
  const odbcabstraction::MetadataSettings& metadata_settings_;
  HardchargerConnection& connection_;
  long update_count_ = -1;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetTables(const std::string *catalog_name, const std::string *schema_name,
            const std::string *table_name, const std::string *table_type,
            const ColumnNames &column_names);

public:
  HardchargerStatement(
      const odbcabstraction::Diagnostics &diagnostics,
      WarpSqlClient &sql_client,
      CallOptions call_options,
      const odbcabstraction::MetadataSettings& metadata_settings,
      HardchargerConnection& connection);

  ~HardchargerStatement() override;

  bool SetAttribute(StatementAttributeId attribute, const Attribute &value) override;

  boost::optional<Attribute> GetAttribute(StatementAttributeId attribute) override;

  boost::optional<std::shared_ptr<odbcabstraction::ResultSetMetadata>>
  Prepare(const std::string &query) override;

  bool ExecutePrepared() override;

  bool Execute(const std::string &query) override;

  std::shared_ptr<odbcabstraction::ResultSet> GetResultSet() override;

  long GetUpdateCount() override;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetTables_V2(const std::string *catalog_name, const std::string *schema_name,
               const std::string *table_name, const std::string *table_type) override;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetTables_V3(const std::string *catalog_name, const std::string *schema_name,
               const std::string *table_name, const std::string *table_type) override;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetColumns_V2(const std::string *catalog_name, const std::string *schema_name,
                const std::string *table_name, const std::string *column_name) override;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetColumns_V3(const std::string *catalog_name, const std::string *schema_name,
                const std::string *table_name, const std::string *column_name) override;

  std::shared_ptr<odbcabstraction::ResultSet> GetTypeInfo_V2(int16_t data_type) override;

  std::shared_ptr<odbcabstraction::ResultSet> GetTypeInfo_V3(int16_t data_type) override;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetPrimaryKeys(const std::string *catalog_name, const std::string *schema_name,
                 const std::string *table_name) override;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetForeignKeys(const std::string *pk_catalog_name, const std::string *pk_schema_name,
                 const std::string *pk_table_name, const std::string *fk_catalog_name,
                 const std::string *fk_schema_name, const std::string *fk_table_name) override;

  std::shared_ptr<odbcabstraction::ResultSetMetadata> GetParameterSchema() override;

  void SetParameters(const std::vector<odbcabstraction::ParameterBinding>& params) override;

  odbcabstraction::Diagnostics &GetDiagnostics() override;

  void Cancel() override;
};
} // namespace hardcharger
} // namespace driver
