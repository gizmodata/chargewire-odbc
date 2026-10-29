/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "chargewire_statement_get_tables.h"
#include "odbcabstraction/types.h"
#include <odbcabstraction/spi/statement.h>
#include <odbcabstraction/diagnostics.h>

#include "chargewire_client.h"

namespace driver {
namespace chargewire_odbc {

class ChargewireConnection;

class ChargewireStatement : public odbcabstraction::Statement {

private:
  odbcabstraction::Diagnostics diagnostics_;
  std::map<StatementAttributeId, Attribute> attribute_;
  CallOptions call_options_;
  ChargewireClient &sql_client_;
  std::shared_ptr<odbcabstraction::ResultSet> current_result_set_;
  std::shared_ptr<ChargewirePreparedStatement> prepared_statement_;
  std::shared_ptr<ResultInfo> flight_info_;
  const odbcabstraction::MetadataSettings& metadata_settings_;
  ChargewireConnection& connection_;
  long update_count_ = -1;

  std::shared_ptr<odbcabstraction::ResultSet>
  GetTables(const std::string *catalog_name, const std::string *schema_name,
            const std::string *table_name, const std::string *table_type,
            const ColumnNames &column_names);

public:
  ChargewireStatement(
      const odbcabstraction::Diagnostics &diagnostics,
      ChargewireClient &sql_client,
      CallOptions call_options,
      const odbcabstraction::MetadataSettings& metadata_settings,
      ChargewireConnection& connection);

  ~ChargewireStatement() override;

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

  void SetParameters(const odbcabstraction::ParameterSet& params) override;

  odbcabstraction::Diagnostics &GetDiagnostics() override;

  void Cancel() override;
};
} // namespace chargewire
} // namespace driver
