/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "parameter_batch.h"

#include <arrow/array/builder_base.h>
#include <arrow/builder.h>
#include <arrow/util/decimal.h>
#include <odbcabstraction/encoding.h>
#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>
#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace driver {
namespace chargewire_odbc {

using odbcabstraction::DriverException;
using odbcabstraction::ParameterBinding;
using odbcabstraction::ParameterRowException;
using odbcabstraction::ParameterSet;

namespace {

// Values may sit at any alignment in a row-wise bound struct.
template <typename T>
T Load(const void* data) {
  T value;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

[[noreturn]] void Refuse(const std::string& message, const std::string& state) {
  throw DriverException(message, state);
}

[[noreturn]] void RefuseCType(const ParameterBinding& b, const std::string& target) {
  Refuse("cannot convert C type " + std::to_string(b.c_type) + " to " + target, "07006");
}

void Check(const arrow::Status& status, const char* state = "HY000") {
  if (!status.ok()) Refuse(status.message(), state);
}

// The UTF-8 text of a SQL_C_CHAR or SQL_C_WCHAR value (length already resolved).
std::string Text(const ParameterBinding& b) {
  if (b.length <= 0) return std::string();
  if (b.c_type == SQL_C_CHAR) return std::string(static_cast<const char*>(b.data_ptr), static_cast<size_t>(b.length));
  const size_t unit = odbcabstraction::GetSqlWCharSize();
  // Copy to an aligned buffer: a row-wise struct may place the characters anywhere.
  std::vector<uint32_t> aligned((static_cast<size_t>(b.length) + sizeof(uint32_t) - 1) / sizeof(uint32_t));
  std::memcpy(aligned.data(), b.data_ptr, static_cast<size_t>(b.length));
  std::vector<uint8_t> utf8;
  try {
    odbcabstraction::WcsToUtf8(aligned.data(), static_cast<size_t>(b.length) / unit, &utf8);
  } catch (const std::exception&) {
    Refuse("wide-character value is not valid UTF-16/UTF-32", "22018");
  }
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

bool IsText(const ParameterBinding& b) { return b.c_type == SQL_C_CHAR || b.c_type == SQL_C_WCHAR; }

std::string Trimmed(const std::string& s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return std::string();
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

arrow::Decimal128 NumericStructValue(const ParameterBinding& b, int32_t* scale) {
  const auto numeric = Load<SQL_NUMERIC_STRUCT>(b.data_ptr);
  uint64_t low = 0, high = 0;
  for (int i = 0; i < 8; ++i) low |= static_cast<uint64_t>(numeric.val[i]) << (8 * i);
  for (int i = 8; i < 16; ++i) high |= static_cast<uint64_t>(numeric.val[i]) << (8 * (i - 8));
  if (high >> 63) Refuse("SQL_NUMERIC_STRUCT value is out of range", "22003");
  arrow::Decimal128 value(static_cast<int64_t>(high), low);
  if (numeric.sign == 0) value.Negate();
  *scale = static_cast<SQLSCHAR>(numeric.scale);
  return value;
}

// The value as a decimal at exactly the given scale, or a refusal if that loses digits.
arrow::Decimal128 DecimalValue(const ParameterBinding& b, int32_t precision, int32_t scale);

int64_t IntegerValue(const ParameterBinding& b) {
  switch (b.c_type) {
  case SQL_C_BIT: return Load<uint8_t>(b.data_ptr) != 0;
  case SQL_C_TINYINT:
  case SQL_C_STINYINT: return Load<int8_t>(b.data_ptr);
  case SQL_C_UTINYINT: return Load<uint8_t>(b.data_ptr);
  case SQL_C_SHORT:
  case SQL_C_SSHORT: return Load<int16_t>(b.data_ptr);
  case SQL_C_USHORT: return Load<uint16_t>(b.data_ptr);
  case SQL_C_LONG:
  case SQL_C_SLONG: return Load<int32_t>(b.data_ptr);
  case SQL_C_ULONG: return Load<uint32_t>(b.data_ptr);
  case SQL_C_SBIGINT: return Load<int64_t>(b.data_ptr);
  case SQL_C_UBIGINT: {
    const auto value = Load<uint64_t>(b.data_ptr);
    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) Refuse("integer value is out of range", "22003");
    return static_cast<int64_t>(value);
  }
  case SQL_C_DOUBLE:
  case SQL_C_FLOAT: {
    const double value = b.c_type == SQL_C_DOUBLE ? Load<double>(b.data_ptr) : Load<float>(b.data_ptr);
    if (!std::isfinite(value) || value != std::trunc(value) || value < -9223372036854775808.0 ||
        value >= 9223372036854775808.0) {
      Refuse("floating-point value is not an integer in range", "22003");
    }
    return static_cast<int64_t>(value);
  }
  case SQL_C_CHAR:
  case SQL_C_WCHAR:
  case SQL_C_NUMERIC: {
    if (IsText(b)) {
      const std::string text = Trimmed(Text(b));
      if (text.empty()) Refuse("empty string is not an integer", "22018");
      errno = 0;
      char* end = nullptr;
      const long long value = std::strtoll(text.c_str(), &end, 10);
      if (end == text.c_str() + text.size()) {
        if (errno == ERANGE) Refuse("integer value is out of range", "22003");
        return value;
      }
    }
    // A decimal value is accepted when it is a whole number ("5.0").
    const arrow::Decimal128 whole = DecimalValue(b, 38, 0);
    if (whole > arrow::Decimal128(std::numeric_limits<int64_t>::max()) ||
        whole < arrow::Decimal128(std::numeric_limits<int64_t>::min())) {
      Refuse("integer value is out of range", "22003");
    }
    return static_cast<int64_t>(whole.low_bits());
  }
  default: RefuseCType(b, "an integer");
  }
}

arrow::Decimal128 DecimalValue(const ParameterBinding& b, int32_t precision, int32_t scale) {
  arrow::Decimal128 value;
  int32_t value_scale = 0;
  switch (b.c_type) {
  case SQL_C_NUMERIC: value = NumericStructValue(b, &value_scale); break;
  case SQL_C_CHAR:
  case SQL_C_WCHAR: {
    int32_t ignored_precision = 0;
    const std::string text = Trimmed(Text(b));
    if (!arrow::Decimal128::FromString(text, &value, &ignored_precision, &value_scale).ok()) {
      Refuse("'" + text + "' is not a decimal number", "22018");
    }
    break;
  }
  case SQL_C_DOUBLE:
  case SQL_C_FLOAT: {
    const double real = b.c_type == SQL_C_DOUBLE ? Load<double>(b.data_ptr) : Load<float>(b.data_ptr);
    auto converted = arrow::Decimal128::FromReal(real, precision, scale);
    if (!converted.ok()) Refuse("floating-point value does not fit DECIMAL(" + std::to_string(precision) + "," +
                                std::to_string(scale) + ")", "22003");
    return *converted;
  }
  case SQL_C_UBIGINT: value = arrow::Decimal128(0, Load<uint64_t>(b.data_ptr)); break;
  default: value = arrow::Decimal128(IntegerValue(b)); break;
  }
  auto rescaled = value.Rescale(value_scale, scale);
  if (!rescaled.ok()) {
    Refuse("decimal value " + value.ToString(value_scale) + " does not fit scale " + std::to_string(scale), "22003");
  }
  if (!rescaled->FitsInPrecision(precision)) {
    Refuse("decimal value " + value.ToString(value_scale) + " does not fit DECIMAL(" + std::to_string(precision) + "," +
           std::to_string(scale) + ")", "22003");
  }
  return *rescaled;
}

double DoubleValue(const ParameterBinding& b) {
  switch (b.c_type) {
  case SQL_C_DOUBLE: return Load<double>(b.data_ptr);
  case SQL_C_FLOAT: return Load<float>(b.data_ptr);
  case SQL_C_UBIGINT: return static_cast<double>(Load<uint64_t>(b.data_ptr));
  case SQL_C_NUMERIC: {
    int32_t scale = 0;
    return NumericStructValue(b, &scale).ToDouble(scale);
  }
  case SQL_C_CHAR:
  case SQL_C_WCHAR: {
    const std::string text = Trimmed(Text(b));
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size()) Refuse("'" + text + "' is not a number", "22018");
    return value;
  }
  default: return static_cast<double>(IntegerValue(b));
  }
}

bool BoolValue(const ParameterBinding& b) {
  if (IsText(b)) {
    const std::string text = Trimmed(Text(b));
    return !text.empty() && (text[0] == '1' || text[0] == 't' || text[0] == 'T');
  }
  return IntegerValue(b) != 0;
}

// ---- Dates and times -------------------------------------------------------

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CheckDate(int64_t year, int64_t month, int64_t day) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) Refuse("invalid month " + std::to_string(month), "22007");
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  const int days = kDays[month - 1] + (month == 2 && leap ? 1 : 0);
  if (day < 1 || day > days) Refuse("invalid day " + std::to_string(day), "22007");
}

void CheckTime(int64_t hour, int64_t minute, int64_t second, int64_t fraction) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    Refuse("invalid time of day", "22007");
  }
  if (fraction < 0 || fraction > 999999999) Refuse("invalid fractional seconds", "22008");
}

struct DateTime {
  int64_t days = 0;           // since 1970-01-01
  int64_t seconds = 0;        // of the day
  int64_t nanoseconds = 0;    // of the second
};

// Parses "[YYYY-MM-DD][ |T][HH:MM:SS[.fffffffff]]" (date, time or both).
DateTime ParseDateTime(const std::string& raw, bool want_date, bool want_time) {
  const std::string text = Trimmed(raw);
  DateTime result;
  size_t pos = 0;
  auto number = [&](int max_digits) -> int64_t {
    size_t start = pos;
    int64_t value = 0;
    if (pos < text.size() && text[pos] == '-' && start == 0 && want_date) ++pos;  // negative years
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) &&
           static_cast<int>(pos - start) < max_digits) {
      value = value * 10 + (text[pos++] - '0');
    }
    if (pos == start || (pos == start + 1 && text[start] == '-')) Refuse("cannot parse '" + text + "' as a date/time", "22007");
    return text[start] == '-' ? -value : value;
  };
  auto expect = [&](char c) {
    if (pos >= text.size() || text[pos] != c) Refuse("cannot parse '" + text + "' as a date/time", "22007");
    ++pos;
  };
  bool has_date = false;
  if (want_date && text.find('-', 1) != std::string::npos && (text.find(':') == std::string::npos ||
                                                              text.find('-', 1) < text.find(':'))) {
    const int64_t year = number(6);
    expect('-');
    const int64_t month = number(2);
    expect('-');
    const int64_t day = number(2);
    CheckDate(year, month, day);
    result.days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    has_date = true;
    if (pos < text.size() && (text[pos] == ' ' || text[pos] == 'T')) ++pos;
  }
  if (pos < text.size() && want_time) {
    const int64_t hour = number(2);
    expect(':');
    const int64_t minute = number(2);
    expect(':');
    const int64_t second = number(2);
    int64_t fraction = 0;
    if (pos < text.size() && text[pos] == '.') {
      ++pos;
      int digits = 0;
      while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        if (digits < 9) fraction = fraction * 10 + (text[pos] - '0');
        ++digits;
        ++pos;
      }
      if (digits == 0) Refuse("cannot parse '" + text + "' as a date/time", "22007");
      for (; digits < 9; ++digits) fraction *= 10;
    }
    CheckTime(hour, minute, second, fraction);
    result.seconds = hour * 3600 + minute * 60 + second;
    result.nanoseconds = fraction;
  } else if (!has_date) {
    Refuse("cannot parse '" + text + "' as a date/time", "22007");
  }
  if (pos < text.size() && text[pos] == 'Z') ++pos;
  if (pos != text.size()) Refuse("cannot parse '" + text + "' as a date/time (unexpected '" + text.substr(pos) + "')", "22007");
  return result;
}

DateTime DateTimeValue(const ParameterBinding& b, bool want_date, bool want_time) {
  DateTime result;
  switch (b.c_type) {
  case SQL_C_TYPE_DATE:
  case SQL_C_DATE: {
    const auto d = Load<SQL_DATE_STRUCT>(b.data_ptr);
    CheckDate(d.year, d.month, d.day);
    result.days = DaysFromCivil(d.year, d.month, d.day);
    return result;
  }
  case SQL_C_TYPE_TIME:
  case SQL_C_TIME: {
    const auto t = Load<SQL_TIME_STRUCT>(b.data_ptr);
    CheckTime(t.hour, t.minute, t.second, 0);
    result.seconds = t.hour * 3600 + t.minute * 60 + t.second;
    return result;
  }
  case SQL_C_TYPE_TIMESTAMP:
  case SQL_C_TIMESTAMP: {
    const auto ts = Load<SQL_TIMESTAMP_STRUCT>(b.data_ptr);
    CheckDate(ts.year, ts.month, ts.day);
    CheckTime(ts.hour, ts.minute, ts.second, ts.fraction);
    result.days = DaysFromCivil(ts.year, ts.month, ts.day);
    result.seconds = ts.hour * 3600 + ts.minute * 60 + ts.second;
    result.nanoseconds = ts.fraction;
    return result;
  }
  case SQL_C_CHAR:
  case SQL_C_WCHAR:
    return ParseDateTime(Text(b), want_date, want_time);
  default: RefuseCType(b, want_date ? "a date/timestamp" : "a time");
  }
}

int64_t UnitsPerSecond(arrow::TimeUnit::type unit) {
  switch (unit) {
  case arrow::TimeUnit::SECOND: return 1;
  case arrow::TimeUnit::MILLI: return 1000;
  case arrow::TimeUnit::MICRO: return 1000000;
  default: return 1000000000;
  }
}

int64_t ToUnits(int64_t seconds, int64_t nanoseconds, arrow::TimeUnit::type unit) {
  const int64_t per_second = UnitsPerSecond(unit);
  if (seconds > std::numeric_limits<int64_t>::max() / per_second - 1 ||
      seconds < std::numeric_limits<int64_t>::min() / per_second + 1) {
    Refuse("date/time value is out of range for the parameter's type", "22008");
  }
  return seconds * per_second + nanoseconds / (1000000000 / per_second);
}

// ---- Text rendering for string parameters ------------------------------------

std::string GuidText(const ParameterBinding& b) {
  const auto g = Load<SQLGUID>(b.data_ptr);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned>(g.Data1), static_cast<unsigned>(g.Data2), static_cast<unsigned>(g.Data3),
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

std::string StringValue(const ParameterBinding& b) {
  char buf[64];
  switch (b.c_type) {
  case SQL_C_CHAR:
  case SQL_C_WCHAR: return Text(b);
  case SQL_C_BINARY: return std::string(static_cast<const char*>(b.data_ptr), static_cast<size_t>(b.length));
  case SQL_C_GUID: return GuidText(b);
  case SQL_C_DOUBLE: std::snprintf(buf, sizeof(buf), "%.17g", Load<double>(b.data_ptr)); return buf;
  case SQL_C_FLOAT: std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(Load<float>(b.data_ptr))); return buf;
  case SQL_C_UBIGINT: return std::to_string(Load<uint64_t>(b.data_ptr));
  case SQL_C_NUMERIC: {
    int32_t scale = 0;
    return NumericStructValue(b, &scale).ToString(scale);
  }
  case SQL_C_TYPE_DATE:
  case SQL_C_DATE: {
    const auto d = Load<SQL_DATE_STRUCT>(b.data_ptr);
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", d.year, d.month, d.day);
    return buf;
  }
  case SQL_C_TYPE_TIME:
  case SQL_C_TIME: {
    const auto t = Load<SQL_TIME_STRUCT>(b.data_ptr);
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", t.hour, t.minute, t.second);
    return buf;
  }
  case SQL_C_TYPE_TIMESTAMP:
  case SQL_C_TIMESTAMP: {
    const auto ts = Load<SQL_TIMESTAMP_STRUCT>(b.data_ptr);
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02u:%02u:%02u.%09u", ts.year, ts.month, ts.day, ts.hour,
                  ts.minute, ts.second, static_cast<unsigned>(ts.fraction));
    return buf;
  }
  default: return std::to_string(IntegerValue(b));
  }
}

// ---- Appending one value -----------------------------------------------------

template <typename BuilderType, typename T>
void AppendInteger(arrow::ArrayBuilder* builder, const ParameterBinding& b) {
  const int64_t value = IntegerValue(b);
  if (value < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
      (value > 0 && static_cast<uint64_t>(value) > static_cast<uint64_t>(std::numeric_limits<T>::max()))) {
    Refuse("integer value " + std::to_string(value) + " is out of range for the parameter's type", "22003");
  }
  Check(static_cast<BuilderType*>(builder)->Append(static_cast<T>(value)));
}

void AppendValue(arrow::ArrayBuilder* builder, const arrow::DataType& type, const ParameterBinding& b) {
  if (b.is_null) {
    Check(builder->AppendNull());
    return;
  }
  switch (type.id()) {
  case arrow::Type::STRING: {
    const std::string value = StringValue(b);
    Check(static_cast<arrow::StringBuilder*>(builder)->Append(value));
    return;
  }
  case arrow::Type::BINARY: {
    if (b.c_type != SQL_C_BINARY && !IsText(b)) RefuseCType(b, "binary");
    const std::string value = b.c_type == SQL_C_WCHAR ? Text(b)
        : std::string(static_cast<const char*>(b.data_ptr), static_cast<size_t>(b.length));
    Check(static_cast<arrow::BinaryBuilder*>(builder)->Append(value));
    return;
  }
  case arrow::BooleanType::type_id:  // arrow::Type::BOOL; BOOL is an ODBC macro
    Check(static_cast<arrow::BooleanBuilder*>(builder)->Append(BoolValue(b))); return;
  case arrow::Type::INT8: AppendInteger<arrow::Int8Builder, int8_t>(builder, b); return;
  case arrow::Type::INT16: AppendInteger<arrow::Int16Builder, int16_t>(builder, b); return;
  case arrow::Type::INT32: AppendInteger<arrow::Int32Builder, int32_t>(builder, b); return;
  case arrow::Type::INT64: AppendInteger<arrow::Int64Builder, int64_t>(builder, b); return;
  case arrow::Type::UINT8: AppendInteger<arrow::UInt8Builder, uint8_t>(builder, b); return;
  case arrow::Type::UINT16: AppendInteger<arrow::UInt16Builder, uint16_t>(builder, b); return;
  case arrow::Type::UINT32: AppendInteger<arrow::UInt32Builder, uint32_t>(builder, b); return;
  case arrow::Type::UINT64: {
    if (b.c_type == SQL_C_UBIGINT) {
      Check(static_cast<arrow::UInt64Builder*>(builder)->Append(Load<uint64_t>(b.data_ptr)));
      return;
    }
    const int64_t value = IntegerValue(b);
    if (value < 0) Refuse("integer value " + std::to_string(value) + " is out of range for the parameter's type", "22003");
    Check(static_cast<arrow::UInt64Builder*>(builder)->Append(static_cast<uint64_t>(value)));
    return;
  }
  case arrow::Type::FLOAT:
    Check(static_cast<arrow::FloatBuilder*>(builder)->Append(static_cast<float>(DoubleValue(b))));
    return;
  case arrow::Type::DOUBLE: Check(static_cast<arrow::DoubleBuilder*>(builder)->Append(DoubleValue(b))); return;
  case arrow::Type::DECIMAL128: {
    const auto& decimal = static_cast<const arrow::Decimal128Type&>(type);
    Check(static_cast<arrow::Decimal128Builder*>(builder)->Append(
        DecimalValue(b, decimal.precision(), decimal.scale())));
    return;
  }
  case arrow::Type::DATE32: {
    if (b.c_type == SQL_C_TYPE_TIME || b.c_type == SQL_C_TIME) RefuseCType(b, "a date");
    const DateTime value = DateTimeValue(b, true, true);
    if (value.seconds != 0 || value.nanoseconds != 0) Refuse("the time of day would be lost converting to a date", "22008");
    const int64_t days = value.days;
    if (days < std::numeric_limits<int32_t>::min() || days > std::numeric_limits<int32_t>::max()) {
      Refuse("date is out of range", "22008");
    }
    Check(static_cast<arrow::Date32Builder*>(builder)->Append(static_cast<int32_t>(days)));
    return;
  }
  case arrow::Type::TIMESTAMP: {
    const auto& timestamp = static_cast<const arrow::TimestampType&>(type);
    if (b.c_type == SQL_C_TYPE_TIME || b.c_type == SQL_C_TIME) RefuseCType(b, "a timestamp");
    const DateTime value = DateTimeValue(b, true, true);
    if (value.days > std::numeric_limits<int64_t>::max() / 86400 ||
        value.days < std::numeric_limits<int64_t>::min() / 86400) {
      Refuse("timestamp is out of range", "22008");
    }
    Check(static_cast<arrow::TimestampBuilder*>(builder)->Append(
        ToUnits(value.days * 86400 + value.seconds, value.nanoseconds, timestamp.unit())));
    return;
  }
  case arrow::Type::TIME32:
  case arrow::Type::TIME64: {
    const auto& time = static_cast<const arrow::TimeType&>(type);
    if (b.c_type == SQL_C_TYPE_DATE || b.c_type == SQL_C_DATE) RefuseCType(b, "a time");
    const DateTime value = DateTimeValue(b, false, true);
    const int64_t units = ToUnits(value.seconds, value.nanoseconds, time.unit());
    if (type.id() == arrow::Type::TIME32) {
      Check(static_cast<arrow::Time32Builder*>(builder)->Append(static_cast<int32_t>(units)));
    } else {
      Check(static_cast<arrow::Time64Builder*>(builder)->Append(units));
    }
    return;
  }
  default:
    Refuse("unsupported Arrow parameter type: " + type.ToString(), "HYC00");
  }
}

// Chargewire describes an unconstrained placeholder (SELECT ?) as Arrow null.
// Its type then comes from the C type that SQLBindParameter declared.
std::shared_ptr<arrow::DataType> InferredType(const ParameterSet& parameters, size_t column) {
  const ParameterBinding& first = parameters.at(0, column);
  switch (first.c_type) {
  case SQL_C_CHAR: case SQL_C_WCHAR: case SQL_C_GUID: return arrow::utf8();
  case SQL_C_BIT: return arrow::boolean();
  case SQL_C_TINYINT: case SQL_C_STINYINT: return arrow::int8();
  case SQL_C_UTINYINT: return arrow::uint8();
  case SQL_C_SHORT: case SQL_C_SSHORT: return arrow::int16();
  case SQL_C_USHORT: return arrow::uint16();
  case SQL_C_LONG: case SQL_C_SLONG: return arrow::int32();
  case SQL_C_ULONG: return arrow::uint32();
  case SQL_C_SBIGINT: return arrow::int64();
  case SQL_C_UBIGINT: return arrow::uint64();
  case SQL_C_FLOAT: return arrow::float32();
  case SQL_C_DOUBLE: return arrow::float64();
  case SQL_C_BINARY: return arrow::binary();
  case SQL_C_TYPE_DATE: case SQL_C_DATE: return arrow::date32();
  case SQL_C_TYPE_TIME: case SQL_C_TIME: return arrow::time64(arrow::TimeUnit::MICRO);
  case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: return arrow::timestamp(arrow::TimeUnit::MICRO);
  case SQL_C_NUMERIC: {
    const int scale = std::max(0, std::min<int>(first.decimal_digits, 38));
    return arrow::decimal128(38, scale);
  }
  default:
    return nullptr;
  }
}

} // namespace

std::shared_ptr<arrow::RecordBatch> BuildParameterBatch(const std::shared_ptr<arrow::Schema>& param_schema,
                                                        const ParameterSet& parameters) {
  if (static_cast<int>(parameters.column_count) != param_schema->num_fields()) {
    throw DriverException("Parameter count mismatch: expected " + std::to_string(param_schema->num_fields()) +
                              " but got " + std::to_string(parameters.column_count),
                          "07002");
  }
  if (parameters.row_count == 0 || parameters.cells.size() != parameters.row_count * parameters.column_count) {
    throw DriverException("No parameter sets to send", "HY000");
  }

  auto fields = param_schema->fields();
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  arrays.reserve(fields.size());
  for (size_t column = 0; column < fields.size(); ++column) {
    auto type = fields[column]->type();
    if (type->id() == arrow::Type::NA) {
      type = InferredType(parameters, column);
      if (!type) {
        bool all_null = true;
        for (size_t row = 0; row < parameters.row_count; ++row) all_null = all_null && parameters.at(row, column).is_null;
        if (!all_null) {
          throw ParameterRowException(parameters.source_rows[0],
              "Parameter " + std::to_string(column + 1) + ": cannot infer an untyped parameter from C type " +
                  std::to_string(parameters.at(0, column).c_type), "07006");
        }
        type = arrow::utf8();
      }
      fields[column] = fields[column]->WithType(type);
    }

    std::unique_ptr<arrow::ArrayBuilder> builder;
    Check(arrow::MakeBuilder(arrow::default_memory_pool(), type, &builder));
    Check(builder->Reserve(static_cast<int64_t>(parameters.row_count)));
    for (size_t row = 0; row < parameters.row_count; ++row) {
      try {
        AppendValue(builder.get(), *type, parameters.at(row, column));
      } catch (const ParameterRowException&) {
        throw;
      } catch (const DriverException& error) {
        throw ParameterRowException(parameters.source_rows[row],
            "Parameter set " + std::to_string(parameters.source_rows[row] + 1) + ", parameter " +
                std::to_string(column + 1) + ": " + error.GetMessageText(), error.GetSqlState());
      }
    }
    std::shared_ptr<arrow::Array> array;
    Check(builder->Finish(&array));
    arrays.push_back(std::move(array));
  }
  return arrow::RecordBatch::Make(arrow::schema(fields, param_schema->metadata()),
                                  static_cast<int64_t>(parameters.row_count), std::move(arrays));
}

} // namespace chargewire_odbc
} // namespace driver
