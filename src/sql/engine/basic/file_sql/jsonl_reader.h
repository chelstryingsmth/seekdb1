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
#include "csv_schema.h"
#include <rapidjson/document.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <map>

namespace oceanbase::sql::file_sql {
struct JsonRecord {
  std::vector<std::string> names;
  std::vector<CsvField> fields;
  std::vector<ScalarType> types;
};

struct FlatJsonGuard : rapidjson::BaseReaderHandler<rapidjson::UTF8<>,FlatJsonGuard> {
  bool StartObject() { if (inside) { return false; } inside = true; return true; }
  bool EndObject(rapidjson::SizeType) { inside = false; return true; }
  bool StartArray() { return false; }
  bool Key(const char *,rapidjson::SizeType,bool) { return ++keys <= 4096; }
  bool inside = false;
  std::size_t keys = 0;
};

inline bool read_json_record(std::istream &input, JsonRecord &record, std::size_t *line_number = nullptr)
{
  record = JsonRecord{};
  std::string line;
  const auto fail = [&](const std::string &message) {
    throw std::runtime_error("JSONL line "+std::to_string(line_number == nullptr ? 1 : *line_number)+": "+message);
  };
  for (;;) {
    line.clear();
    if (line_number != nullptr) { ++*line_number; }
    for (;;) {
      const int ch = input.get();
      if (ch == std::char_traits<char>::eof() || ch == '\n') { break; }
      if (line.size() == 16 * 1024 * 1024) { fail("JSONL record byte limit exceeded"); }
      line.push_back(static_cast<char>(ch));
    }
    if (line_number != nullptr && *line_number == 1 && line.compare(0,3,"\xEF\xBB\xBF") == 0) { line.erase(0,3); }
    if (input.bad()) { fail("JSONL input read failure"); }
    if (line.find_first_not_of(" \t\r") != std::string::npos) { break; }
    if (input.eof()) { return false; }
  }
  rapidjson::Reader validator;
  rapidjson::MemoryStream source(line.data(),line.size());
  FlatJsonGuard guard;
  if (!validator.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(source,guard)) {
    fail("invalid or nested object at byte "+std::to_string(validator.GetErrorOffset()));
  }
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
  if (document.HasParseError() || !document.IsObject()) { fail("invalid JSONL object"); }
  std::set<std::string> seen;
  for (auto it = document.MemberBegin(); it != document.MemberEnd(); ++it) {
    if (record.names.size() == 4096) { fail("JSONL column limit exceeded"); }
    std::string name(it->name.GetString(), it->name.GetStringLength());
    if (!seen.insert(name).second) { fail("duplicate JSONL field"); }
    const auto &value = it->value;
    CsvField field;
    ScalarType type = ScalarType::Null;
    if (value.IsNull()) {
    } else if (value.IsString()) {
      field = {std::string(value.GetString(), value.GetStringLength()), true};
      type = valid_date(field.text) ? ScalarType::Date
           : valid_datetime(field.text) ? ScalarType::Datetime : ScalarType::Varchar;
    } else if (value.IsBool()) {
      field.text = value.GetBool() ? "true" : "false";
      type = ScalarType::Boolean;
    } else if (value.IsInt64()) {
      field.text = std::to_string(value.GetInt64());
      type = ScalarType::Bigint;
    } else if (value.IsUint64()) {
      field.text = std::to_string(value.GetUint64());
      type = ScalarType::Varchar;
    } else if (value.IsDouble()) {
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      value.Accept(writer);
      field.text.assign(buffer.GetString(), buffer.GetSize());
      type = ScalarType::Double;
    } else {
      fail("nested JSONL objects and arrays are unsupported");
    }
    record.names.push_back(std::move(name));
    record.fields.push_back(std::move(field));
    record.types.push_back(type);
  }
  return true;
}

inline CsvSchema infer_jsonl(std::istream &input)
{
  CsvSchema schema;
  std::map<std::string, std::size_t> columns;
  JsonRecord record;
  std::size_t line = 0;
  while (read_json_record(input, record, &line)) {
    for (std::size_t i = 0; i < record.names.size(); ++i) {
      const auto inserted = columns.emplace(record.names[i], schema.names.size());
      if (inserted.second) {
        if (schema.names.size() == 4096) { throw std::runtime_error("JSONL union column limit exceeded"); }
        schema.names.push_back(record.names[i]);
        schema.types.push_back(record.types[i]);
        schema.nullable.push_back(schema.rows > 0 || record.types[i] == ScalarType::Null);
        schema.temporal_scales.push_back(datetime_scale(record.fields[i]));
      } else {
        schema.temporal_scales[inserted.first->second] = std::max(schema.temporal_scales[inserted.first->second],datetime_scale(record.fields[i]));
        schema.types[inserted.first->second] = widen(schema.types[inserted.first->second], record.types[i]);
        schema.nullable[inserted.first->second] = schema.nullable[inserted.first->second] || record.types[i] == ScalarType::Null;
      }
    }
    const std::set<std::string> present(record.names.begin(), record.names.end());
    for (std::size_t i = 0; i < schema.names.size(); ++i) {
      if (present.find(schema.names[i]) == present.end()) { schema.nullable[i] = true; }
    }
    ++schema.rows;
  }
  return schema;
}
} // namespace oceanbase::sql::file_sql
