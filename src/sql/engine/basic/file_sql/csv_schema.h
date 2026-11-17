/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "file_source.h"
#include <charconv>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <limits>

namespace oceanbase::sql::file_sql {
enum class ScalarType { Null, Bigint, Double, Boolean, Varchar, Date, Datetime };

inline bool valid_date(const std::string &s)
{
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) { return false; }
  }
  const int year = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
  const int month = (s[5]-'0')*10+s[6]-'0';
  const int day = (s[8]-'0')*10+s[9]-'0';
  if (year == 0 || month < 1 || month > 12 || day < 1) { return false; }
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day <= days[month-1] + (month == 2 && leap ? 1 : 0);
}

inline bool valid_datetime(const std::string &s)
{
  if (s.size() < 19 || s.size() > 26 || !valid_date(s.substr(0,10)) || (s[10] != ' ' && s[10] != 'T')
      || s[13] != ':' || s[16] != ':') { return false; }
  for (std::size_t i : {11u,12u,14u,15u,17u,18u}) {
    if (s[i] < '0' || s[i] > '9') { return false; }
  }
  if (s.size() > 19) {
    if (s.size() == 20 || s[19] != '.') { return false; }
    for (std::size_t i = 20; i < s.size(); ++i) { if (s[i] < '0' || s[i] > '9') { return false; } }
  }
  return (s[11]-'0')*10+s[12]-'0' < 24 && (s[14]-'0')*10+s[15]-'0' < 60
      && (s[17]-'0')*10+s[18]-'0' < 60;
}

inline int boolean_text(const std::string &text)
{
  if (text.size() != 4 && text.size() != 5) { return -1; }
  std::string lower = text;
  for (char &ch : lower) { if (ch >= 'A' && ch <= 'Z') { ch += 'a'-'A'; } }
  return lower == "true" ? 1 : lower == "false" ? 0 : -1;
}
inline bool bigint_value(const std::string &text, std::int64_t &value)
{
  const char *start = text.data();
  const char *end = start+text.size();
  if (start != end && *start == '+') { ++start; if (start != end && (*start == '-' || *start == '+')) { return false; } }
  if (start == end) { return false; }
  const auto parsed = std::from_chars(start,end,value);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}
inline int datetime_scale(const CsvField &field)
{
  return valid_datetime(field.text) && field.text.size() > 20 ? static_cast<int>(field.text.size()-20) : 0;
}

inline ScalarType infer_scalar(const CsvField &field)
{
  const auto &s = field.text;
  if (!field.quoted && s.empty()) { return ScalarType::Null; }
  if (boolean_text(s) >= 0) { return ScalarType::Boolean; }
  std::int64_t integer = 0;
  const char *start = s.data();
  if (!s.empty() && s[0] == '+') {
    ++start;
    if (s.size() > 1 && (s[1] == '-' || s[1] == '+')) { return ScalarType::Varchar; }
  }
  const auto parsed = std::from_chars(start, s.data()+s.size(), integer);
  if (parsed.ec == std::errc{} && parsed.ptr == s.data()+s.size()) { return ScalarType::Bigint; }
  if (valid_date(s)) { return ScalarType::Date; }
  if (valid_datetime(s)) { return ScalarType::Datetime; }
  // Integer overflow must retain its exact text rather than becoming a lossy double.
  if (parsed.ec == std::errc::result_out_of_range && parsed.ptr == s.data()+s.size()) {
    return ScalarType::Varchar;
  }
  if (!s.empty() && s.find_first_not_of("0123456789eE+.-") == std::string::npos) {
    double value = 0;
    const auto number = std::from_chars(start,s.data()+s.size(),value,std::chars_format::general);
    if (number.ptr == s.data()+s.size() && number.ec == std::errc{} && std::isfinite(value)) {
      return ScalarType::Double;
    }
  }
  return ScalarType::Varchar;
}

inline ScalarType widen(ScalarType a, ScalarType b)
{
  if (a == b || b == ScalarType::Null) { return a; }
  if (a == ScalarType::Null) { return b; }
  if ((a == ScalarType::Bigint && b == ScalarType::Double)
      || (b == ScalarType::Bigint && a == ScalarType::Double)) { return ScalarType::Double; }
  if ((a == ScalarType::Date && b == ScalarType::Datetime)
      || (b == ScalarType::Date && a == ScalarType::Datetime)) { return ScalarType::Datetime; }
  return ScalarType::Varchar;
}

struct CsvSchema {
  std::vector<std::string> names;
  std::vector<ScalarType> types;
  std::vector<bool> nullable;
  std::vector<int> temporal_scales;
  std::size_t rows = 0;
};

inline void csv_null_markers(std::vector<CsvField> &row, const std::string &null_text)
{
  if (!null_text.empty()) {
    for (auto &field : row) { if (!field.quoted && field.text == null_text) { field.text.clear(); } }
  }
}

inline CsvSchema infer_csv(std::istream &input, bool header = true, char delimiter = ',',
                           char quote = '"', char escape = '"', const std::string &null_text = "")
{
  CsvSchema schema;
  CsvReader reader(input, delimiter, 16*1024*1024, quote, escape);
  std::vector<CsvField> row;
  if (!reader.next(row)) { return schema; }
  if (row.size() > 4096) { throw std::runtime_error("CSV column limit exceeded"); }
  schema.types.resize(row.size(), ScalarType::Null);
  schema.nullable.resize(row.size(), false);
  schema.temporal_scales.resize(row.size(),0);
  if (header) {
    schema.names = csv_column_names(row);
  } else {
    csv_null_markers(row,null_text);
    for (std::size_t i = 0; i < row.size(); ++i) {
      schema.names.push_back("column"+std::to_string(i+1));
      schema.types[i] = infer_scalar(row[i]);
      schema.nullable[i] = schema.types[i] == ScalarType::Null;
      schema.temporal_scales[i] = datetime_scale(row[i]);
    }
    ++schema.rows;
  }
  while (reader.next(row)) {
    csv_null_markers(row,null_text);
    if (row.size() != schema.types.size()) {
      throw std::runtime_error("CSV record " + std::to_string(schema.rows + (header ? 2 : 1))
                               + ": field count differs from schema");
    }
    for (std::size_t i = 0; i < row.size(); ++i) {
      const auto type = infer_scalar(row[i]);
      schema.temporal_scales[i] = std::max(schema.temporal_scales[i],datetime_scale(row[i]));
      schema.nullable[i] = schema.nullable[i] || type == ScalarType::Null;
      schema.types[i] = widen(schema.types[i], type);
    }
    ++schema.rows;
  }
  for (std::size_t i = 0; i < schema.types.size(); ++i) {
    if (schema.types[i] == ScalarType::Null) { schema.nullable[i] = true; }
  }
  return schema;
}
} // namespace oceanbase::sql::file_sql
