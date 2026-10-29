/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "chargewire_result_set_metadata.h"
#include <odbcabstraction/platform.h>
#include "column_metadata.h"
#include <arrow/util/key_value_metadata.h>
#include "utils.h"

#include <odbcabstraction/types.h>
#include <odbcabstraction/exceptions.h>
#include <algorithm>
#include <utility>

namespace driver {
namespace chargewire_odbc {

using namespace odbcabstraction;
using arrow::DataType;
using arrow::Field;
using std::make_optional;
using std::nullopt;

constexpr int32_t DefaultDecimalPrecision = 38;

// This indicates the column length used when the both property StringColumnLength is not specified and
// the server does not provide a length on column metadata.
constexpr int32_t DefaultLengthForVariableLengthColumns = 1024;

namespace {
std::shared_ptr<const arrow::KeyValueMetadata> empty_metadata_map(new arrow::KeyValueMetadata);

inline ColumnMetadata GetMetadata(const std::shared_ptr<Field> &field) {
  const auto &metadata_map = field->metadata();

  ColumnMetadata metadata(metadata_map ? metadata_map : empty_metadata_map);
  return metadata;
}

arrow::Result<int32_t> GetFieldPrecision(const std::shared_ptr<Field> &field) {
  return GetMetadata(field).GetPrecision();
}

// TODO: remove using on updating arrow to latest version
arrow::Result<bool> GetMetadataProperty(const ColumnMetadata &metadata,
                                        const std::string &property_name) {
  std::string result;
  ARROW_ASSIGN_OR_RAISE(result, metadata.metadata_map()->Get(property_name));
  return result == "1";
}
}

size_t ChargewireResultSetMetadata::GetColumnCount() {
  return schema_->num_fields();
}

std::string ChargewireResultSetMetadata::GetColumnName(int column_position) {
  return schema_->field(column_position - 1)->name();
}

std::string ChargewireResultSetMetadata::GetName(int column_position) {
  return schema_->field(column_position - 1)->name();
}

size_t ChargewireResultSetMetadata::GetPrecision(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);

  int32_t column_size = GetFieldPrecision(field).ValueOrElse([] { return 0; });
  SqlDataType data_type_v3 = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);

  return GetColumnSize(data_type_v3, column_size).value_or(0);
}

size_t ChargewireResultSetMetadata::GetScale(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  ColumnMetadata metadata = GetMetadata(field);

  int32_t type_scale = metadata.GetScale().ValueOrElse([] { return 0; });
  SqlDataType data_type_v3 = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);

  return GetTypeScale(data_type_v3, type_scale).value_or(0);
}

uint16_t ChargewireResultSetMetadata::GetDataType(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  const SqlDataType conciseType = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);
  return GetNonConciseDataType(conciseType);
}

driver::odbcabstraction::Nullability
ChargewireResultSetMetadata::IsNullable(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  return field->nullable() ? odbcabstraction::NULLABILITY_NULLABLE : odbcabstraction::NULLABILITY_NO_NULLS;
}

std::string ChargewireResultSetMetadata::GetSchemaName(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));

  return metadata.GetSchemaName().ValueOrElse([] { return ""; });
}

std::string ChargewireResultSetMetadata::GetCatalogName(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));

  return metadata.GetCatalogName().ValueOrElse([] { return ""; });
}

std::string ChargewireResultSetMetadata::GetTableName(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));

  return metadata.GetTableName().ValueOrElse([] { return ""; });
}

std::string ChargewireResultSetMetadata::GetColumnLabel(int column_position) {
  return schema_->field(column_position - 1)->name();
}

size_t ChargewireResultSetMetadata::GetColumnDisplaySize(
    int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);

  int32_t column_size = metadata_settings_.string_column_length_.value_or(GetFieldPrecision(field).ValueOr(DefaultLengthForVariableLengthColumns));
  SqlDataType data_type_v3 = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);

  return GetDisplaySize(data_type_v3, column_size).value_or(NO_TOTAL);
}

std::string ChargewireResultSetMetadata::GetBaseColumnName(int column_position) {
  return schema_->field(column_position - 1)->name();
}

std::string ChargewireResultSetMetadata::GetBaseTableName(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));
  return metadata.GetTableName().ValueOrElse([] { return ""; });
}

uint16_t ChargewireResultSetMetadata::GetConciseType(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position -1);

  const SqlDataType sqlColumnType = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);
  return sqlColumnType;
}

size_t ChargewireResultSetMetadata::GetLength(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);

  int32_t column_size = metadata_settings_.string_column_length_.value_or(GetFieldPrecision(field).ValueOr(DefaultLengthForVariableLengthColumns));
  SqlDataType data_type_v3 = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);

  return chargewire_odbc::GetLength(data_type_v3, column_size).value_or(DefaultLengthForVariableLengthColumns);
}

std::string ChargewireResultSetMetadata::GetLiteralPrefix(int column_position) {
  // TODO: Flight SQL column metadata does not have this, should we add to the spec?
  return "";
}

std::string ChargewireResultSetMetadata::GetLiteralSuffix(int column_position) {
  // TODO: Flight SQL column metadata does not have this, should we add to the spec?
  return "";
}

std::string ChargewireResultSetMetadata::GetLocalTypeName(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  ColumnMetadata metadata = GetMetadata(field);

  return metadata.GetTypeName().ValueOrElse([&] {
    SqlDataType data_type = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);
    return GetTypeNameFromSqlDataType(static_cast<int16_t>(data_type));
  });
}

size_t ChargewireResultSetMetadata::GetNumPrecRadix(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  SqlDataType data_type_v3 = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);

  return GetRadixFromSqlDataType(data_type_v3).value_or(NO_TOTAL);
}

size_t ChargewireResultSetMetadata::GetOctetLength(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  ColumnMetadata metadata = GetMetadata(field);

  int32_t column_size = metadata_settings_.string_column_length_.value_or(GetFieldPrecision(field).ValueOr(DefaultLengthForVariableLengthColumns));
  SqlDataType data_type_v3 = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);

  // Workaround to get the precision for Decimal and Numeric types, since server doesn't return it currently.
  // TODO: Use the server precision when its fixed.
  std::shared_ptr<DataType> arrow_type = field->type();
  if (arrow_type->id() == arrow::Type::DECIMAL128){
    int32_t precision = GetDecimalTypePrecision(arrow_type);
    return GetCharOctetLength(data_type_v3, column_size, precision).value_or(DefaultDecimalPrecision+2);
  }

  return GetCharOctetLength(data_type_v3, column_size).value_or(DefaultLengthForVariableLengthColumns);
}

std::string ChargewireResultSetMetadata::GetTypeName(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);
  ColumnMetadata metadata = GetMetadata(field);

  return metadata.GetTypeName().ValueOrElse([&] {
    // Fall back to computing the type name from the Arrow field's SqlDataType
    // when the server does not provide ARROW:FLIGHT:SQL:TYPE_NAME metadata.
    // Power BI matches this value against SQLGetTypeInfo's TYPE_NAME column
    // to determine column searchability for DirectQuery folding.
    SqlDataType data_type = GetDataTypeFromArrowField_V3(field, metadata_settings_.use_wide_char_);
    return GetTypeNameFromSqlDataType(static_cast<int16_t>(data_type));
  });
}

driver::odbcabstraction::Updatability
ChargewireResultSetMetadata::GetUpdatable(int column_position) {
  return odbcabstraction::UPDATABILITY_READWRITE_UNKNOWN;
}

bool ChargewireResultSetMetadata::IsAutoUnique(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));

  // TODO: Is AutoUnique equivalent to AutoIncrement?
  return metadata.GetIsAutoIncrement().ValueOrElse([] { return false; });
}

bool ChargewireResultSetMetadata::IsCaseSensitive(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));

  return GetMetadataProperty(metadata, ColumnMetadata::kIsCaseSensitive).ValueOrElse([] { return false; });
}

driver::odbcabstraction::Searchability
ChargewireResultSetMetadata::IsSearchable(int column_position) {
  ColumnMetadata metadata = GetMetadata(schema_->field(column_position - 1));

  // DuckDB supports WHERE, GROUP BY, and ORDER BY on all column types.
  // Flight SQL column metadata may omit kIsSearchable (defaults to false),
  // which causes Power Query to reject columns for query folding.
  return odbcabstraction::SEARCHABILITY_ALL;
}

bool ChargewireResultSetMetadata::IsUnsigned(int column_position) {
  const std::shared_ptr<Field> &field = schema_->field(column_position - 1);

  switch (field->type()->id()) {
    case arrow::Type::UINT8:
    case arrow::Type::UINT16:
    case arrow::Type::UINT32:
    case arrow::Type::UINT64:
      return true;
    default:
      return false;
  }
}

bool ChargewireResultSetMetadata::IsFixedPrecScale(int column_position) {
  // TODO: Flight SQL column metadata does not have this, should we add to the spec?
  return false;
}

ChargewireResultSetMetadata::ChargewireResultSetMetadata(
    std::shared_ptr<arrow::Schema> schema,
    const odbcabstraction::MetadataSettings& metadata_settings)
    :
    metadata_settings_(metadata_settings),
    schema_(std::move(schema)) {}

ChargewireResultSetMetadata::ChargewireResultSetMetadata(
    const std::shared_ptr<ResultInfo> &flight_info,
    const odbcabstraction::MetadataSettings& metadata_settings)
    :
    metadata_settings_(metadata_settings){
  auto schema_result = flight_info->GetSchema(nullptr);
  ThrowIfNotOK(schema_result.status());
  schema_ = std::move(schema_result).ValueUnsafe();
}

namespace {
// Fractional-second digits of a timestamp or time parameter, or -1.
int FractionalDigits(const arrow::DataType& type) {
  arrow::TimeUnit::type unit;
  if (type.id() == arrow::Type::TIMESTAMP) {
    unit = static_cast<const arrow::TimestampType&>(type).unit();
  } else if (type.id() == arrow::Type::TIME32 || type.id() == arrow::Type::TIME64) {
    unit = static_cast<const arrow::TimeType&>(type).unit();
  } else {
    return -1;
  }
  switch (unit) {
  case arrow::TimeUnit::SECOND: return 0;
  case arrow::TimeUnit::MILLI: return 3;
  case arrow::TimeUnit::MICRO: return 6;
  default: return 9;
  }
}
} // namespace

ChargewireParameterMetadata::ChargewireParameterMetadata(
    std::shared_ptr<arrow::Schema> schema, const odbcabstraction::MetadataSettings& metadata_settings)
    : ChargewireResultSetMetadata(schema, metadata_settings), parameter_schema_(std::move(schema)) {}

size_t ChargewireParameterMetadata::GetLength(int column_position) {
  const auto& type = *parameter_schema_->field(column_position - 1)->type();
  if (arrow::is_decimal(type.id())) {
    return static_cast<const arrow::DecimalType&>(type).precision();
  }
  const int digits = FractionalDigits(type);
  if (digits >= 0) {
    // ODBC column sizes: "yyyy-mm-dd hh:mm:ss[.f...]" and "hh:mm:ss[.f...]".
    const size_t whole = type.id() == arrow::Type::TIMESTAMP ? 19 : 8;
    return digits ? whole + 1 + digits : whole;
  }
  return ChargewireResultSetMetadata::GetLength(column_position);
}

size_t ChargewireParameterMetadata::GetPrecision(int column_position) {
  const auto& type = *parameter_schema_->field(column_position - 1)->type();
  if (arrow::is_decimal(type.id())) {
    return static_cast<const arrow::DecimalType&>(type).precision();
  }
  const int digits = FractionalDigits(type);
  return digits >= 0 ? digits : ChargewireResultSetMetadata::GetPrecision(column_position);
}

size_t ChargewireParameterMetadata::GetScale(int column_position) {
  const auto& type = *parameter_schema_->field(column_position - 1)->type();
  if (arrow::is_decimal(type.id())) {
    return std::max(0, static_cast<const arrow::DecimalType&>(type).scale());
  }
  const int digits = FractionalDigits(type);
  return digits >= 0 ? digits : ChargewireResultSetMetadata::GetScale(column_position);
}

} // namespace chargewire
} // namespace driver